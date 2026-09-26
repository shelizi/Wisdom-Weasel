#include "stdafx.h"
#include "WeaselServerImpl.h"
#include <mutex>
#include <atomic>
#include <Windows.h>
#include <resource.h>
#include <WeaselUtility.h>

namespace weasel {
class PipeServer : public PipeChannel<DWORD, PipeMessage> {
 public:
  using ServerRunner = std::function<void()>;
  using Respond = std::function<void(Msg)>;
  using ServerHandler = std::function<void(PipeMessage, Respond)>;

  PipeServer(std::wstring&& pn_cmd, SECURITY_ATTRIBUTES* s);

 public:
  void Listen(ServerHandler const& handler);
  /* Get a server runner */
  ServerRunner GetServerRunner(ServerHandler const& handler);
  /* Request to stop listening loop */
  void Stop() {
    m_stopping.store(true, std::memory_order_relaxed);
    /* poke the pending ConnectNamedPipe by opening a client once */
    HANDLE h = ::CreateFile(pname.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                            NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
      ::CloseHandle(h);
    }
  }

 private:
  void _ProcessPipeThread(HANDLE pipe, ServerHandler const& handler);
  std::atomic_bool m_stopping{false};
};
}  // namespace weasel

using namespace weasel;

extern CAppModule _Module;

ServerImpl::ServerImpl()
    : m_pRequestHandler(NULL),
      m_darkMode(IsUserDarkMode()),
      channel(std::make_unique<PipeServer>(GetPipeName(), sa.get_attr())) {
  m_hUser32Module = GetModuleHandle(_T("user32.dll"));
}

ServerImpl::~ServerImpl() {
  _Finailize();
}

void ServerImpl::_Finailize() {
  // Idempotent finalize: stop & join listener if still running.
  if (m_listenThread.joinable()) {
    if (channel)
      channel->Stop();
    m_listenThread.join();
  }
  if (IsWindow()) {
    DestroyWindow();
  }
}

LRESULT ServerImpl::OnColorChange(UINT uMsg,
                                  WPARAM wParam,
                                  LPARAM lParam,
                                  BOOL& bHandled) {
  if (IsUserDarkMode() != m_darkMode) {
    m_darkMode = IsUserDarkMode();
    m_pRequestHandler->UpdateColorTheme(m_darkMode);
  }
  return 0;
}

LRESULT ServerImpl::OnCreate(UINT uMsg,
                             WPARAM wParam,
                             LPARAM lParam,
                             BOOL& bHandled) {
  // not neccessary...
  ::SetWindowText(m_hWnd, WEASEL_IPC_WINDOW);
  return 0;
}

LRESULT ServerImpl::OnClose(UINT uMsg,
                            WPARAM wParam,
                            LPARAM lParam,
                            BOOL& bHandled) {
  Stop();
  return 0;
}

LRESULT ServerImpl::OnDestroy(UINT uMsg,
                              WPARAM wParam,
                              LPARAM lParam,
                              BOOL& bHandled) {
  bHandled = FALSE;
  return 1;
}

LRESULT ServerImpl::OnQueryEndSystemSession(UINT uMsg,
                                            WPARAM wParam,
                                            LPARAM lParam,
                                            BOOL& bHandled) {
  return TRUE;
}

LRESULT ServerImpl::OnEndSystemSession(UINT uMsg,
                                       WPARAM wParam,
                                       LPARAM lParam,
                                       BOOL& bHandled) {
  if (m_pRequestHandler) {
    m_pRequestHandler->Finalize();
    m_pRequestHandler = nullptr;
  }
  return 0;
}

