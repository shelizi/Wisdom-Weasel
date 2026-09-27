#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <functional>

// 推論中途取消：呼叫端在工作執行緒上設定檢查函式（例如使用者又打了字，請求已過時），
// 本機模型每產生一個 token 檢查一次，過時就停止，讓新的請求盡快開始
inline std::function<bool()>& LLMCancelCheck() {
  thread_local std::function<bool()> check;
  return check;
}

inline bool LLMCancelled() {
  const auto& check = LLMCancelCheck();
  return check && check();
}

struct LLMCancelScope {
  explicit LLMCancelScope(std::function<bool()> check) { LLMCancelCheck() = std::move(check); }
  ~LLMCancelScope() { LLMCancelCheck() = nullptr; }
  LLMCancelScope(const LLMCancelScope&) = delete;
  LLMCancelScope& operator=(const LLMCancelScope&) = delete;
};

// LLM提供者抽象基类
class LLMProvider {
 public:
  virtual ~LLMProvider() = default;

  // 从配置文件加载配置
  virtual bool LoadConfig(const std::string& config_name) = 0;

  // 预测候选词
  // context: 历史上下文
  // current_input: 当前输入（可为空）
  // max_candidates: 最大候选词数量
  virtual std::vector<std::wstring> PredictCandidates(
      const std::wstring& context,
      const std::wstring& current_input,
      size_t max_candidates) = 0;

  // 注音整句校正：zhuyin 是使用者打的注音（可能有打錯的鍵），draft 是 Rime 轉出的整句。
  // instruction 是使用者自訂的校正指令（llm/typo/prompt），空字串時用預設。
  // 回傳推測的正確句子；不支援或失敗時回傳空字串
  virtual std::wstring CorrectSentence(const std::wstring& context,
                                       const std::wstring& zhuyin,
                                       const std::wstring& draft,
                                       const std::wstring& instruction) {
    return std::wstring();
  }

  // 文字接在前文後面的機率（log，自然對數）：total 為整段總和，per_char 為每個字分到的值。
  // 用來比較同音字的句子哪個通順；不支援（例如 API 模型）時回傳 false
  virtual bool ScoreText(const std::wstring& context, const std::wstring& text, double* total,
                         std::vector<double>* per_char) {
    return false;
  }

  // 检查LLM是否可用
  virtual bool IsAvailable() const = 0;

  // 获取提供者名称
  virtual std::string GetProviderName() const = 0;
};

// ---------------------------------------------------------------------------
// 提示詞（llm/prompt）：Base 與 Instruct / OpenAI 共用同一段使用者可編輯的文字。
// - Base：提示詞 + 前文，讓模型續寫
// - Instruct / OpenAI：提示詞在前當作風格指引，後面接程式需要的任務說明
// 舊設定 llm/llamacpp/prompt_prefix 仍可讀取。

// 去掉首尾空白與換行
inline std::wstring LLMTrimPrompt(const std::wstring& text) {
  const wchar_t* ws = L" \t\r\n　";
  const size_t b = text.find_first_not_of(ws);
  if (b == std::wstring::npos)
    return std::wstring();
  return text.substr(b, text.find_last_not_of(ws) - b + 1);
}

// Base：接在前文前面的引導文字（以空行與前文隔開）
inline std::wstring LLMBasePrefix(const std::wstring& prompt) {
  const std::wstring p = LLMTrimPrompt(prompt);
  return p.empty() ? std::wstring() : p + L"\n\n";
}

// Instruct / OpenAI：system 指令 = 提示詞 + 任務說明
inline std::wstring LLMInstructSystem(const std::wstring& prompt, size_t max_candidates) {
  const std::wstring p = LLMTrimPrompt(prompt);
  const std::wstring n = std::to_wstring(max_candidates);
  return (p.empty() ? std::wstring() : p + L"\n\n") +
         L"你是中文輸入法的候選詞預測器。請根據上下文與目前輸入，預測接下來最可能出現的 " + n +
         L" 個詞或短語。\n"
         L"要求：只輸出候選詞本身，候選詞之間以一個空格分隔，依可能性由高到低排列，"
         L"不要編號、解釋或標點，最多 " + n + L" 個。";
}

