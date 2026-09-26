#include "stdafx.h"
#include "LLMProvider.h"
#include "DevConsole.h"
#include <WeaselUtility.h>
#include <rime_api.h>
#include <winhttp.h>
#include <sstream>
#include <algorithm>
#include <cstdio>
#include <cwctype>

#pragma comment(lib, "winhttp.lib")

OpenAICompatibleProvider::OpenAICompatibleProvider()
    : m_enabled(false),
      m_max_tokens(10),
      m_temperature(0.7),
      m_top_p(1.0),
      m_presence_penalty(0.0),
      m_frequency_penalty(0.0),
      m_has_seed(false),
      m_seed(0),
      m_extra_body_json(""),
      m_hSession(nullptr),
      m_hConnect(nullptr) {}

OpenAICompatibleProvider::~OpenAICompatibleProvider() {
  CloseConnection();
}

void OpenAICompatibleProvider::CloseConnection() {
  if (m_hConnect) {
    WinHttpCloseHandle((HINTERNET)m_hConnect);
    m_hConnect = nullptr;
  }
  if (m_hSession) {
    WinHttpCloseHandle((HINTERNET)m_hSession);
    m_hSession = nullptr;
  }
  m_cached_url.clear();
}

std::string LLMDisableThinkingJson(const std::string& api_url, const std::string& model) {
  std::string url = api_url, name = model;
  std::transform(url.begin(), url.end(), url.begin(), ::tolower);
  std::transform(name.begin(), name.end(), name.begin(), ::tolower);
  if (url.find("openrouter.ai") != std::string::npos)
    return "\"reasoning\":{\"enabled\":false}";
  // DeepSeek 官方 API：chat_template_kwargs 會被無視，要用 thinking.type
  if (url.find("api.deepseek.com") != std::string::npos)
    return "\"thinking\":{\"type\":\"disabled\"}";
  if (url.find(":11434") != std::string::npos || url.find("ollama") != std::string::npos)
    return "\"think\":false";
  if (url.find("api.openai.com") != std::string::npos) {
    // 只有推理模型接受 reasoning_effort，其他模型送了會回 400
    const bool reasoning = name.rfind("gpt-5", 0) == 0 ||
                           (name.size() >= 2 && name[0] == 'o' && isdigit((unsigned char)name[1]));
    return reasoning ? "\"reasoning_effort\":\"minimal\"" : "";
  }
  return "\"chat_template_kwargs\":{\"enable_thinking\":false}";
}

void OpenAICompatibleProvider::ConfigureDirect(const std::string& api_url,
                                               const std::string& api_key,
                                               const std::string& model,
                                               const std::wstring& prompt,
                                               bool disable_thinking, int think_tokens) {
  m_enabled = !api_url.empty();
  m_api_url = api_url;
  m_api_key = api_key;
  m_model = model;
  m_max_tokens = 10;
  m_temperature = 0.0;
  m_top_p = 1.0;
  m_presence_penalty = 0.0;
  m_frequency_penalty = 0.0;
  m_has_seed = false;
  m_seed = 0;
  m_extra_body_json.clear();
  m_prompt = prompt;
  m_disable_thinking = disable_thinking;
  m_think_tokens = think_tokens;
  CloseConnection();
}