LRESULT ServerImpl::OnCommand(UINT uMsg,
                              WPARAM wParam,
                              LPARAM lParam,
                              BOOL& bHandled) {
  UINT uID = LOWORD(wParam);
  switch (uID) {
    case ID_WEASELTRAY_ENABLE_ASCII:
      m_pRequestHandler->SetOption(lParam, "ascii_mode", true);
      return 0;
    case ID_WEASELTRAY_DISABLE_ASCII:
      m_pRequestHandler->SetOption(lParam, "ascii_mode", false);
      return 0;
    default:;
  }

  std::map<UINT, CommandHandler>::iterator it = m_MenuHandlers.find(uID);
  if (it == m_MenuHandlers.end()) {
    bHandled = FALSE;
    return 0;
  }
  it->second();  // execute command
  return 0;
}

DWORD ServerImpl::OnCommand(WEASEL_IPC_COMMAND uMsg,
                            DWORD wParam,
                            DWORD lParam) {
  BOOL handled = TRUE;
  OnCommand(uMsg, wParam, lParam, handled);
  return handled;
}

int ServerImpl::Start() {
  std::wstring instanceName = L"(WEASEL)Furandōru-Sukāretto-";
  instanceName += getUsername();
  HANDLE hMutexOneInstance = ::CreateMutex(NULL, FALSE, instanceName.c_str());
  bool areYouOK = (::GetLastError() == ERROR_ALREADY_EXISTS ||
                   ::GetLastError() == ERROR_ACCESS_DENIED);

  if (areYouOK) {
    return 0;  // assure single instance
  }

  HWND hwnd = Create(NULL);

  return (int)hwnd;
}

int ServerImpl::Stop() {
  // DO NOT exit process or finalize here
  // Let WeaselServer handle this
  PostMessage(WM_QUIT);
  return 0;
}

// ---------------------------------------------------------------------------
// 卡住侦测：所有应用程序的输入法请求都在同一条线程上处理，它一卡住，打字中的应用程序就跟着冻结。
// 监看线程发现一个请求（含等锁的时间）超过 2 秒时，把所有线程当下的调用栈写到
// %TEMP%\rime.weasel\stall.log，方便找出卡在哪里。

#include <dbghelp.h>
#include <tlhelp32.h>
#include <fstream>
#pragma comment(lib, "dbghelp.lib")

namespace {

std::atomic<ULONGLONG> g_request_start{0};  // 0 = 没有进行中的请求
std::atomic<DWORD> g_request_msg{0};
std::atomic<DWORD> g_listener_tid{0};

#ifdef _M_X64
// 只做栈回溯，不配置内存（线程可能停在堆锁里）；读到坏地址就停
int WalkStack(CONTEXT ctx, DWORD64* frames, int max_frames) {
  int n = 0;
  __try {
    while (n < max_frames && ctx.Rip) {
      frames[n++] = ctx.Rip;
      DWORD64 image_base = 0;
      PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image_base, NULL);
      if (!fn) {
        ctx.Rip = *(DWORD64*)ctx.Rsp;
        ctx.Rsp += 8;
      } else {
        PVOID handler_data = NULL;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fn, &ctx, &handler_data,
                         &establisher, NULL);
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return n;
}

void WriteStall(ULONGLONG elapsed, DWORD msg) {
  static bool sym_ready = false;
  const HANDLE process = GetCurrentProcess();
  if (!sym_ready) {
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    sym_ready = !!SymInitialize(process, NULL, TRUE);
  }
  std::ofstream out(WeaselLogPath() / "stall.log", std::ios::app);
  SYSTEMTIME t;
  GetLocalTime(&t);
  char head[160];
  sprintf_s(head, "==== %04d-%02d-%02d %02d:%02d:%02d request msg=%lu stalled %llu ms (listener tid=%lu)",
            t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, msg, elapsed,
            g_listener_tid.load());
  out << head << "\n";

  const DWORD pid = GetCurrentProcessId();
  const DWORD self = GetCurrentThreadId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE)
    return;
  THREADENTRY32 te = {sizeof(te)};
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid || te.th32ThreadID == self)
      continue;
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                           FALSE, te.th32ThreadID);
    if (!th)
      continue;
    CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_FULL;
    DWORD64 frames[48];
    int n = 0;
    if (SuspendThread(th) != (DWORD)-1) {
      if (GetThreadContext(th, &ctx))
        n = WalkStack(ctx, frames, 48);
      ResumeThread(th);
    }
    CloseHandle(th);
    out << "-- thread " << te.th32ThreadID
        << (te.th32ThreadID == g_listener_tid.load() ? " (IPC listener)" : "") << "\n";
    for (int i = 0; i < n; ++i) {
      char buf[sizeof(SYMBOL_INFO) + 256] = {};
      SYMBOL_INFO* sym = (SYMBOL_INFO*)buf;
      sym->SizeOfStruct = sizeof(SYMBOL_INFO);
      sym->MaxNameLen = 255;
      DWORD64 disp = 0;
      IMAGEHLP_MODULE64 mod = {sizeof(mod)};
      const char* mod_name = SymGetModuleInfo64(process, frames[i], &mod) ? mod.ModuleName : "?";
      out << "   " << mod_name << "!";
      if (SymFromAddr(process, frames[i], &disp, sym))
        out << sym->Name << "+0x" << std::hex << disp << std::dec;
      else
        out << "0x" << std::hex << frames[i] << std::dec;
      IMAGEHLP_LINE64 line = {sizeof(line)};
      DWORD line_disp = 0;
      if (SymGetLineFromAddr64(process, frames[i], &line_disp, &line))
        out << "  " << line.FileName << ":" << line.LineNumber;
      out << "\n";
    }
  }
  CloseHandle(snap);
  out << "\n";
}

