#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>

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

// 共用工具（LLMProvider.cpp）
std::string LLMJsonEscape(const std::string& s);
// 從 chat/completions 回應取出第一個 message content（found 表示是否找到）
std::wstring LLMExtractChatContent(const std::string& json_response, bool* found);
// 單次 POST JSON；HTTP 2xx 才回傳 true。status_code 可為 nullptr
bool LLMHttpPostJson(const std::string& url, const std::string& api_key,
                     const std::string& body, std::string* response,
                     unsigned long timeout_ms, unsigned long* status_code = nullptr);

// 本機模型的一次性對話（LlamaCppProvider.cpp）：載入模型 → 以 chat template 生成 → 釋放。
// 給個人詞庫精煉這類偶爾執行、需要較長上下文的工作用，不佔用預測用的模型。
struct LLMLocalModelSpec {
  std::string model_path;
  bool instruct = true;
  int n_ctx = 8192;
  int n_gpu_layers = 0;
  int n_threads = 4;
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
  bool IsAvailable() const override;
  std::string GetProviderName() const override { return "OpenAI Compatible"; }

 private:
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
  bool IsAvailable() const override;
  std::string GetProviderName() const override { return "llama.cpp Local"; }

  // 不經 rime 設定，直接指定模型載入（LLMLocalChat 用）
  bool LoadModelDirect(const LLMLocalModelSpec& spec, double temperature);
  // 一次對話：Instruct 模型套用模型內建的 chat template，Base 模型用純文字續寫
  std::string Chat(const std::string& system_utf8, const std::string& user_utf8, int max_tokens);
  // 目前模型可用的上下文長度（token）與文字的 token 數
  int ContextSize() const { return m_ctx_size; }
  int CountTokens(const std::string& text_utf8) const;

 private:
  // 初始化模型
  bool InitializeModel();
  // 清理资源
  void Cleanup();
  // 生成文本
  std::string GenerateText(const std::string& prompt, size_t max_tokens);
  // 批量采样：n_parallel 条序列并行，每条只生成一个词（最多 max_new_tokens 个 token）；与单次生成一样复用 system 的 KV cache
  std::vector<std::string> GenerateCandidatesBatch(const std::string& system_prompt_utf8, const std::string& user_prompt_utf8, size_t n_parallel, int max_new_tokens);
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

// 本機模型的對話工作階段：載入一次，連續多次對話（個人詞庫分批精煉用）
class LLMLocalChatSession {
 public:
  LLMLocalChatSession();
  ~LLMLocalChatSession();
  bool Open(const LLMLocalModelSpec& spec, std::wstring* error);
  bool Chat(const std::string& system_utf8, const std::string& user_utf8, int max_tokens,
            std::string* output, std::wstring* error);

 private:
  LlamaCppProvider* provider_ = nullptr;
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
