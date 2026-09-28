#pragma once
#include <WeaselIPC.h>
#include <WeaselUI.h>
#include <map>
#include <string>
#include <thread>
#include <memory>
#include <atomic>
#include <mutex>

#include <rime_api.h>

#include "../core/ime/controller.h"

// 前向声明
class ContextHistory;
class DevConsole;

class ScopedThread {
 public:
  template <typename Function>
  ScopedThread(Function&& f) : thread(std::forward<Function>(f)) {}
  ~ScopedThread() {
    if (thread.joinable())
      thread.join();
  }
  ScopedThread(const ScopedThread&) = delete;
  ScopedThread& operator=(const ScopedThread&) = delete;

 private:
  std::thread thread;
};

struct CaseInsensitiveCompare {
  bool operator()(const std::string& str1, const std::string& str2) const {
    std::string str1Lower, str2Lower;
    std::transform(str1.begin(), str1.end(), std::back_inserter(str1Lower),
                   [](char c) { return std::tolower(c); });
    std::transform(str2.begin(), str2.end(), std::back_inserter(str2Lower),
                   [](char c) { return std::tolower(c); });
    return str1Lower < str2Lower;
  }
};

typedef std::map<std::string, bool> AppOptions;
typedef std::map<std::string, AppOptions, CaseInsensitiveCompare>
    AppOptionsByAppName;

// Rime 之外的輸入法狀態在
// ime::SessionState（core/ime/session_state.h），這裡只多了 Weasel 的介面狀態
struct SessionStatus : ime::SessionState {
  SessionStatus() : style(weasel::UIStyle()), __synced(false) {
    RIME_STRUCT(RimeStatus, status);
  }
  weasel::UIStyle style;
  RimeStatus status;
  bool __synced;
};
typedef std::map<DWORD, SessionStatus> SessionStatusMap;
typedef DWORD WeaselSessionId;
class RimeWithWeaselHandler : public weasel::RequestHandler,
                              private ime::Frontend {
 public:
  RimeWithWeaselHandler(weasel::UI* ui);
  virtual ~RimeWithWeaselHandler();
  virtual void Initialize();
  virtual void Finalize();
  virtual DWORD FindSession(WeaselSessionId ipc_id);
  virtual DWORD AddSession(LPWSTR buffer, EatLine eat = 0);
  virtual DWORD RemoveSession(WeaselSessionId ipc_id);
  virtual BOOL ProcessKeyEvent(weasel::KeyEvent keyEvent,
                               WeaselSessionId ipc_id,
                               EatLine eat);
  virtual void CommitComposition(WeaselSessionId ipc_id);
  virtual void ClearComposition(WeaselSessionId ipc_id);
  virtual void SelectCandidateOnCurrentPage(size_t index,
                                            WeaselSessionId ipc_id);
  virtual bool HighlightCandidateOnCurrentPage(size_t index,
                                               WeaselSessionId ipc_id,
                                               EatLine eat);
  virtual bool ChangePage(bool backward, WeaselSessionId ipc_id, EatLine eat);
  virtual void FocusIn(DWORD param, WeaselSessionId ipc_id);
  virtual void FocusOut(DWORD param, WeaselSessionId ipc_id);
  virtual void UpdateInputPosition(RECT const& rc, WeaselSessionId ipc_id);
  virtual void StartMaintenance();
  virtual void EndMaintenance();
  virtual void LLMTestRequest();
  virtual void PersonalCommand(DWORD command);
  virtual void SetOption(WeaselSessionId ipc_id,
                         const std::string& opt,
                         bool val);
  virtual void UpdateColorTheme(BOOL darkMode);

  void OnUpdateUI(std::function<void()> const& cb);

  // 设置上下文历史记录实例
  void SetContextHistory(ContextHistory* context_history);

  // 设置开发终端实例
  void SetDevConsole(DevConsole* dev_console);

  // 获取上下文历史记录（供LLM使用）
  ContextHistory* GetContextHistory() const { return m_context_history; }

 private:
  void _Setup();
  bool _IsDeployerRunning();
  void _UpdateUI(WeaselSessionId ipc_id);
  void _LoadSchemaSpecificSettings(WeaselSessionId ipc_id,
                                   const std::string& schema_id);
  void _LoadAppInlinePreeditSet(WeaselSessionId ipc_id,
                                bool ignore_app_name = false);
  bool _ShowMessage(weasel::Context& ctx, weasel::Status& status);
  bool _Respond(WeaselSessionId ipc_id, EatLine eat);
  void _ReadClientInfo(WeaselSessionId ipc_id, LPWSTR buffer);
  void _GetCandidateInfo(weasel::CandidateInfo& cinfo, RimeContext& ctx);
  void _GetStatus(weasel::Status& stat,
                  WeaselSessionId ipc_id,
                  weasel::Context& ctx);
  void _GetContext(weasel::Context& ctx, RimeSessionId session_id);
  void _UpdateShowNotifications(RimeConfig* config, bool initialize = false);

  bool _IsSessionTSF(RimeSessionId session_id);
  void _UpdateInlinePreeditStatus(WeaselSessionId ipc_id);

  RimeSessionId to_session_id(WeaselSessionId ipc_id) {
    return m_session_status_map[ipc_id].session_id;
  }
  SessionStatus& get_session_status(WeaselSessionId ipc_id) {
    return m_session_status_map[ipc_id];
  }
  SessionStatus& new_session_status(WeaselSessionId ipc_id) {
    return m_session_status_map[ipc_id] = SessionStatus();
  }

  AppOptionsByAppName m_app_options;
  weasel::UI* m_ui;  // reference
  DWORD m_active_session;
  bool m_disabled;
  std::string m_last_schema_id;
  std::string m_last_app_name;
  weasel::UIStyle m_base_style;
  std::map<std::string, bool> m_show_notifications;
  std::map<std::string, bool> m_show_notifications_base;
  std::function<void()> _UpdateUICallback;

  static void OnNotify(void* context_object,
                       uintptr_t session_id,
                       const char* message_type,
                       const char* message_value);
  static std::string m_message_type;
  static std::string m_message_value;
  static std::string m_message_label;
  static std::string m_option_name;
  SessionStatusMap m_session_status_map;
  bool m_current_dark_mode;
  bool m_global_ascii_mode;
  int m_show_notifications_time;
  DWORD m_pid;

  // 上下文历史记录和开发终端
  ContextHistory* m_context_history;
  DevConsole* m_dev_console;

  // 輸入法行為（混打、逐字選字、LLM 預測、個人詞庫、選字統計）在 core/ime
  // 的控制器
  std::unique_ptr<ime::Controller> m_controller;
  bool m_llm_server_ui_shown =
      false;  // TSF 下为显示异步 LLM 结果而弹出的服务端候选窗是否在显示

  // ime::Frontend：控制器用到的 Weasel 功能
  ime::SessionState* Session(uint64_t id) override;
  void Refresh(uint64_t id) override;
  void HideCandidates() override;
  std::mutex& ApiMutex() override;
  std::wstring ContextKey(uint64_t id) override;  // 應用程式＋前景視窗＋標題
  void Redeploy() override;
  bool ReleaseSessions() override;
};