#else
void WriteStall(ULONGLONG, DWORD) {}
#endif  // _M_X64

void StartStallWatchdog() {
  std::thread([]() {
    ULONGLONG reported = 0;  // 已回报过的请求（以开始时间辨识），同一次卡住只写一次
    for (;;) {
      Sleep(250);
      const ULONGLONG start = g_request_start.load();
      if (!start || start == reported)
        continue;
      const ULONGLONG elapsed = GetTickCount64() - start;
      if (elapsed < 2000)
        continue;
      reported = start;
      WriteStall(elapsed, g_request_msg.load());
    }
  }).detach();
}

}  // namespace

// 所有 IPC 请求都在这把锁下串行处理（librime 与候选窗都不是线程安全的）。
// 后台线程（如 LLM 异步预测完成后刷新候选窗）也必须先取得这把锁，见 weasel::ServerApiMutex()。
std::mutex& weasel::ServerApiMutex() {
  static std::mutex m;
  return m;
}
#define g_api_mutex weasel::ServerApiMutex()

int ServerImpl::Run() {
  // Guard against double Run invocation.
  if (m_listenThread.joinable()) {
    return -1;  // already running
  }

  auto listener = [this](PipeMessage msg, PipeServer::Respond resp) -> void {
    g_listener_tid = GetCurrentThreadId();
    g_request_msg = msg.Msg;
    g_request_start = GetTickCount64();  // 从收到请求开始计时，等锁的时间也算
    {
      std::lock_guard guard(g_api_mutex);
      HandlePipeMessage(msg, resp);
    }
    g_request_start = 0;
  };
  StartStallWatchdog();
  m_listenThread =
      std::thread([this, listener]() { channel->Listen(listener); });

  CMessageLoop theLoop;
  _Module.AddMessageLoop(&theLoop);
  int nRet = theLoop.Run();
  _Module.RemoveMessageLoop();

  // Ensure listener thread is stopped & joined before returning.
  if (channel)
    channel->Stop();
  if (m_listenThread.joinable())
    m_listenThread.join();

  return nRet;
}

DWORD ServerImpl::OnEcho(WEASEL_IPC_COMMAND uMsg, DWORD wParam, DWORD lParam) {
  if (!m_pRequestHandler)
    return 0;
  return m_pRequestHandler->FindSession(lParam);
}