// Instruct / OpenAI：user 訊息 = 上下文與目前輸入
inline std::wstring LLMInstructUser(const std::wstring& context, const std::wstring& current_input) {
  return L"上下文：「" + context + L"」\n目前輸入：「" + current_input + L"」\n候選詞：";
}

// 注音整句校正（llm/typo/llm）
// 預設的校正指令；設定視窗的「校正提示詞」可以改寫（llm/typo/prompt）
constexpr wchar_t kLLMCorrectInstruction[] =
    L"你是中文注音輸入法的校正器。使用者用注音打了一句話，其中可能有打錯的鍵"
    L"（按到隔壁的鍵、多打或少打一個注音符號、聲調打錯），所以輸入法依注音轉出的初稿可能有錯字。"
    L"請根據前文、注音與初稿，推測使用者真正想打的句子。\n"
    L"要求：只輸出校正後的句子本身，字數與初稿相同或相近，不要解釋、引號或多餘的標點；"
    L"初稿已經正確時，原樣輸出初稿。";

// Instruct / OpenAI：system 指令 = 提示詞 + 校正指令（自訂的取代預設）
inline std::wstring LLMCorrectSystem(const std::wstring& prompt, const std::wstring& instruction) {
  const std::wstring p = LLMTrimPrompt(prompt);
  const std::wstring i = LLMTrimPrompt(instruction);
  return (p.empty() ? std::wstring() : p + L"\n\n") + (i.empty() ? kLLMCorrectInstruction : i);
}

// Instruct / OpenAI：user 訊息
inline std::wstring LLMCorrectUser(const std::wstring& context, const std::wstring& zhuyin,
                                   const std::wstring& draft) {
  return L"前文：「" + context + L"」\n注音：" + zhuyin + L"\n初稿：" + draft + L"\n校正：";
}

// Base：以幾個範例引導續寫，模型接在最後的「校正：」後面輸出一行；
// 自訂的校正指令放在範例說明前面（Base 模型靠範例格式續寫，不取代範例）
inline std::wstring LLMCorrectBasePrompt(const std::wstring& prompt, const std::wstring& instruction,
                                         const std::wstring& context, const std::wstring& zhuyin,
                                         const std::wstring& draft) {
  const std::wstring i = LLMTrimPrompt(instruction);
  return LLMBasePrefix(prompt) + (i.empty() ? std::wstring() : i + L"\n\n") +
         L"以下是注音輸入法的校正紀錄。注音可能有打錯的鍵，初稿是輸入法直接轉出的文字，"
         L"校正是使用者真正要打的句子。\n\n"
         L"前文：\n注音：ㄨㄛˇ ㄐㄧㄣ ㄊㄧㄢ ㄏㄣˇ ㄈㄤˊ\n初稿：我今天很房\n校正：我今天很忙\n\n"
         L"前文：好久不見，\n注音：ㄗㄨㄟˋ ㄐㄧㄣˋ ㄏㄠˇ ㄇㄚ\n初稿：最近好嗎\n校正：最近好嗎\n\n"
         L"前文：\n注音：ㄑㄧㄝˋ ㄒㄧㄝˋ ㄋㄧˇ ㄉㄜ ㄅㄤ ㄇㄤˊ\n初稿：竊謝你的幫忙\n校正：謝謝你的幫忙\n\n"
         L"前文：" + context + L"\n注音：" + zhuyin + L"\n初稿：" + draft + L"\n校正：";
}

// ---------------------------------------------------------------------------
// 思考（推理）模型

// 去掉思考內容，只留結論：移除 <think>…</think>（也認 <thinking>）；
// 只有結束標記（開頭標記在提示裡）時取其後；思考還沒寫完就被截斷時回傳空字串
inline std::wstring LLMStripThinking(const std::wstring& text) {
  std::wstring s = text;
  static const wchar_t* const kTags[][2] = {{L"<think>", L"</think>"}, {L"<thinking>", L"</thinking>"}};
  for (const auto& tags : kTags) {
    const std::wstring open = tags[0], close = tags[1];
    const size_t last_close = s.rfind(close);
    if (last_close != std::wstring::npos) {
      s = s.substr(last_close + close.size());
    } else {
      const size_t o = s.find(open);
      if (o != std::wstring::npos)
        s = s.substr(0, o);
    }
  }
  const wchar_t* ws = L" \t\r\n";
  const size_t b = s.find_first_not_of(ws);
  return b == std::wstring::npos ? std::wstring() : s.substr(b);
}