bool OpenAICompatibleProvider::LoadConfig(const std::string& config_name) {
  extern DevConsole* g_dev_console;
  
  // 硬编码测试配置（用于测试）
  // 注意：如果设置为 true，将不会读取yaml配置文件
  bool use_hardcoded_config = false;  // 设置为 true 使用硬编码配置，false 使用配置文件
  
  if (use_hardcoded_config) {
    if (g_dev_console && g_dev_console->IsEnabled()) {
      g_dev_console->WriteLine(L"[LLM] 使用硬编码测试配置");
    }
    m_enabled = true;
    m_api_url = "http://localhost:11434/v1/chat/completions";
    m_api_key = "";
    m_model = "qwen3:8b";
    m_max_tokens = 10;
    m_temperature = 0.7;
    m_top_p = 1.0;
    m_presence_penalty = 0.0;
    m_frequency_penalty = 0.0;
    m_has_seed = false;
    m_seed = 0;
    m_extra_body_json.clear();
    
    if (g_dev_console && g_dev_console->IsEnabled()) {
      g_dev_console->WriteLine(L"[LLM] LoadConfig: llm/enabled = true");
      g_dev_console->WriteLine(L"[LLM] LoadConfig: api_url = " + u8tow(m_api_url));
      g_dev_console->WriteLine(L"[LLM] LoadConfig: model = " + u8tow(m_model));
      g_dev_console->WriteLine(L"[LLM] LoadConfig: max_tokens = " + std::to_wstring(m_max_tokens));
      g_dev_console->WriteLine(L"[LLM] LoadConfig: temperature = " + std::to_wstring(m_temperature));
      g_dev_console->WriteLine(L"[LLM] LoadConfig: 配置加载成功（硬编码）");
    }
    CloseConnection();  // URL 可能变化，下次请求时重建连接
    return true;
  }

  RimeApi* rime_api = rime_get_api();
  if (!rime_api) {
    if (g_dev_console && g_dev_console->IsEnabled()) {
      g_dev_console->WriteLine(L"[LLM] LoadConfig失败: rime_api未初始化");
      g_dev_console->WriteLine(L"[LLM] 可能原因: Rime API在LoadConfig之前未正确初始化");
    }
    return false;
  }

  if (g_dev_console && g_dev_console->IsEnabled()) {
    g_dev_console->WriteLine(L"[LLM] 开始从配置文件加载: " + u8tow(config_name));
  }

  RimeConfig config = {NULL};
  if (!rime_api->config_open(config_name.c_str(), &config)) {
    if (g_dev_console && g_dev_console->IsEnabled()) {
      std::wstring config_name_w = u8tow(config_name);
      g_dev_console->WriteLine(L"[LLM] LoadConfig失败: 无法打开配置文件 " + config_name_w);
      g_dev_console->WriteLine(L"[LLM] 可能原因:");
      g_dev_console->WriteLine(L"[LLM]   1. 配置文件不存在: weasel.yaml 或 weasel.custom.yaml");
      g_dev_console->WriteLine(L"[LLM]   2. 配置文件路径错误");
      g_dev_console->WriteLine(L"[LLM]   3. 配置文件格式错误（YAML语法错误）");
      g_dev_console->WriteLine(L"[LLM]   4. Rime未正确初始化");
    }
    return false;
  }

  if (g_dev_console && g_dev_console->IsEnabled()) {
    g_dev_console->WriteLine(L"[LLM] 配置文件打开成功，开始读取配置项");
  }

  // 读取LLM配置
  Bool enabled = false;
  bool found_enabled = rime_api->config_get_bool(&config, "llm/enabled", &enabled);
  
  if (g_dev_console && g_dev_console->IsEnabled()) {
    if (found_enabled) {
      g_dev_console->WriteLine(L"[LLM] 找到配置项 llm/enabled = " + 
                                std::wstring(enabled ? L"true" : L"false"));
    } else {
      g_dev_console->WriteLine(L"[LLM] 未找到配置项 llm/enabled");
      g_dev_console->WriteLine(L"[LLM] 尝试读取的路径: llm/enabled");
    }
  }
  
  if (found_enabled) {
    m_enabled = !!enabled;
  } else {
    m_enabled = false;
    if (g_dev_console && g_dev_console->IsEnabled()) {
      g_dev_console->WriteLine(L"[LLM] LoadConfig失败: 未找到配置项 llm/enabled");
      g_dev_console->WriteLine(L"[LLM] 请在配置文件中添加：");
      g_dev_console->WriteLine(L"[LLM]   llm:");
      g_dev_console->WriteLine(L"[LLM]     enabled: true");
      g_dev_console->WriteLine(L"[LLM] 配置文件位置通常在: %APPDATA%\\Rime\\weasel.yaml");
    }
    rime_api->config_close(&config);
    return false;
  }

  if (!m_enabled) {
    if (g_dev_console && g_dev_console->IsEnabled()) {
      g_dev_console->WriteLine(L"[LLM] LoadConfig失败: llm/enabled 为 false");
      g_dev_console->WriteLine(L"[LLM] 请将配置文件中的 llm/enabled 设置为 true");
    }
    rime_api->config_close(&config);
    return false;
  }

  // 读取OpenAI配置
  const int BUF_SIZE = 512;
  char buffer[BUF_SIZE + 1] = {0};

  bool found_api_url = rime_api->config_get_string(&config, "llm/openai/api_url", buffer, BUF_SIZE);
  if (found_api_url) {
    m_api_url = buffer;
    if (g_dev_console && g_dev_console->IsEnabled()) {
      g_dev_console->WriteLine(L"[LLM] 找到配置项 llm/openai/api_url = " + u8tow(m_api_url));
    }
  } else {
    m_api_url = "https://api.openai.com/v1/chat/completions";
    if (g_dev_console && g_dev_console->IsEnabled()) {
      g_dev_console->WriteLine(L"[LLM] 未找到配置项 llm/openai/api_url，使用默认值 = " + u8tow(m_api_url));
      g_dev_console->WriteLine(L"[LLM] 建议在配置文件中添加: llm/openai/api_url");
    }
  }

  // api_key是可选的，对于本地服务（如Ollama）可以为空
  bool found_api_key = rime_api->config_get_string(&config, "llm/openai/api_key", buffer, BUF_SIZE);
  if (found_api_key) {
    m_api_key = buffer;
    if (g_dev_console && g_dev_console->IsEnabled()) {
      if (m_api_key.empty()) {
        g_dev_console->WriteLine(L"[LLM] 找到配置项 llm/openai/api_key = (空，适用于本地服务如Ollama)");
      } else {
        // 只显示前8个字符，保护隐私
        std::wstring key_preview = m_api_key.length() > 8 
          ? (u8tow(m_api_key.substr(0, 8)) + L"...") 
          : u8tow(m_api_key);
        g_dev_console->WriteLine(L"[LLM] 找到配置项 llm/openai/api_key = " + key_preview);
      }
    }
  } else {
    // api_key未配置，使用空字符串（适用于本地服务）
    m_api_key = "";
    if (g_dev_console && g_dev_console->IsEnabled()) {
      g_dev_console->WriteLine(L"[LLM] 未找到配置项 llm/openai/api_key，使用空字符串（适用于本地服务如Ollama）");
    }
  }

  bool found_model = rime_api->config_get_string(&config, "llm/openai/model", buffer, BUF_SIZE);
  if (found_model) {
    m_model = buffer;
    if (g_dev_console && g_dev_console->IsEnabled()) {
      g_dev_console->WriteLine(L"[LLM] 找到配置项 llm/openai/model = " + u8tow(m_model));
    }
  } else {
    m_model = "gpt-3.5-turbo";
    if (g_dev_console && g_dev_console->IsEnabled()) {
      g_dev_console->WriteLine(L"[LLM] 未找到配置项 llm/openai/model，使用默认值 = " + u8tow(m_model));
    }
  }

  int max_tokens = 10;
  if (rime_api->config_get_int(&config, "llm/openai/max_tokens", &max_tokens)) {
    m_max_tokens = max_tokens;
    if (g_dev_console && g_dev_console->IsEnabled()) {
      g_dev_console->WriteLine(L"[LLM] LoadConfig: max_tokens = " + std::to_wstring(m_max_tokens));
    }
  } else {
    if (g_dev_console && g_dev_console->IsEnabled()) {
      g_dev_console->WriteLine(L"[LLM] LoadConfig: 使用默认 max_tokens = " + std::to_wstring(m_max_tokens));
    }
  }

  // Rime API可能不支持config_get_double，使用字符串读取然后转换
  char temp_str[64] = {0};
  if (rime_api->config_get_string(&config, "llm/openai/temperature",
                                   temp_str, sizeof(temp_str) - 1)) {
    m_temperature = atof(temp_str);
    if (g_dev_console && g_dev_console->IsEnabled()) {
      g_dev_console->WriteLine(L"[LLM] LoadConfig: temperature = " + std::to_wstring(m_temperature));
    }
  } else {
    m_temperature = 0.7;
    if (g_dev_console && g_dev_console->IsEnabled()) {
      g_dev_console->WriteLine(L"[LLM] LoadConfig: 使用默认 temperature = " + std::to_wstring(m_temperature));
    }
  }

  // 读取可选额外参数（有配置则生效，无配置则使用默认值）
  if (rime_api->config_get_string(&config, "llm/openai/top_p", temp_str,
                                  sizeof(temp_str) - 1)) {
    m_top_p = atof(temp_str);
  } else {
    m_top_p = 1.0;
  }

  if (rime_api->config_get_string(&config, "llm/openai/presence_penalty",
                                  temp_str, sizeof(temp_str) - 1)) {
    m_presence_penalty = atof(temp_str);
  } else {
    m_presence_penalty = 0.0;
  }

  if (rime_api->config_get_string(&config, "llm/openai/frequency_penalty",
                                  temp_str, sizeof(temp_str) - 1)) {
    m_frequency_penalty = atof(temp_str);
  } else {
    m_frequency_penalty = 0.0;
  }

  int seed = 0;
  if (rime_api->config_get_int(&config, "llm/openai/seed", &seed)) {
    m_seed = seed;
    m_has_seed = true;
  } else {
    m_seed = 0;
    m_has_seed = false;
  }

  if (g_dev_console && g_dev_console->IsEnabled()) {
    g_dev_console->WriteLine(L"[LLM] LoadConfig: top_p = " +
                             std::to_wstring(m_top_p));
    g_dev_console->WriteLine(L"[LLM] LoadConfig: presence_penalty = " +
                             std::to_wstring(m_presence_penalty));
    g_dev_console->WriteLine(L"[LLM] LoadConfig: frequency_penalty = " +
                             std::to_wstring(m_frequency_penalty));
    g_dev_console->WriteLine(
        L"[LLM] LoadConfig: seed = " +
        std::wstring(m_has_seed ? std::to_wstring(m_seed) : L"(未设置)"));
  }

  // 关闭思考（思考型模型）
  {
    Bool no_think = false;
    m_disable_thinking =
        rime_api->config_get_bool(&config, "llm/openai/disable_thinking", &no_think) && no_think;
    int think_tokens = 2048;
    m_think_tokens = rime_api->config_get_int(&config, "llm/openai/think_tokens", &think_tokens)
                         ? (std::max)(0, think_tokens)
                         : 2048;
  }

  // 任意 JSON 透传（必须是 JSON 对象字符串，如 {"stream":false,"user":"abc"}）
  if (rime_api->config_get_string(&config, "llm/openai/extra_body_json", buffer,
                                  BUF_SIZE)) {
    m_extra_body_json = buffer;
  } else {
    m_extra_body_json.clear();
  }
  if (g_dev_console && g_dev_console->IsEnabled()) {
    g_dev_console->WriteLine(
        L"[LLM] LoadConfig: extra_body_json = " +
        std::wstring(m_extra_body_json.empty() ? L"(空)" : u8tow(m_extra_body_json)));
  }

  // 提示词（与 llama.cpp 共用）：llm/prompt；兼容旧的 llm/llamacpp/prompt_prefix
  {
    char prompt_buf[4096] = {0};
    if (rime_api->config_get_string(&config, "llm/prompt", prompt_buf, sizeof(prompt_buf) - 1) ||
        rime_api->config_get_string(&config, "llm/llamacpp/prompt_prefix", prompt_buf,
                                    sizeof(prompt_buf) - 1))
      m_prompt = u8tow(prompt_buf);
    else
      m_prompt.clear();
  }

  CloseConnection();  // URL 可能变化，下次请求时重建连接
  rime_api->config_close(&config);

  if (g_dev_console && g_dev_console->IsEnabled()) {
    g_dev_console->WriteLine(L"[LLM] LoadConfig: 配置加载成功");
  }
  
  return true;
}