DWORD ServerImpl::OnStartSession(WEASEL_IPC_COMMAND uMsg,
                                 DWORD wParam,
                                 DWORD lParam) {
  if (!m_pRequestHandler)
    return 0;
  return m_pRequestHandler->AddSession(
      reinterpret_cast<LPWSTR>(channel->ReceiveBuffer()),
      [this](std::wstring& msg) -> bool {
        *channel << msg;
        return true;
      });
}

DWORD ServerImpl::OnEndSession(WEASEL_IPC_COMMAND uMsg,
                               DWORD wParam,
                               DWORD lParam) {
  if (!m_pRequestHandler)
    return 0;
  return m_pRequestHandler->RemoveSession(lParam);
}

DWORD ServerImpl::OnKeyEvent(WEASEL_IPC_COMMAND uMsg,
                             DWORD wParam,
                             DWORD lParam) {
  if (!m_pRequestHandler /* || !m_pSharedMemory*/)
    return 0;

  auto eat = [this](std::wstring& msg) -> bool {
    *channel << msg;
    return true;
  };
  return m_pRequestHandler->ProcessKeyEvent(KeyEvent(wParam), lParam, eat);
}

DWORD ServerImpl::OnShutdownServer(WEASEL_IPC_COMMAND uMsg,
                                   DWORD wParam,
                                   DWORD lParam) {
  Stop();
  return 0;
}

DWORD ServerImpl::OnFocusIn(WEASEL_IPC_COMMAND uMsg,
                            DWORD wParam,
                            DWORD lParam) {
  if (!m_pRequestHandler)
    return 0;
  m_pRequestHandler->FocusIn(wParam, lParam);
  return 0;
}

DWORD ServerImpl::OnFocusOut(WEASEL_IPC_COMMAND uMsg,
                             DWORD wParam,
                             DWORD lParam) {
  if (!m_pRequestHandler)
    return 0;
  m_pRequestHandler->FocusOut(wParam, lParam);
  return 0;
}

DWORD ServerImpl::OnUpdateInputPosition(WEASEL_IPC_COMMAND uMsg,
                                        DWORD wParam,
                                        DWORD lParam) {
  if (!m_pRequestHandler)
    return 0;
  /*
   * 移位标志 = 1bit == 0
   * height: 0~127 = 7bit
   * top:-2048~2047 = 12bit（有符号）
   * left:-2048~2047 = 12bit（有符号）
   *
   * 高解析度下：
   * 移位标志 = 1bit == 1
   * height: 0~254 = 7bit（舍弃低1位）
   * top: -4096~4094 = 12bit（有符号，舍弃低1位）
   * left: -4096~4094 = 12bit（有符号，舍弃低1位）
   */
  RECT rc;
  int hi_res = (wParam >> 31) & 0x01;
  rc.left = ((wParam & 0x7ff) - (wParam & 0x800)) << hi_res;
  rc.top = (((wParam >> 12) & 0x7ff) - ((wParam >> 12) & 0x800)) << hi_res;
  const int width = 6;
  int height = ((wParam >> 24) & 0x7f) << hi_res;
  rc.right = rc.left + width;
  rc.bottom = rc.top + height;

  {
    using PPTLPFPMDPI = BOOL(WINAPI*)(HWND, LPPOINT);
    PPTLPFPMDPI PhysicalToLogicalPointForPerMonitorDPI =
        (PPTLPFPMDPI)::GetProcAddress(m_hUser32Module,
                                      "PhysicalToLogicalPointForPerMonitorDPI");
    POINT lt = {rc.left, rc.top};
    POINT rb = {rc.right, rc.bottom};
    PhysicalToLogicalPointForPerMonitorDPI(NULL, &lt);
    PhysicalToLogicalPointForPerMonitorDPI(NULL, &rb);
    rc = {lt.x, lt.y, rb.x, rb.y};
  }

  m_pRequestHandler->UpdateInputPosition(rc, lParam);
  return 0;
}