// 輸出額度：base 是結論本身需要的 token；開啟思考時再加上思考長度上限 think_tokens。
// think_tokens 為 0 表示不限制，回傳 -1（API 不送 max_tokens；本機生成到結束或上下文用完）
inline int LLMTokenBudget(int base, bool thinking, int think_tokens) {
  if (!thinking)
    return base;
  return think_tokens <= 0 ? -1 : base + think_tokens;
}

// OpenAI 相容 API 關閉思考的參數（不含前後逗號）。各家寫法不同，依網址判斷：
// OpenRouter 用 reasoning.enabled；Ollama 用 think；OpenAI 的推理模型用 reasoning_effort；
// 其餘（llama-server、vLLM、LM Studio 等）用 chat_template_kwargs.enable_thinking
std::string LLMDisableThinkingJson(const std::string& api_url, const std::string& model);

// 共用工具（LLMProvider.cpp）
std::string LLMJsonEscape(const std::string& s);
// 從 chat/completions 回應取出第一個 message content（found 表示是否找到）
std::wstring LLMExtractChatContent(const std::string& json_response, bool* found);
// 單次 POST JSON；HTTP 2xx 才回傳 true。status_code 可為 nullptr。
// timeout_ms 也是整個請求的總時間上限，超過時 timed_out 設為 true
bool LLMHttpPostJson(const std::string& url, const std::string& api_key,
                     const std::string& body, std::string* response,
                     unsigned long timeout_ms, unsigned long* status_code = nullptr,
                     bool* timed_out = nullptr);
// SSE 串流 POST（"stream":true）：每收到一個 data: 事件就呼叫 on_event（[DONE] 結束），
// on_event 回傳 false 時中斷。idle_ms 內沒有新事件（保持連線的註解不算）就中斷並設 timed_out。
// HTTP 非 2xx 時回傳 false，回應內容放在 error_body
bool LLMHttpPostStream(const std::string& url, const std::string& api_key,
                       const std::string& body,
                       const std::function<bool(const std::string& data)>& on_event,
                       unsigned long idle_ms, unsigned long* status_code = nullptr,
                       bool* timed_out = nullptr, std::string* error_body = nullptr);
// 取出 JSON 裡第一個名為 key 的字串欄位（null 或非字串的會跳過）
std::wstring LLMExtractJsonString(const std::string& json, const char* key, bool* found);

// 本機模型的一次性對話（LlamaCppProvider.cpp）：載入模型 → 以 chat template 生成 → 釋放。
// 給個人詞庫精煉這類偶爾執行、需要較長上下文的工作用，不佔用預測用的模型。
struct LLMLocalModelSpec {
  std::string model_path;
  bool instruct = true;
  int n_ctx = 8192;
  int n_gpu_layers = 0;
  int n_threads = 4;
  bool disable_thinking = false;  // 關閉思考：在 chat template 的生成提示後補上空的思考區塊
  int think_tokens = 2048;        // 開啟思考時的思考長度上限（0 = 不限制）
};
bool LLMLocalChat(const LLMLocalModelSpec& spec, const std::string& system_utf8,
                  const std::string& user_utf8, int max_tokens, std::string* output,
                  std::wstring* error);

// OpenAI兼容接口提供者
class OpenAICompatibleProvider : public LLMProvider {
 public:
  OpenAICompatibleProvider();
  ~OpenAICompatibleProvider() override;

  bool LoadConfig(const std::string& config_name) override;
  std::vector<std::wstring> PredictCandidates(
      const std::wstring& context,
      const std::wstring& current_input,
      size_t max_candidates) override;
  std::wstring CorrectSentence(const std::wstring& context, const std::wstring& zhuyin,
                               const std::wstring& draft,
                               const std::wstring& instruction) override;
  bool IsAvailable() const override;
  std::string GetProviderName() const override { return "OpenAI Compatible"; }
  // 不經 rime 設定（也不看 llm/enabled），直接指定 API（注音校正用）
  void ConfigureDirect(const std::string& api_url, const std::string& api_key,
                       const std::string& model, const std::wstring& prompt,
                       bool disable_thinking, int think_tokens);

