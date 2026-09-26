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

// 前向声明
class ContextHistory;
class PersonalLexicon;
class PersonalRefiner;
class DevConsole;
class LLMProvider;

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

struct SessionStatus {
  SessionStatus() : style(weasel::UIStyle()), __synced(false), session_id(0) {
    RIME_STRUCT(RimeStatus, status);
  }
  weasel::UIStyle style;
  RimeStatus status;
  bool __synced;
  RimeSessionId session_id;
  // last fully converted preview (preedit_type: preview), one unit per
  // syllable, to keep showing the text after the caret while selecting
  std::string preview_input;
  std::vector<std::wstring> preview_units;
  // 中英混打：組字中按 Shift 切到英文後，已轉好的中文與打的英文暫存在這裡，
  // 顯示在組字區最前面，Enter 一起送出（沒在組字時切英文照舊直接輸出）
  std::wstring mixed_text;
  bool mixed_english = false;  // 正在混打的英文段
  std::wstring mixed_commit;   // 待送出的混打內容
  bool mixed_active() const { return mixed_english || !mixed_text.empty(); }
  // 注音逐字選字（像新注音）：←/→ 框住的字（音節序號），-1 表示沒有框選
  int focus = -1;
  std::string focus_input;  // 框選時的輸入與游標，改變了就取消框選
  size_t focus_caret = 0;
};
typedef std::map<DWORD, SessionStatus> SessionStatusMap;
typedef DWORD WeaselSessionId;
class RimeWithWeaselHandler : public weasel::RequestHandler {
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
  // 中英混打：處理 Shift 切換與混打中的按鍵，已處理時回傳 true
  bool _HandleMixedInput(const weasel::KeyEvent& keyEvent, WeaselSessionId ipc_id, EatLine eat);
  // 送出目前的組字並取回轉換好的文字（不交給應用程式）
  std::wstring _TakeComposition(RimeSessionId session_id);
  // 注音逐字選字：←/→/Home/End 移動框選
  bool _HandleZhuyinFocus(const weasel::KeyEvent& keyEvent, WeaselSessionId ipc_id, EatLine eat);
  bool _FocusSyllable(WeaselSessionId ipc_id, int index);
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

  // LLM相关（上下文统一从 m_context_history 获取，不再单独维护 buffer）
  std::unique_ptr<LLMProvider> m_llm_provider;
  bool m_llm_prediction_mode;
  std::vector<std::wstring> m_current_llm_candidates;
  std::wstring m_pending_llm_commit;  // 待提交的LLM候选词
  bool m_mixed_shift_tap = false;     // Shift 按下後還沒按其他鍵（放開時算一次切換）
  bool m_llm_completion_active = false;  // 当前 LLM 候选是输入中补全（Rime 首选 + 续写）
  bool m_llm_server_ui_shown = false;  // TSF 下为显示异步 LLM 结果而弹出的服务端候选窗是否在显示
  bool m_llm_after_commit = true;   // llm/predict_after_commit：送出後預測下一個詞
  bool m_llm_while_typing = true;   // llm/predict_while_typing：打字停頓時自動補完
  bool m_typo_llm_on = false;       // llm/typo/llm：LLM 整句校正（Rime 容錯由注音方案處理）
  size_t m_llm_correction_count = 0;  // m_current_llm_candidates 開頭幾個是整句校正（m_llm_mutex 保護）
  // 注音整句校正用的模型（llm/typo/*），與智慧預測分開；同一個模型時直接共用 m_llm_provider
  std::unique_ptr<LLMProvider> m_typo_owned;
  std::wstring m_typo_prompt;        // llm/typo/prompt：自訂校正指令（m_llm_mutex 保護）
  LLMProvider* m_typo_llm = nullptr;  // m_llm_infer_mutex 下使用
  bool _TypoLLMAvailable() const;
  void _LoadTypoProvider(RimeConfig* config);
  size_t m_llm_context_max_chars = 100;       // llm/context/max_chars：给模型的前文最多几个字
  unsigned m_llm_context_idle_minutes = 10;   // llm/context/idle_minutes：窗口闲置多久后旧前文失效
  void _UpdateContextKey(WeaselSessionId ipc_id);  // 依前景窗口切换上下文
  std::atomic<uint64_t> m_llm_request_seq{0};  // LLM异步预测请求序号（用于丢弃旧结果）
  std::mutex m_llm_mutex;                      // 保护 m_current_llm_candidates
  std::mutex m_llm_infer_mutex;  // 串行化 LLM 推理：llama.cpp 的 context 不能被多个线程同时使用
  std::wstring m_llm_loaded_model;  // 目前载入的模型（设定画面显示用；在 m_llm_infer_mutex 下读写）
  std::unique_ptr<PersonalLexicon> m_personal;  // 個人詞庫（llm/personal/enabled）
  std::unique_ptr<PersonalRefiner> m_refiner;   // 個人詞庫定時精煉
  size_t m_personal_max = 3;                    // 候選中最多幾個來自個人詞庫
  bool m_llm_enabled = false;                   // llm/enabled：所有預測候選的總開關
  bool _PredictionAvailable() const;            // LLM 或個人詞庫至少一個可用
  
  // 双击·键检测（用于清空上下文）
  DWORD m_last_grave_key_time;  // 上次·键按下的时间（毫秒）
  static const DWORD GRAVE_DOUBLE_CLICK_TIMEOUT = 500;  // 双击时间间隔阈值（毫秒）
  static const DWORD kLLMCompletionDelayMs = 300;  // 输入中停顿多久触发补全预测（毫秒）

  // LLM预测相关方法
  // delay_ms>0 时为防抖：延迟后若已有更新的请求则放弃；completion_prefix 非空时为输入中补全模式
  // zhuyin 非空时先请 LLM 校正整句（注音打错字）；complete=false 时只校正、不续写
  void _TriggerLLMPrediction(WeaselSessionId ipc_id, const std::wstring& current_input = L"",
                             DWORD delay_ms = 0, const std::wstring& completion_prefix = L"",
                             const std::wstring& zhuyin = L"", bool complete = true);
  // 目前组字的注音（大千键位的按键转回注音符号）；不是注音方案时回传空字串
  std::wstring _ComposingZhuyin(WeaselSessionId ipc_id);
  // 选中第 llm_index 个 LLM 候选：清空 composition、提交并继续预测下一个词
  bool _CommitLLMCandidate(WeaselSessionId ipc_id, size_t llm_index, EatLine eat);
  // 输入中补全：安排（防抖）或清除补全候选
  void _ScheduleLLMCompletion(WeaselSessionId ipc_id, DWORD delay_ms);
  void _CancelLLMCompletion();
  void _ExitLLMPredictionMode(WeaselSessionId ipc_id);
};