std::vector<std::wstring> OpenAICompatibleProvider::PredictCandidates(
    const std::wstring& context,
    const std::wstring& current_input,
    size_t max_candidates) {
  std::vector<std::wstring> candidates;

  if (!IsAvailable() || context.empty()) {
    return candidates;
  }

  // 构建 prompt：提示词（与 llama.cpp 共用）+ 任务说明放 system，上下文放 user
  extern DevConsole* g_dev_console;
  std::string request_body = BuildChatBody(LLMInstructSystem(m_prompt, max_candidates),
                                           LLMInstructUser(context, current_input),
                                           LLMTokenBudget(m_max_tokens, !m_disable_thinking,
                                                          m_think_tokens),
                                           m_temperature);

  if (g_dev_console && g_dev_console->IsEnabled()) {
    g_dev_console->WriteLine(L"[LLM] 发送预测请求");
    g_dev_console->WriteLine(L"  上下文: " + context);
    g_dev_console->WriteLine(L"  请求URL: " + u8tow(m_api_url));
    g_dev_console->WriteLine(L"  请求体: " + u8tow(request_body));
  }

  // 执行HTTP请求
  std::string response_body;
  if (!ExecuteRequest(m_api_url, request_body, response_body)) {
    if (g_dev_console && g_dev_console->IsEnabled()) {
      g_dev_console->WriteLine(L"[LLM] 请求失败");
    }
    return candidates;
  }

  // 输出响应内容到开发终端
  if (g_dev_console && g_dev_console->IsEnabled()) {
    g_dev_console->WriteLine(L"[LLM] 收到响应");
    g_dev_console->WriteLine(L"  响应内容: " + u8tow(response_body));
  }

  // 解析响应
  candidates = ParseResponse(response_body);

  // if (g_dev_console && g_dev_console->IsEnabled()) {
  //   std::wstringstream ss;
  //   ss << L"[LLM] 解析得到 " << candidates.size() << L" 个候选词";
  //   g_dev_console->WriteLine(ss.str());
  //   for (size_t i = 0; i < candidates.size(); ++i) {
  //     std::wstringstream ss2;
  //     ss2 << L"  " << (i + 1) << L". " << candidates[i];
  //     g_dev_console->WriteLine(ss2.str());
  //   }
  // }

  return candidates;
}