 private:
  // chat/completions 的請求內容（system + user 兩則訊息，含 extra_body_json）；max_tokens < 0 時不送
  std::string BuildChatBody(const std::wstring& system, const std::wstring& user, int max_tokens,
                            double temperature) const;
  // 执行HTTP请求
  bool ExecuteRequest(const std::string& url,
                      const std::string& request_body,
                      std::string& response_body);
  // 解析JSON响应
  std::vector<std::wstring> ParseResponse(const std::string& json_response);
  void CloseConnection();  // 关闭并清空复用的 HTTP 连接

  bool m_enabled;
  std::string m_api_url;
  std::string m_api_key;
  std::string m_model;
  int m_max_tokens;
  double m_temperature;
  double m_top_p;
  double m_presence_penalty;
  double m_frequency_penalty;
  bool m_has_seed;
  int m_seed;
  std::string m_extra_body_json;  // 额外透传 JSON（对象字符串）
  std::wstring m_prompt;          // llm/prompt：与 llama.cpp 共用的提示词
  bool m_disable_thinking = false;  // llm/openai/disable_thinking：请求时关闭思考
  int m_think_tokens = 2048;        // llm/openai/think_tokens：开启思考时的思考长度上限（0 = 不限制）
  void* m_hSession;       // HINTERNET，复用的 WinHTTP 会话
  void* m_hConnect;       // HINTERNET，复用的连接
  std::string m_cached_url;  // 当前连接对应的 URL，变化时重建连接
};

// llama.cpp 本地推理提供者
class LlamaCppProvider : public LLMProvider {
 public:
  LlamaCppProvider();
  ~LlamaCppProvider() override;

  bool LoadConfig(const std::string& config_name) override;
  std::vector<std::wstring> PredictCandidates(
      const std::wstring& context,
      const std::wstring& current_input,
      size_t max_candidates) override;
  std::wstring CorrectSentence(const std::wstring& context, const std::wstring& zhuyin,
                               const std::wstring& draft,
                               const std::wstring& instruction) override;
  bool IsAvailable() const override;
  bool ScoreText(const std::wstring& context, const std::wstring& text, double* total,
                 std::vector<double>* per_char) override;
  std::string GetProviderName() const override { return "llama.cpp Local"; }

  // 不經 rime 設定，直接指定模型載入（LLMLocalChat 用）
  bool LoadModelDirect(const LLMLocalModelSpec& spec, double temperature);
  void SetPromptPrefix(const std::wstring& prompt) { m_prompt_prefix = prompt; }
  // 一次對話：Instruct 模型套用模型內建的 chat template，Base 模型用純文字續寫；
  // max_tokens < 0 表示不限制（生成到結束或上下文用完）
  std::string Chat(const std::string& system_utf8, const std::string& user_utf8, int max_tokens);
  // 目前模型可用的上下文長度（token）與文字的 token 數
  int ContextSize() const { return m_ctx_size; }
  // 對話需要的輸出額度：開啟思考時加上思考長度上限（-1 = 不限制）
  int ChatBudget(int base) const { return LLMTokenBudget(base, Thinking(), m_think_tokens); }
  int CountTokens(const std::string& text_utf8) const;

 private:
  // 初始化模型
  bool InitializeModel();
  // 清理资源
  void Cleanup();
  // 生成文本；stop_at_newline 时生成到换行就停（校正只要一行）
  std::string GenerateText(const std::string& prompt, size_t max_tokens,
                           bool stop_at_newline = false);
  // Instruct 模型套用模型内建的 chat template；没有 template 时回传空字串
  std::string ApplyChatTemplate(const std::string& system_utf8, const std::string& user_utf8) const;
  // 丢弃预测用的 system prompt KV 缓存（改跑别的 prompt 前呼叫；下次预测会重新 prefill）
  void DropSystemPromptCache();
  // 批量采样：n_parallel 条序列并行，每条只生成一个词（最多 max_new_tokens 个 token）；与单次生成一样复用 system 的 KV cache。
  // think_budget：思考区块另外可用的 token（0 = 不思考，-1 = 不限制，受上下文大小限制）
  std::vector<std::string> GenerateCandidatesBatch(const std::string& system_prompt_utf8, const std::string& user_prompt_utf8, size_t n_parallel, int max_new_tokens, int think_budget = 0);
  // 预处理并缓存 system prompt 的 KV 状态
  bool PrepareSystemPrompt(const std::string& system_prompt_utf8);

