#include "stdafx.h"
#include "MemoryCompressor.h"
#include "DevConsole.h"
#include <WeaselUtility.h>
#include <rime_api.h>
#include "../core/net/http.h"
#include <sstream>
#include <future>

namespace {

std::wstring WordsToSpaceSeparated(const std::vector<std::wstring>& words) {
  std::wstringstream ss;
  for (size_t i = 0; i < words.size(); ++i) {
    if (i > 0) ss << L" ";
    ss << words[i];
  }
  return ss.str();
}

}  // namespace

MemoryCompressor::MemoryCompressor()
    : m_enabled(false),
      m_max_tokens(100) {}

MemoryCompressor::~MemoryCompressor() = default;

bool MemoryCompressor::LoadConfig(const std::string& config_name) {
  extern DevConsole* g_dev_console;

  RimeApi* rime_api = rime_get_api();
  if (!rime_api) {
    if (g_dev_console && g_dev_console->IsEnabled()) {
      g_dev_console->WriteLine(L"[记忆压缩] LoadConfig失败: rime_api未初始化");
    }
    return false;
  }

  RimeConfig config = {NULL};
  if (!rime_api->config_open(config_name.c_str(), &config)) {
    if (g_dev_console && g_dev_console->IsEnabled()) {
      g_dev_console->WriteLine(L"[记忆压缩] 无法打开配置文件，记忆压缩未启用");
    }
    return false;
  }

  const int BUF_SIZE = 512;
  char buffer[BUF_SIZE + 1] = {0};

  Bool enabled = false;
  bool found = rime_api->config_get_bool(&config, "llm/memory/enabled", &enabled);
  m_enabled = found && !!enabled;

  if (!m_enabled) {
    rime_api->config_close(&config);
    if (g_dev_console && g_dev_console->IsEnabled()) {
      g_dev_console->WriteLine(L"[记忆压缩] 未启用（在 weasel.yaml 中设置 llm/memory/enabled: true 并配置 api_url 以启用）");
    }
    return true;  // 未启用也算加载成功
  }

  found = rime_api->config_get_string(&config, "llm/memory/api_url", buffer, BUF_SIZE);
  if (found) {
    m_api_url = buffer;
  } else {
    m_api_url.clear();
    if (g_dev_console && g_dev_console->IsEnabled()) {
      g_dev_console->WriteLine(L"[记忆压缩] 未配置 llm/memory/api_url，记忆压缩不可用");
    }
  }

  found = rime_api->config_get_string(&config, "llm/memory/api_key", buffer, BUF_SIZE);
  m_api_key = found ? buffer : "";

  found = rime_api->config_get_string(&config, "llm/memory/model", buffer, BUF_SIZE);
  m_model = found ? buffer : "gpt-3.5-turbo";

  int max_tokens = 100;
  if (rime_api->config_get_int(&config, "llm/memory/max_tokens", &max_tokens)) {
    m_max_tokens = max_tokens;
  }

  rime_api->config_close(&config);

  if (g_dev_console && g_dev_console->IsEnabled() && IsAvailable()) {
    g_dev_console->WriteLine(L"[记忆压缩] 已启用，api_url = " + u8tow(m_api_url));
  }
  return true;
}

void MemoryCompressor::CompressAsync(
    const std::vector<std::wstring>& words,
    std::function<void(std::vector<std::wstring>)> callback) {
  if (!IsAvailable() || words.empty()) {
    if (callback) callback(std::vector<std::wstring>());
    return;
  }

  std::wstring words_str = WordsToSpaceSeparated(words);
  std::string prompt_utf8 = wtou8(
      L"请将以下用户输入历史的词序列压缩为更短的摘要，保留关键信息。"
      L"只输出压缩后的词，词语间用单个空格分隔，不要任何解释或标点，不超过10个词。\n\n词序列：\"" +
      words_str + L"\"");

  std::string escaped_prompt;
  for (char c : prompt_utf8) {
    if (c == '"')
      escaped_prompt += "\\\"";
    else if (c == '\\')
      escaped_prompt += "\\\\";
    else if (c == '\n')
      escaped_prompt += "\\n";
    else if (c == '\r')
      escaped_prompt += "\\r";
    else if (c == '\t')
      escaped_prompt += "\\t";
    else
      escaped_prompt += c;
  }

  std::ostringstream json;
  json << "{\"model\":\"" << m_model << "\","
       << "\"messages\":[{\"role\":\"user\",\"content\":\"" << escaped_prompt << "\"}],"
       << "\"max_tokens\":" << m_max_tokens << "}";
  std::string request_body = json.str();
  std::string api_url = m_api_url;
  std::string api_key = m_api_key;

  std::async(std::launch::async, [this, request_body, api_url, api_key, callback]() {
    std::string response_body;
    if (!ExecuteRequestOneShot(api_url, api_key, request_body, response_body)) {
      if (callback) callback(std::vector<std::wstring>());
      return;
    }
    std::vector<std::wstring> result = ParseResponse(response_body);
    if (callback) callback(result);
  });
}

bool MemoryCompressor::ExecuteRequest(const std::string& url,
                                      const std::string& request_body,
                                      std::string& response_body) {
  return ExecuteRequestOneShot(url, m_api_key, request_body, response_body);
}

bool MemoryCompressor::ExecuteRequestOneShot(const std::string& url,
                                             const std::string& api_key,
                                             const std::string& request_body,
                                             std::string& response_body) {
  net::Request request;
  request.method = "POST";
  request.url = url;
  request.headers.push_back({"Content-Type", "application/json"});
  if (!api_key.empty())
    request.headers.push_back({"Authorization", "Bearer " + api_key});
  request.body = request_body;
  request.connect_timeout_ms = request.receive_timeout_ms = 15000;
  net::Response response;
  std::string error;
  const bool ok = net::Fetch(request, &response, &error);
  response_body = response.body;
  return ok && !response_body.empty();
}

std::vector<std::wstring> MemoryCompressor::ParseResponse(const std::string& json_response) {
  std::vector<std::wstring> words;
  size_t content_pos = json_response.find("\"content\"");
  if (content_pos == std::string::npos) return words;
  size_t colon_pos = json_response.find(':', content_pos);
  if (colon_pos == std::string::npos) return words;
  size_t quote_start = json_response.find('"', colon_pos);
  if (quote_start == std::string::npos) return words;
  size_t quote_end = json_response.find('"', quote_start + 1);
  if (quote_end == std::string::npos) return words;
  std::string content = json_response.substr(quote_start + 1, quote_end - quote_start - 1);
  std::wstring content_w = u8tow(content);
  std::wstringstream ss(content_w);
  std::wstring word;
  while (ss >> word) {
    if (!word.empty()) words.push_back(word);
  }
  return words;
}