std::string OpenAICompatibleProvider::BuildChatBody(const std::wstring& system,
                                                    const std::wstring& user, int max_tokens,
                                                    double temperature) const {
  auto escape_json = LLMJsonEscape;
  extern DevConsole* g_dev_console;
  std::ostringstream json;
  json << "{"
       << "\"model\":\"" << escape_json(m_model) << "\","
       << "\"messages\":["
       << "{\"role\":\"system\",\"content\":\"" << escape_json(wtou8(system)) << "\"},"
       << "{\"role\":\"user\",\"content\":\"" << escape_json(wtou8(user)) << "\"}"
       << "],";
  if (max_tokens >= 0)
    json << "\"max_tokens\":" << max_tokens << ",";
  json << "\"temperature\":" << temperature;

  json << ",\"top_p\":" << m_top_p
       << ",\"presence_penalty\":" << m_presence_penalty
       << ",\"frequency_penalty\":" << m_frequency_penalty;
  if (m_has_seed) {
    json << ",\"seed\":" << m_seed;
  }
  if (m_disable_thinking) {
    const std::string no_think = LLMDisableThinkingJson(m_api_url, m_model);
    if (!no_think.empty())
      json << "," << no_think;
  }

  // 透传额外 JSON（合并对象内部字段到根对象）
  if (!m_extra_body_json.empty()) {
    size_t start = m_extra_body_json.find_first_not_of(" \t\r\n");
    size_t end = m_extra_body_json.find_last_not_of(" \t\r\n");
    if (start != std::string::npos && end != std::string::npos &&
        m_extra_body_json[start] == '{' && m_extra_body_json[end] == '}') {
      std::string inner =
          m_extra_body_json.substr(start + 1, end - start - 1);
      if (!inner.empty()) {
        json << "," << inner;
      }
    } else {
      if (g_dev_console && g_dev_console->IsEnabled()) {
        g_dev_console->WriteLine(
            L"[LLM] extra_body_json 格式无效，需为 JSON 对象字符串，已忽略");
      }
    }
  }

  json << "}";
  return json.str();
}