DWORD ServerImpl::OnStartMaintenance(WEASEL_IPC_COMMAND uMsg,
                                     DWORD wParam,
                                     DWORD lParam) {
  if (m_pRequestHandler)
    m_pRequestHandler->StartMaintenance();
  return 0;
}

DWORD ServerImpl::OnEndMaintenance(WEASEL_IPC_COMMAND uMsg,
                                   DWORD wParam,
                                   DWORD lParam) {
  if (m_pRequestHandler)
    m_pRequestHandler->EndMaintenance();
  return 0;
}

DWORD ServerImpl::OnLLMTest(WEASEL_IPC_COMMAND uMsg, DWORD wParam, DWORD lParam) {
  if (m_pRequestHandler)
    m_pRequestHandler->LLMTestRequest();
  return 0;
}

DWORD ServerImpl::OnPersonal(WEASEL_IPC_COMMAND uMsg, DWORD wParam, DWORD lParam) {
  if (m_pRequestHandler)
    m_pRequestHandler->PersonalCommand(wParam);
  return 0;
}

DWORD ServerImpl::OnCommitComposition(WEASEL_IPC_COMMAND uMsg,
                                      DWORD wParam,
                                      DWORD lParam) {
  if (m_pRequestHandler)
    m_pRequestHandler->CommitComposition(lParam);
  return 0;
}

DWORD ServerImpl::OnClearComposition(WEASEL_IPC_COMMAND uMsg,
                                     DWORD wParam,
                                     DWORD lParam) {
  if (m_pRequestHandler)
    m_pRequestHandler->ClearComposition(lParam);
  return 0;
}

DWORD ServerImpl::OnSelectCandidateOnCurrentPage(WEASEL_IPC_COMMAND uMsg,
                                                 DWORD wParam,
                                                 DWORD lParam) {
  if (m_pRequestHandler)
    m_pRequestHandler->SelectCandidateOnCurrentPage(wParam, lParam);
  return 0;
}

DWORD ServerImpl::OnHighlightCandidateOnCurrentPage(WEASEL_IPC_COMMAND uMsg,
                                                    DWORD wParam,
                                                    DWORD lParam) {
  if (m_pRequestHandler) {
    auto eat = [this](std::wstring& msg) -> bool {
      *channel << msg;
      return true;
    };
    m_pRequestHandler->HighlightCandidateOnCurrentPage(wParam, lParam, eat);
  }
  return 0;
}

DWORD ServerImpl::OnChangePage(WEASEL_IPC_COMMAND uMsg,
                               DWORD wParam,
                               DWORD lParam) {
  if (m_pRequestHandler) {
    auto eat = [this](std::wstring& msg) -> bool {
      *channel << msg;
      return true;
    };
    m_pRequestHandler->ChangePage(wParam, lParam, eat);
  }
  return 0;
}

#define MAP_PIPE_MSG_HANDLE(__msg, __wParam, __lParam) \
  {                                                    \
    auto lParam = __lParam;                            \
    auto wParam = __wParam;                            \
    LRESULT _result = 0;                               \
    switch (__msg) {
#define PIPE_MSG_HANDLE(__msg, __func)       \
  case __msg:                                \
    _result = __func(__msg, wParam, lParam); \
    break;

#define END_MAP_PIPE_MSG_HANDLE(__result) \
  }                                       \
  __result = _result;                     \
  }

