#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>

// 记忆压缩：将旧的上下文词序列通过 LLM 压缩为更短的摘要
// 在配置文件中单独配置（llm/memory/），与预测用 LLM 分离
class MemoryCompressor {
 public:
  MemoryCompressor();
  ~MemoryCompressor();

  // 从 weasel 配置加载 llm/memory/ 下的配置
  bool LoadConfig(const std::string& config_name);

  // 是否启用且可用
  bool IsAvailable() const { return m_enabled && !m_api_url.empty(); }

  // 异步压缩：将 words 通过 LLM 压缩为更短的词序列，完成后回调 compressed_words
  // 回调可能在后台线程执行；若失败则 compressed_words 为空
  void CompressAsync(const std::vector<std::wstring>& words,
                     std::function<void(std::vector<std::wstring>)> callback);

 private:
  // 同步执行 HTTP 请求（可被后台线程调用，内部使用一次性连接避免与主线程竞争）
  bool ExecuteRequest(const std::string& url,
                      const std::string& request_body,
                      std::string& response_body);
  static std::vector<std::wstring> ParseResponse(const std::string& json_response);
  // 一次性连接（可在后台线程执行）
  static bool ExecuteRequestOneShot(const std::string& url,
                             const std::string& api_key,
                             const std::string& request_body,
                             std::string& response_body);

  bool m_enabled;
  std::string m_api_url;
  std::string m_api_key;
  std::string m_model;
  int m_max_tokens;
};
