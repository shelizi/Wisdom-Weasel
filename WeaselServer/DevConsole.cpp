#include "stdafx.h"
#include "DevConsole.h"
#include <WeaselUtility.h>
#include <rime_api.h>
#include <io.h>
#include <fcntl.h>
#include <iostream>
#include <sstream>
#include <condition_variable>
#include <deque>
#include <mutex>

struct DevConsole::Queue {
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<std::string> lines;
  size_t bytes = 0;
  size_t dropped = 0;
  bool stop = false;
  HANDLE output = INVALID_HANDLE_VALUE;
};

// 佇列上限：主控台暫停太久時丟掉最舊的內容，不佔用過多記憶體
static const size_t kMaxQueuedBytes = 4 * 1024 * 1024;

void DevConsole::StartWriter() {
  auto queue = std::make_shared<Queue>();
  queue->output = m_hConsoleOutput;
  {
    std::lock_guard<std::mutex> lock(m_queue_mutex);
    m_queue = queue;
  }
  // 執行緒持有佇列的 shared_ptr：關閉時不等它（寫入可能正卡在主控台），直接分離
  std::thread([q = queue]() {
    std::unique_lock<std::mutex> lock(q->mutex);
    for (;;) {
      q->cv.wait(lock, [&] { return q->stop || !q->lines.empty(); });
      if (q->lines.empty() && q->stop)
        return;
      std::string batch;
      if (q->dropped) {
        batch = "[DevConsole] 主控台暫停期間略過 " +
                std::to_string(q->dropped) + " 段記錄\r\n";
        q->dropped = 0;
      }
      while (!q->lines.empty()) {
        batch += q->lines.front();
        q->lines.pop_front();
      }
      q->bytes = 0;
      const HANDLE output = q->output;
      lock.unlock();
      DWORD written = 0;
      WriteFile(output, batch.data(), static_cast<DWORD>(batch.size()),
                &written, NULL);
      lock.lock();
    }
  }).detach();
}

void DevConsole::StopWriter() {
  std::shared_ptr<Queue> queue;
  {
    std::lock_guard<std::mutex> lock(m_queue_mutex);
    queue.swap(m_queue);
  }
  if (!queue)
    return;
  {
    std::lock_guard<std::mutex> lock(queue->mutex);
    queue->stop = true;
  }
  queue->cv.notify_all();
}

DevConsole::DevConsole()
    : m_enabled(false),
      m_console_allocated(false),
      m_hConsoleOutput(INVALID_HANDLE_VALUE),
      m_hConsoleInput(INVALID_HANDLE_VALUE) {}

DevConsole::~DevConsole() {
  Close();
}

bool DevConsole::Initialize() {
  // 从配置文件读取是否启用
  if (!LoadConfig()) {
    // 配置加载失败，可能是rime_api还未初始化或配置文件不存在
    // 静默失败，不启用开发终端
    return false;
  }

  if (!m_enabled) {
    // 配置项不存在或为false，不启用开发终端
    return false;
  }
  m_enabled = false;  // Open 成功才算啟用
  return Open();
}

void DevConsole::Reload() {
  const bool was_enabled = m_enabled;
  if (!LoadConfig())
    return;
  const bool wanted = m_enabled;
  m_enabled = was_enabled;
  if (wanted && !was_enabled)
    Open();
  else if (!wanted && was_enabled)
    Close();
}

bool DevConsole::Open() {
  // 分配控制台窗口
  if (!AllocateConsole()) {
    // AllocConsole失败，可能是GUI应用无法分配控制台
    // 静默失败，不启用开发终端
    return false;
  }

  // 重定向标准输出
  if (!RedirectStdout()) {
    FreeConsole();
    return false;
  }
  StartWriter();
  m_enabled = true;

  // 设置控制台标题
  std::wstring title = L"Weasel 开发终端 - 调试日志";
  SetConsoleTitleW(title.c_str());

  // 设置控制台编码为UTF-8
  SetConsoleOutputCP(65001);  // UTF-8

  // 拿掉關閉按鈕：直接關主控台視窗會結束整個輸入法服務（要關請到設定的「開發」頁）
  if (m_console_allocated) {
    if (HWND window = GetConsoleWindow())
      DeleteMenu(GetSystemMenu(window, FALSE), SC_CLOSE, MF_BYCOMMAND);
  }

  // 输出欢迎信息
  WriteLine("========================================");
  WriteLine("Weasel 开发终端已启动");
  WriteLine("实时显示调试日志输出（在设定的「開發」页关闭）");
  WriteLine("========================================");
  WriteLine("");

  return true;
}