template <typename _Resp>
void ServerImpl::HandlePipeMessage(PipeMessage pipe_msg, _Resp resp) {
  DWORD result;

  MAP_PIPE_MSG_HANDLE(pipe_msg.Msg, pipe_msg.wParam, pipe_msg.lParam)
  PIPE_MSG_HANDLE(WEASEL_IPC_ECHO, OnEcho)
  PIPE_MSG_HANDLE(WEASEL_IPC_START_SESSION, OnStartSession)
  PIPE_MSG_HANDLE(WEASEL_IPC_END_SESSION, OnEndSession)
  PIPE_MSG_HANDLE(WEASEL_IPC_PROCESS_KEY_EVENT, OnKeyEvent)
  PIPE_MSG_HANDLE(WEASEL_IPC_SHUTDOWN_SERVER, OnShutdownServer)
  PIPE_MSG_HANDLE(WEASEL_IPC_FOCUS_IN, OnFocusIn)
  PIPE_MSG_HANDLE(WEASEL_IPC_FOCUS_OUT, OnFocusOut)
  PIPE_MSG_HANDLE(WEASEL_IPC_UPDATE_INPUT_POS, OnUpdateInputPosition)
  PIPE_MSG_HANDLE(WEASEL_IPC_START_MAINTENANCE, OnStartMaintenance)
  PIPE_MSG_HANDLE(WEASEL_IPC_END_MAINTENANCE, OnEndMaintenance)
  PIPE_MSG_HANDLE(WEASEL_IPC_COMMIT_COMPOSITION, OnCommitComposition)
  PIPE_MSG_HANDLE(WEASEL_IPC_CLEAR_COMPOSITION, OnClearComposition);
  PIPE_MSG_HANDLE(WEASEL_IPC_SELECT_CANDIDATE_ON_CURRENT_PAGE,
                  OnSelectCandidateOnCurrentPage);
  PIPE_MSG_HANDLE(WEASEL_IPC_HIGHLIGHT_CANDIDATE_ON_CURRENT_PAGE,
                  OnHighlightCandidateOnCurrentPage);
  PIPE_MSG_HANDLE(WEASEL_IPC_CHANGE_PAGE, OnChangePage);
  PIPE_MSG_HANDLE(WEASEL_IPC_TRAY_COMMAND, OnCommand);
  PIPE_MSG_HANDLE(WEASEL_IPC_LLM_TEST, OnLLMTest);
  PIPE_MSG_HANDLE(WEASEL_IPC_PERSONAL, OnPersonal);
  END_MAP_PIPE_MSG_HANDLE(result);

  resp(result);
}

PipeServer::PipeServer(std::wstring&& pn_cmd, SECURITY_ATTRIBUTES* s)
    : PipeChannel(std::move(pn_cmd), s) {}

void PipeServer::Listen(ServerHandler const& handler) {
  for (;;) {
    if (m_stopping.load(std::memory_order_relaxed))
      break;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    try {
      pipe = _ConnectServerPipe(pname);
      /* Serve this connection in a detached std::thread.
         Capture handler by value to avoid dangling reference after Listen
         returns. */
      std::thread{[handler, pipe, this] {
        _ProcessPipeThread(pipe, handler);
      }}.detach();
    } catch (DWORD ex) {
      _FinalizePipe(pipe);
    }
    if (m_stopping.load(std::memory_order_relaxed))
      break;
  }
}

PipeServer::ServerRunner PipeServer::GetServerRunner(
    ServerHandler const& handler) {
  return [&handler, this]() { Listen(handler); };
}

void PipeServer::_ProcessPipeThread(HANDLE pipe, ServerHandler const& handler) {
  try {
    for (;;) {
      Res msg;
      _Receive(pipe, &msg, sizeof(msg));
      handler(msg, [this, pipe](Msg resp) { _Send(pipe, resp); });
    }
  } catch (...) {
    _FinalizePipe(pipe);
  }
}

// weasel::Server

Server::Server() : m_pImpl(new ServerImpl) {}

Server::~Server() {
  if (m_pImpl)
    delete m_pImpl;
}

int Server::Start() {
  return m_pImpl->Start();
}

int Server::Stop() {
  return m_pImpl->Stop();
}

int Server::Run() {
  return m_pImpl->Run();
}

void Server::SetRequestHandler(RequestHandler* pHandler) {
  m_pImpl->SetRequestHandler(pHandler);
}

void Server::AddMenuHandler(UINT uID, CommandHandler handler) {
  m_pImpl->AddMenuHandler(uID, handler);
}

HWND Server::GetHWnd() {
  return m_pImpl->m_hWnd;
}