std::wstring OpenAICompatibleProvider::CorrectSentence(const std::wstring& context,
                                                       const std::wstring& zhuyin,
                                                       const std::wstring& draft,
                                                       const std::wstring& instruction) {
  if (!IsAvailable() || zhuyin.empty() || draft.empty())
    return L"";
  // 校正要穩定的結果：溫度 0；字數與初稿相近，多留一些 token
  const std::string request_body =
      BuildChatBody(LLMCorrectSystem(m_prompt, instruction), LLMCorrectUser(context, zhuyin, draft),
                    LLMTokenBudget((int)draft.size() * 3 + 16, !m_disable_thinking, m_think_tokens),
                    0.0);
  std::string response_body;
  if (!ExecuteRequest(m_api_url, request_body, response_body))
    return L"";
  bool found = false;
  std::wstring result = LLMExtractChatContent(response_body, &found);
  extern DevConsole* g_dev_console;
  if (g_dev_console && g_dev_console->IsEnabled())
    g_dev_console->WriteLine(L"[LLM] 整句校正 (OpenAI): " + draft + L" → " + result);
  if (!found)
    return L"";
  const size_t b = result.find_first_not_of(L" \t\r\n");
  if (b == std::wstring::npos)
    return L"";
  result = result.substr(b);
  return result.substr(0, result.find_first_of(L"\r\n"));
}

bool OpenAICompatibleProvider::IsAvailable() const {
  // api_key可以为空（适用于本地服务如Ollama），只需要enabled和api_url不为空
  return m_enabled && !m_api_url.empty();
}