  bool m_enabled;
  std::string m_model_path;      // 模型文件路径
  int m_n_ctx;                    // 上下文大小
  int m_n_gpu_layers;             // GPU层数
  int m_max_tokens;               // 最大生成token数
  double m_temperature;           // 温度参数
  int m_top_k;                    // Top-K 采样
  double m_top_p;                 // Top-P (nucleus) 采样
  double m_repeat_penalty;        // 重复惩罚
  double m_presence_penalty;      // 出现惩罚
  double m_frequency_penalty;     // 频率惩罚
  int m_mirostat;                 // 0=关闭, 1=mirostat v1, 2=mirostat v2
  double m_min_p;                 // 最小概率过滤
  double m_typical_p;             // typical sampling
  int m_n_threads;                // 线程数
  bool m_instruct_model;          // true=Instruct 使用指令 prompt，false=Base 仅用 context 补全
  std::wstring m_prompt_prefix;   // llm/prompt：Base 接在前文前的引导文字，Instruct 放在 system 指令前
  bool m_disable_thinking = false;  // llm/llamacpp/disable_thinking：chat template 后补空的思考区块
  int m_think_tokens = 2048;        // llm/llamacpp/think_tokens：开启思考时的思考长度上限（0 = 不限制）
  // 会思考：Instruct 模型且没有关闭思考（Base 模型只会续写）
  bool Thinking() const { return m_instruct_model && !m_disable_thinking; }

  // llama.cpp 对象（使用前向声明避免包含头文件）
  void* m_model;                  // llama_model*
  void* m_context;                // llama_context*
  void* m_sampler;                 // llama_sampler*
  void* m_memory;                  // llama_memory_t (缓存，避免每次获取)
  void* m_vocab;                   // const llama_vocab* (缓存，避免每次获取)
  int m_ctx_size;                  // 实际上下文大小（缓存，避免每次获取）
  std::string m_system_prompt_utf8;
  std::vector<uint8_t> m_system_state;
  size_t m_system_state_size;
  bool m_system_prompt_ready;
  bool m_model_loaded;             // 模型是否已加载
};

// 本機模型的對話工作階段：載入一次，連續多次對話（個人詞庫分批精煉用）。
// 推理行程裡直接載入模型（LlamaCppProvider.cpp）；輸入法服務裡轉給推理行程（RemoteLLMProvider.cpp）
class LLMLocalChatSession {
 public:
  LLMLocalChatSession();
  ~LLMLocalChatSession();
  bool Open(const LLMLocalModelSpec& spec, std::wstring* error);
  bool Chat(const std::string& system_utf8, const std::string& user_utf8, int max_tokens,
            std::string* output, std::wstring* error);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// HF Constraint 接口提供者（/v1/generate/completions）
// 请求体: {"prompt": "历史上下文", "pinyin_constraints": ["当前输入"]}
class HFConstraintProvider : public LLMProvider {
 public:
  HFConstraintProvider();
  ~HFConstraintProvider() override;  bool LoadConfig(const std::string& config_name) override;
  std::vector<std::wstring> PredictCandidates(
      const std::wstring& context,
      const std::wstring& current_input,
      size_t max_candidates) override;
  bool IsAvailable() const override;
  std::string GetProviderName() const override {
    return "HF Constraint";
  }

 private:
  bool ExecuteRequest(const std::string& url,
                      const std::string& request_body,
                      std::string& response_body);
  std::vector<std::wstring> ParseResponse(const std::string& json_response);
  void CloseConnection();  // 关闭并清空复用的 HTTP 连接

  bool m_enabled;
  std::string m_api_url;  // 默认 http://localhost:8000/v1/generate/completions
  void* m_hSession;       // HINTERNET，复用的 WinHTTP 会话
  void* m_hConnect;       // HINTERNET，复用的连接
  std::string m_cached_url;  // 当前连接对应的 URL，变化时重建连接
};
