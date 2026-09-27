// Windows：子行程以 --ipc <讀取端> <寫入端> 取得繼承的管道 handle；
// 子行程放進 job，父行程結束（包括當掉）時系統會一併結束它。
#include "../process.h"

#include <windows.h>

#include <cstdlib>
#include <string>

namespace platform {

namespace {

std::wstring Utf8ToWide(const std::string& s) {
  if (s.empty())
    return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
  return w;
}

// 依 CommandLineToArgvW 的規則加引號
void AppendQuoted(std::wstring* cmd, const std::wstring& arg) {
  if (!cmd->empty())
    cmd->push_back(L' ');
  if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
    cmd->append(arg);
    return;
  }
  cmd->push_back(L'"');
  for (auto it = arg.begin();; ++it) {
    size_t backslashes = 0;
    while (it != arg.end() && *it == L'\\') {
      ++it;
      ++backslashes;
    }
    if (it == arg.end()) {
      cmd->append(backslashes * 2, L'\\');
      break;
    }
    if (*it == L'"') {
      cmd->append(backslashes * 2 + 1, L'\\');
    } else {
      cmd->append(backslashes, L'\\');
    }
    cmd->push_back(*it);
  }
  cmd->push_back(L'"');
}

bool ReadHandle(HANDLE h, void* buf, size_t size) {
  char* p = static_cast<char*>(buf);
  while (size > 0) {
    DWORD got = 0;
    if (!ReadFile(h, p, (DWORD)(size < 0x10000000 ? size : 0x10000000), &got, nullptr) || got == 0)
      return false;
    p += got;
    size -= got;
  }
  return true;
}

bool WriteHandle(HANDLE h, const void* buf, size_t size) {
  const char* p = static_cast<const char*>(buf);
  while (size > 0) {
    DWORD put = 0;
    if (!WriteFile(h, p, (DWORD)(size < 0x10000000 ? size : 0x10000000), &put, nullptr) || put == 0)
      return false;
    p += put;
    size -= put;
  }
  return true;
}

class HandlePipe : public Pipe {
 public:
  HandlePipe(HANDLE in, HANDLE out) : in_(in), out_(out) {}
  ~HandlePipe() override {
    CloseHandle(in_);
    CloseHandle(out_);
  }
  bool Read(void* buf, size_t size) override { return ReadHandle(in_, buf, size); }
  bool Write(const void* buf, size_t size) override { return WriteHandle(out_, buf, size); }

 private:
  HANDLE in_;
  HANDLE out_;
};

}  // namespace

struct ChildProcess::Handles {
  HANDLE process = nullptr;
  HANDLE job = nullptr;
  HANDLE read = nullptr;   // 讀子行程的輸出
  HANDLE write = nullptr;  // 寫給子行程
};

ChildProcess::~ChildProcess() {
  if (!h_)
    return;
  Shutdown();
  CloseHandle(h_->read);
  CloseHandle(h_->process);
  if (h_->job)
    CloseHandle(h_->job);
}

void ChildProcess::Shutdown() {
  if (!h_->write)
    return;
  // 先關掉寫入端：子行程讀到結尾就會自行結束；稍等一下，還沒結束就強制結束
  CloseHandle(h_->write);
  h_->write = nullptr;
  if (WaitForSingleObject(h_->process, 500) != WAIT_OBJECT_0)
    TerminateProcess(h_->process, 1);
  WaitForSingleObject(h_->process, 2000);
}

std::unique_ptr<ChildProcess> ChildProcess::Start(const std::filesystem::path& exe,
                                                  const std::vector<std::string>& args) {
  SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
  HANDLE child_in = nullptr, parent_write = nullptr;
  HANDLE parent_read = nullptr, child_out = nullptr;
  if (!CreatePipe(&child_in, &parent_write, &inherit, 0))
    return nullptr;
  if (!CreatePipe(&parent_read, &child_out, &inherit, 0)) {
    CloseHandle(child_in);
    CloseHandle(parent_write);
    return nullptr;
  }
  // 父行程這一端不給子行程繼承
  SetHandleInformation(parent_write, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(parent_read, HANDLE_FLAG_INHERIT, 0);
  HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           &inherit, OPEN_EXISTING, 0, nullptr);

  std::wstring cmd;
  AppendQuoted(&cmd, exe.wstring());
  AppendQuoted(&cmd, L"--ipc");
  AppendQuoted(&cmd, std::to_wstring((unsigned long long)(uintptr_t)child_in));
  AppendQuoted(&cmd, std::to_wstring((unsigned long long)(uintptr_t)child_out));
  for (const auto& arg : args)
    AppendQuoted(&cmd, Utf8ToWide(arg));

  // 只讓子行程繼承這幾個 handle
  HANDLE inherited[] = {child_in, child_out, nul};
  SIZE_T attr_size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
  std::string attr_buf(attr_size, '\0');
  auto attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(&attr_buf[0]);
  InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size);
  UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                            nul == INVALID_HANDLE_VALUE ? sizeof(HANDLE) * 2 : sizeof(inherited),
                            nullptr, nullptr);
  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof(si);
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  si.StartupInfo.hStdInput = si.StartupInfo.hStdOutput = si.StartupInfo.hStdError =
      nul == INVALID_HANDLE_VALUE ? nullptr : nul;
  si.lpAttributeList = attrs;

  PROCESS_INFORMATION pi{};
  const BOOL started =
      CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, TRUE,
                     CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                     exe.parent_path().c_str(), &si.StartupInfo, &pi);
  DeleteProcThreadAttributeList(attrs);
  // 子行程那一端交出去了，自己這邊關掉，子行程結束時讀取才會收到結尾
  CloseHandle(child_in);
  CloseHandle(child_out);
  if (nul != INVALID_HANDLE_VALUE)
    CloseHandle(nul);
  if (!started) {
    CloseHandle(parent_write);
    CloseHandle(parent_read);
    return nullptr;
  }

  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    AssignProcessToJobObject(job, pi.hProcess);
  }
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);

  std::unique_ptr<ChildProcess> child(new ChildProcess());
  child->h_ = std::make_unique<Handles>();
  child->h_->process = pi.hProcess;
  child->h_->job = job;
  child->h_->read = parent_read;
  child->h_->write = parent_write;
  return child;
}

bool ChildProcess::Read(void* buf, size_t size) {
  return ReadHandle(h_->read, buf, size);
}

bool ChildProcess::Write(const void* buf, size_t size) {
  return h_->write && WriteHandle(h_->write, buf, size);
}

void ChildProcess::Kill() {
  TerminateProcess(h_->process, 1);
}

bool ChildProcess::Alive() const {
  return WaitForSingleObject(h_->process, 0) == WAIT_TIMEOUT;
}

std::unique_ptr<Pipe> OpenParentPipe(int argc, char** argv) {
  for (int i = 1; i + 2 < argc; ++i) {
    if (std::string(argv[i]) == "--ipc") {
      HANDLE in = (HANDLE)(uintptr_t)std::strtoull(argv[i + 1], nullptr, 10);
      HANDLE out = (HANDLE)(uintptr_t)std::strtoull(argv[i + 2], nullptr, 10);
      if (!in || !out)
        return nullptr;
      return std::make_unique<HandlePipe>(in, out);
    }
  }
  return nullptr;
}

std::filesystem::path ExecutableDir() {
  wchar_t path[MAX_PATH] = {0};
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  return std::filesystem::path(path).parent_path();
}

}  // namespace platform