bool OpenAICompatibleProvider::ExecuteRequest(const std::string& url,
                                               const std::string& request_body,
                                               std::string& response_body) {
  URL_COMPONENTS url_comp = {0};
  url_comp.dwStructSize = sizeof(URL_COMPONENTS);
  url_comp.dwSchemeLength = (DWORD)-1;
  url_comp.dwHostNameLength = (DWORD)-1;
  url_comp.dwUrlPathLength = (DWORD)-1;
  url_comp.dwExtraInfoLength = (DWORD)-1;

  std::wstring url_w = u8tow(url);
  wchar_t hostname[256] = {0};
  wchar_t path[1024] = {0};
  url_comp.lpszHostName = hostname;
  url_comp.lpszUrlPath = path;

  if (!WinHttpCrackUrl(url_w.c_str(), (DWORD)url_w.length(), 0, &url_comp)) {
    return false;
  }

  INTERNET_PORT port = url_comp.nPort;
  bool use_https = (url_comp.nScheme == INTERNET_SCHEME_HTTPS);
  if (port == 0) {
    port = use_https ? INTERNET_DEFAULT_HTTPS_PORT
                     : INTERNET_DEFAULT_HTTP_PORT;
  }

  std::wstring hostname_str(hostname, url_comp.dwHostNameLength);
  std::wstring path_str(path, url_comp.dwUrlPathLength);

  HINTERNET hSession = (HINTERNET)m_hSession;
  HINTERNET hConnect = (HINTERNET)m_hConnect;

  if (m_cached_url != url || !hSession || !hConnect) {
    CloseConnection();
    bool is_localhost = (hostname_str == L"localhost" || hostname_str == L"127.0.0.1");
    DWORD access_type = is_localhost ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_DEFAULT_PROXY;
    hSession = WinHttpOpen(
        L"Weasel IME/1.0", access_type,
        is_localhost ? (LPCWSTR)WINHTTP_NO_PROXY_NAME : NULL,
        is_localhost ? (LPCWSTR)WINHTTP_NO_PROXY_BYPASS : NULL, 0);
    if (!hSession) {
      return false;
    }
    DWORD timeout = 10000;
    WinHttpSetTimeouts(hSession, timeout, timeout, timeout, timeout);
    hConnect = WinHttpConnect(hSession, hostname_str.c_str(), port, 0);
    if (!hConnect) {
      WinHttpCloseHandle(hSession);
      return false;
    }
    m_hSession = hSession;
    m_hConnect = hConnect;
    m_cached_url = url;
  }

  HINTERNET hRequest = WinHttpOpenRequest(
      hConnect, L"POST", path_str.c_str(), NULL, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES,
      use_https ? WINHTTP_FLAG_SECURE : 0);
  if (!hRequest) {
    CloseConnection();
    return false;
  }

  std::wstring headers = L"Content-Type: application/json\r\n";
  if (!m_api_key.empty()) {
    std::wstring api_key_w = u8tow(m_api_key);
    headers += L"Authorization: Bearer " + api_key_w + L"\r\n";
  }

  if (!WinHttpSendRequest(hRequest, headers.c_str(), (DWORD)-1,
                          (LPVOID)request_body.c_str(),
                          (DWORD)request_body.length(),
                          (DWORD)request_body.length(),
                          0)) {
    WinHttpCloseHandle(hRequest);
    CloseConnection();
    return false;
  }

  if (!WinHttpReceiveResponse(hRequest, NULL)) {
    WinHttpCloseHandle(hRequest);
    CloseConnection();
    return false;
  }

  DWORD bytes_available = 0;
  response_body.clear();
  while (WinHttpQueryDataAvailable(hRequest, &bytes_available) &&
         bytes_available > 0) {
    std::vector<char> buffer(bytes_available);
    DWORD bytes_read = 0;
    if (WinHttpReadData(hRequest, buffer.data(), bytes_available,
                        &bytes_read)) {
      response_body.append(buffer.data(), bytes_read);
    } else {
      break;
    }
  }

  WinHttpCloseHandle(hRequest);
  return !response_body.empty();
}

std::vector<std::wstring> OpenAICompatibleProvider::ParseResponse(
    const std::string& json_response) {
  std::vector<std::wstring> candidates;

  bool found = false;
  const std::wstring content_w = LLMExtractChatContent(json_response, &found);
  if (!found)
    return candidates;

  // 以空白、逗號、頓號、分號分隔；去掉「1.」「2、」之类的编号
  std::wstring word;
  auto flush = [&]() {
    size_t k = 0;
    while (k < word.size() && iswdigit(word[k]))
      ++k;
    if (k > 0 && k < word.size() &&
        (word[k] == L'.' || word[k] == L'、' || word[k] == L')' || word[k] == L'．'))
      word.erase(0, k + 1);
    if (!word.empty())
      candidates.push_back(word);
    word.clear();
  };
  for (wchar_t c : content_w) {
    if (iswspace(c) || c == L',' || c == L'，' || c == L'、' || c == L';' ||
        c == L'；' || c == L'　')
      flush();
    else
      word += c;
  }
  flush();

  return candidates;
}


// ---------------------------------------------------------------------------
// 共用工具（OpenAI 相容 API、個人詞庫精煉）

std::string LLMJsonEscape(const std::string& s) {
  std::string out;
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += (char)c;
        }
    }
  }
  return out;
}