bool DevConsole::LoadConfig() {
  RimeApi* rime_api = rime_get_api();
  if (!rime_api) {
    // rime_api未初始化，默认启用开发终端
    m_enabled = true;
    return true;
  }

  // 尝试打开weasel配置文件
  RimeConfig config = {NULL};
  if (!rime_api->config_open("weasel", &config)) {
    // 配置文件打开失败，默认启用开发终端
    m_enabled = true;
    return true;
  }

  Bool enabled = true;  // 默认值为true
  // 读取 dev_console/enabled 配置项
  // 如果配置项存在且为false，则禁用开发终端
  // 如果配置项不存在，默认启用（enabled保持为true）
  if (rime_api->config_get_bool(&config, "dev_console/enabled", &enabled)) {
    m_enabled = !!enabled;
  } else {
    // 配置项不存在，默认启用
    m_enabled = true;
  }

  rime_api->config_close(&config);
  return true;
}

bool DevConsole::AllocateConsole() {
  // 尝试分配新的控制台
  if (!::AllocConsole()) {
    DWORD error = GetLastError();
    // 如果已经存在控制台（ERROR_ACCESS_DENIED），尝试获取现有控制台
    if (error == ERROR_ACCESS_DENIED) {
      // 尝试获取标准输出句柄
      m_hConsoleOutput = GetStdHandle(STD_OUTPUT_HANDLE);
      if (m_hConsoleOutput != INVALID_HANDLE_VALUE &&
          m_hConsoleOutput != NULL) {
        m_console_allocated = false;  // 使用现有控制台
        return true;
      }
    }
    // AllocConsole失败，可能是GUI应用没有控制台
    // 尝试附加到父进程的控制台（如果是从命令行启动的）
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
      m_console_allocated = false;  // 使用父进程控制台
      m_hConsoleOutput = GetStdHandle(STD_OUTPUT_HANDLE);
      if (m_hConsoleOutput != INVALID_HANDLE_VALUE &&
          m_hConsoleOutput != NULL) {
        return true;
      }
      FreeConsole();  // 附加失败，释放
    }
    return false;
  }

  m_console_allocated = true;
  m_hConsoleOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  m_hConsoleInput = GetStdHandle(STD_INPUT_HANDLE);

  if (m_hConsoleOutput == INVALID_HANDLE_VALUE || m_hConsoleOutput == NULL) {
    FreeConsole();
    return false;
  }

  return true;
}

void DevConsole::FreeConsole() {
  if (m_console_allocated && ::FreeConsole()) {
    m_console_allocated = false;
  }
  m_hConsoleOutput = INVALID_HANDLE_VALUE;
  m_hConsoleInput = INVALID_HANDLE_VALUE;
}

bool DevConsole::RedirectStdout() {
  // 重定向标准输出到控制台
  FILE* fp = nullptr;
  freopen_s(&fp, "CONOUT$", "w", stdout);
  if (!fp) {
    return false;
  }

  // 重定向标准错误输出
  freopen_s(&fp, "CONOUT$", "w", stderr);
  if (!fp) {
    return false;
  }

  // 确保输出缓冲区立即刷新
  std::cout.setf(std::ios::unitbuf);
  std::cerr.setf(std::ios::unitbuf);

  return true;
}

void DevConsole::RestoreStdout() {
  // 恢复标准输出（如果需要的话）
  // 注意：在Windows GUI应用中，通常不需要恢复
}

void DevConsole::Write(const std::string& message) {
  if (!m_enabled || message.empty()) {
    return;
  }
  std::shared_ptr<Queue> queue;
  {
    std::lock_guard<std::mutex> lock(m_queue_mutex);
    queue = m_queue;
  }
  if (!queue)
    return;

  {
    std::lock_guard<std::mutex> lock(queue->mutex);
    queue->lines.push_back(message);
    queue->bytes += message.size();
    while (queue->bytes > kMaxQueuedBytes && queue->lines.size() > 1) {
      queue->bytes -= queue->lines.front().size();
      queue->lines.pop_front();
      ++queue->dropped;
    }
  }
  queue->cv.notify_one();
}

void DevConsole::Close() {
  if (m_enabled) {
    WriteLine("");
    WriteLine("========================================");
    WriteLine("Weasel 开发终端正在关闭...");
    WriteLine("========================================");
  }

  m_enabled = false;
  StopWriter();
  RestoreStdout();
  FreeConsole();
}