// 在 choices 之后找 "content": "..."（跳过 content 为 null 的情况），
// 并正确解码 JSON 字串（\" \\ \n 以及许多服务用来输出中文的 \uXXXX / 代理对）
std::wstring LLMExtractJsonString(const std::string& json_response, const char* key,
                                  bool* found_out) {
  size_t pos = json_response.find("\"choices\"");
  if (pos == std::string::npos)
    pos = 0;
  const std::string quoted = std::string("\"") + key + "\"";
  std::wstring content_w;
  bool found = false;
  while (!found && (pos = json_response.find(quoted, pos)) != std::string::npos) {
    size_t p = json_response.find(':', pos);
    if (p == std::string::npos)
      break;
    p = json_response.find_first_not_of(" \t\r\n", p + 1);
    if (p == std::string::npos)
      break;
    if (json_response[p] != '"') {  // null 或其他型别
      pos = p;
      continue;
    }
    std::string raw;
    for (size_t i = p + 1; i < json_response.size(); ++i) {
      const char c = json_response[i];
      if (c == '"') {
        found = true;
        break;
      }
      if (c != '\\' || i + 1 >= json_response.size()) {
        raw += c;
        continue;
      }
      const char e = json_response[++i];
      switch (e) {
        case 'n': raw += '\n'; break;
        case 't': raw += '\t'; break;
        case 'r': raw += '\r'; break;
        case 'b': case 'f': break;
        case 'u': {
          auto hex4 = [&](size_t at, unsigned& v) {
            if (at + 4 > json_response.size())
              return false;
            v = (unsigned)strtoul(json_response.substr(at, 4).c_str(), nullptr, 16);
            return true;
          };
          unsigned cp = 0;
          if (!hex4(i + 1, cp))
            break;
          i += 4;
          unsigned lo = 0;
          if (cp >= 0xD800 && cp <= 0xDBFF && i + 2 < json_response.size() &&
              json_response[i + 1] == '\\' && json_response[i + 2] == 'u' && hex4(i + 3, lo)) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            i += 6;
          }
          std::wstring w;
          if (cp >= 0x10000) {
            w += (wchar_t)(0xD800 + ((cp - 0x10000) >> 10));
            w += (wchar_t)(0xDC00 + ((cp - 0x10000) & 0x3FF));
          } else {
            w += (wchar_t)cp;
          }
          raw += wtou8(w);
          break;
        }
        default: raw += e;  // \" \\ \/
      }
    }
    if (found)
      content_w = u8tow(raw);
  }
  if (found_out)
    *found_out = found;
  return content_w;
}

std::wstring LLMExtractChatContent(const std::string& json_response, bool* found_out) {
  // 思考型模型把思考寫在 content 的 <think>…</think> 裡：只留結論
  return LLMStripThinking(LLMExtractJsonString(json_response, "content", found_out));
}

// 單次 POST（每次開新連線；給不常呼叫、可等較久的用途，例如個人詞庫精煉）
bool LLMHttpPostJson(const std::string& url, const std::string& api_key,
                     const std::string& body, std::string* response,
                     unsigned long timeout_ms, unsigned long* status_code, bool* timed_out) {
  if (timed_out)
    *timed_out = false;
  // WinHTTP 的逾時只算「多久沒收到資料」；OpenRouter 在模型還沒算完時會一直送空白保持連線，
  // 永遠不會逾時。另外限制整個請求的總時間
  const ULONGLONG deadline = GetTickCount64() + timeout_ms;
  URL_COMPONENTS uc = {0};
  uc.dwStructSize = sizeof(uc);
  wchar_t host[256] = {0};
  wchar_t path[2048] = {0};
  uc.lpszHostName = host;
  uc.dwHostNameLength = _countof(host);
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = _countof(path);
  const std::wstring url_w = u8tow(url);
  if (!WinHttpCrackUrl(url_w.c_str(), (DWORD)url_w.length(), 0, &uc))
    return false;
  const bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;
  const std::wstring host_s(host, uc.dwHostNameLength);
  const bool local = host_s == L"localhost" || host_s == L"127.0.0.1";
  HINTERNET session = WinHttpOpen(L"Weasel IME/1.0",
                                  local ? WINHTTP_ACCESS_TYPE_NO_PROXY
                                        : WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session)
    return false;
  const int t = (int)timeout_ms;
  WinHttpSetTimeouts(session, t, t, t, t);
  bool ok = false;
  HINTERNET connect = WinHttpConnect(session, host_s.c_str(), uc.nPort, 0);
  HINTERNET request = connect ? WinHttpOpenRequest(connect, L"POST", path, NULL,
                                                   WINHTTP_NO_REFERER,
                                                   WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                   https ? WINHTTP_FLAG_SECURE : 0)
                              : NULL;
  if (request) {
    std::wstring headers = L"Content-Type: application/json\r\n";
    if (!api_key.empty())
      headers += L"Authorization: Bearer " + u8tow(api_key) + L"\r\n";
    if (WinHttpSendRequest(request, headers.c_str(), (DWORD)-1, (LPVOID)body.data(),
                           (DWORD)body.size(), (DWORD)body.size(), 0) &&
        WinHttpReceiveResponse(request, NULL)) {
      DWORD code = 0, size = sizeof(code);
      WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
      if (status_code)
        *status_code = code;
      response->clear();
      DWORD avail = 0;
      bool expired = false;
      while (WinHttpQueryDataAvailable(request, &avail) && avail > 0) {
        std::vector<char> buf(avail);
        DWORD read = 0;
        if (!WinHttpReadData(request, buf.data(), avail, &read))
          break;
        response->append(buf.data(), read);
        if (GetTickCount64() > deadline) {
          expired = true;
          break;
        }
      }
      if (expired && timed_out)
        *timed_out = true;
      ok = !expired && code >= 200 && code < 300;
    }
    WinHttpCloseHandle(request);
  }
  if (connect)
    WinHttpCloseHandle(connect);
  WinHttpCloseHandle(session);
  return ok;
}

bool LLMHttpPostStream(const std::string& url, const std::string& api_key,
                       const std::string& body,
                       const std::function<bool(const std::string& data)>& on_event,
                       unsigned long idle_ms, unsigned long* status_code, bool* timed_out,
                       std::string* error_body) {
  if (timed_out)
    *timed_out = false;
  URL_COMPONENTS uc = {0};
  uc.dwStructSize = sizeof(uc);
  wchar_t host[256] = {0};
  wchar_t path[2048] = {0};
  uc.lpszHostName = host;
  uc.dwHostNameLength = _countof(host);
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = _countof(path);
  const std::wstring url_w = u8tow(url);
  if (!WinHttpCrackUrl(url_w.c_str(), (DWORD)url_w.length(), 0, &uc))
    return false;
  const bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;
  const std::wstring host_s(host, uc.dwHostNameLength);
  const bool local = host_s == L"localhost" || host_s == L"127.0.0.1";
  HINTERNET session = WinHttpOpen(L"Weasel IME/1.0",
                                  local ? WINHTTP_ACCESS_TYPE_NO_PROXY
                                        : WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session)
    return false;
  const int t = (int)idle_ms;
  WinHttpSetTimeouts(session, t, t, t, t);
  bool ok = false;
  HINTERNET connect = WinHttpConnect(session, host_s.c_str(), uc.nPort, 0);
  HINTERNET request = connect ? WinHttpOpenRequest(connect, L"POST", path, NULL,
                                                   WINHTTP_NO_REFERER,
                                                   WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                   https ? WINHTTP_FLAG_SECURE : 0)
                              : NULL;
  if (request) {
    std::wstring headers = L"Content-Type: application/json\r\nAccept: text/event-stream\r\n";
    if (!api_key.empty())
      headers += L"Authorization: Bearer " + u8tow(api_key) + L"\r\n";
    if (WinHttpSendRequest(request, headers.c_str(), (DWORD)-1, (LPVOID)body.data(),
                           (DWORD)body.size(), (DWORD)body.size(), 0) &&
        WinHttpReceiveResponse(request, NULL)) {
      DWORD code = 0, size = sizeof(code);
      WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
      if (status_code)
        *status_code = code;
      const bool success = code >= 200 && code < 300;
      std::string pending, all;
      // 最後一次收到事件的時間；保持連線用的註解行（以 : 開頭）與空白不算
      ULONGLONG last_event = GetTickCount64();
      bool done = false, aborted = false;
      DWORD avail = 0;
      while (!done && WinHttpQueryDataAvailable(request, &avail) && avail > 0) {
        std::vector<char> buf(avail);
        DWORD read = 0;
        if (!WinHttpReadData(request, buf.data(), avail, &read))
          break;
        if (!success) {
          all.append(buf.data(), read);
          continue;
        }
        pending.append(buf.data(), read);
        size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
          std::string line = pending.substr(0, nl);
          pending.erase(0, nl + 1);
          if (!line.empty() && line.back() == '\r')
            line.pop_back();
          if (line.rfind("data:", 0) != 0)
            continue;
          const size_t b = line.find_first_not_of(' ', 5);
          const std::string data = b == std::string::npos ? std::string() : line.substr(b);
          if (data == "[DONE]") {
            done = true;
            break;
          }
          last_event = GetTickCount64();
          if (!on_event(data)) {
            aborted = true;
            done = true;
            break;
          }
        }
        if (!done && GetTickCount64() - last_event > idle_ms) {
          if (timed_out)
            *timed_out = true;
          break;
        }
        // 呼叫端要求取消（例如服務要重新部署、精煉器停止）：不等模型說完
        if (!done && LLMCancelled()) {
          aborted = true;
          break;
        }
      }
      if (!success && error_body)
        *error_body = all;
      ok = success && !aborted && (!timed_out || !*timed_out);
    }
    WinHttpCloseHandle(request);
  }
  if (connect)
    WinHttpCloseHandle(connect);
  WinHttpCloseHandle(session);
  return ok;
}

// 全局开发终端实例（供LLMProvider使用）
DevConsole* g_dev_console = nullptr;

