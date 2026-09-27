#include "LLMProvider.h"
#include "../base/devlog.h"
#include "../base/utf8.h"
#include <rime_api.h>
#include "../net/http.h"
#include <chrono>
#include <sstream>
#include <algorithm>
#include <cstdio>
#include <cwctype>

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
      m_http(std::make_unique<net::Session>()) {}

OpenAICompatibleProvider::~OpenAICompatibleProvider() {
  CloseConnection();
}

void OpenAICompatibleProvider::CloseConnection() {
  m_http->Reset();
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
      g_dev_console->WriteLine(L"[LLM] LoadConfig: api_url = " + utf8::ToWide(m_api_url));
      g_dev_console->WriteLine(L"[LLM] LoadConfig: model = " + utf8::ToWide(m_model));
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
    g_dev_console->WriteLine(L"[LLM] 开始从配置文件加载: " + utf8::ToWide(config_name));
  }

  RimeConfig config = {NULL};
  if (!rime_api->config_open(config_name.c_str(), &config)) {
    if (g_dev_console && g_dev_console->IsEnabled()) {
      std::wstring config_name_w = utf8::ToWide(config_name);
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
      g_dev_console->WriteLine(L"[LLM] 找到配置项 llm/openai/api_url = " + utf8::ToWide(m_api_url));
    }
  } else {
    m_api_url = "https://api.openai.com/v1/chat/completions";
    if (g_dev_console && g_dev_console->IsEnabled()) {
      g_dev_console->WriteLine(L"[LLM] 未找到配置项 llm/openai/api_url，使用默认值 = " + utf8::ToWide(m_api_url));
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
          ? (utf8::ToWide(m_api_key.substr(0, 8)) + L"...") 
          : utf8::ToWide(m_api_key);
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
      g_dev_console->WriteLine(L"[LLM] 找到配置项 llm/openai/model = " + utf8::ToWide(m_model));
    }
  } else {
    m_model = "gpt-3.5-turbo";
    if (g_dev_console && g_dev_console->IsEnabled()) {
      g_dev_console->WriteLine(L"[LLM] 未找到配置项 llm/openai/model，使用默认值 = " + utf8::ToWide(m_model));
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
        std::wstring(m_extra_body_json.empty() ? L"(空)" : utf8::ToWide(m_extra_body_json)));
  }

  // 提示词（与 llama.cpp 共用）：llm/prompt；兼容旧的 llm/llamacpp/prompt_prefix
  {
    char prompt_buf[4096] = {0};
    if (rime_api->config_get_string(&config, "llm/prompt", prompt_buf, sizeof(prompt_buf) - 1) ||
        rime_api->config_get_string(&config, "llm/llamacpp/prompt_prefix", prompt_buf,
                                    sizeof(prompt_buf) - 1))
      m_prompt = utf8::ToWide(prompt_buf);
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
  std::string request_body = BuildChatBody(LLMInstructSystem(m_prompt, max_candidates),
                                           LLMInstructUser(context, current_input),
                                           LLMTokenBudget(m_max_tokens, !m_disable_thinking,
                                                          m_think_tokens),
                                           m_temperature);

  if (g_dev_console && g_dev_console->IsEnabled()) {
    g_dev_console->WriteLine(L"[LLM] 发送预测请求");
    g_dev_console->WriteLine(L"  上下文: " + context);
    g_dev_console->WriteLine(L"  请求URL: " + utf8::ToWide(m_api_url));
    g_dev_console->WriteLine(L"  请求体: " + utf8::ToWide(request_body));
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
    g_dev_console->WriteLine(L"  响应内容: " + utf8::ToWide(response_body));
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
  std::ostringstream json;
  json << "{"
       << "\"model\":\"" << escape_json(m_model) << "\","
       << "\"messages\":["
       << "{\"role\":\"system\",\"content\":\"" << escape_json(utf8::FromWide(system)) << "\"},"
       << "{\"role\":\"user\",\"content\":\"" << escape_json(utf8::FromWide(user)) << "\"}"
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
  // 打字時連續預測：重用同一條連線
  net::Request request;
  request.method = "POST";
  request.url = url;
  request.headers.push_back({"Content-Type", "application/json"});
  if (!m_api_key.empty())
    request.headers.push_back({"Authorization", "Bearer " + m_api_key});
  request.body = request_body;
  request.connect_timeout_ms = request.receive_timeout_ms = 10000;
  net::Response response;
  std::string error;
  const bool ok = m_http->Fetch(request, &response, &error);
  response_body = response.body;
  return ok && !response_body.empty();
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
          raw += utf8::FromWide(w);
          break;
        }
        default: raw += e;  // \" \\ \/
      }
    }
    if (found)
      content_w = utf8::ToWide(raw);
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
  net::Request request;
  request.method = "POST";
  request.url = url;
  request.headers.push_back({"Content-Type", "application/json"});
  if (!api_key.empty())
    request.headers.push_back({"Authorization", "Bearer " + api_key});
  request.body = body;
  request.connect_timeout_ms = request.receive_timeout_ms = (int)timeout_ms;
  // 有些服務在模型還沒算完時會一直送空白保持連線：另外限制整個請求的總時間
  request.total_timeout_ms = (int)timeout_ms;
  request.max_response = 64u << 20;
  net::Response r;
  std::string error;
  const bool sent = net::Fetch(request, &r, &error);
  *response = r.body;
  if (status_code)
    *status_code = (unsigned long)r.status;
  if (timed_out)
    *timed_out = r.timed_out;
  return sent && !r.timed_out && r.status >= 200 && r.status < 300;
}

bool LLMHttpPostStream(const std::string& url, const std::string& api_key,
                       const std::string& body,
                       const std::function<bool(const std::string& data)>& on_event,
                       unsigned long idle_ms, unsigned long* status_code, bool* timed_out,
                       std::string* error_body) {
  using Clock = std::chrono::steady_clock;
  if (timed_out)
    *timed_out = false;
  net::Request request;
  request.method = "POST";
  request.url = url;
  request.headers.push_back({"Content-Type", "application/json"});
  request.headers.push_back({"Accept", "text/event-stream"});
  if (!api_key.empty())
    request.headers.push_back({"Authorization", "Bearer " + api_key});
  request.body = body;
  request.connect_timeout_ms = request.receive_timeout_ms = (int)idle_ms;
  net::Response r;
  std::string pending, all;
  // 最後一次收到事件的時間；保持連線用的註解行（以 : 開頭）與空白不算
  Clock::time_point last_event = Clock::now();
  bool done = false, aborted = false, idle = false;
  auto on_data = [&](const char* data, size_t size) {
    if (r.status < 200 || r.status >= 300) {
      all.append(data, size);
      return true;
    }
    pending.append(data, size);
    size_t nl;
    while ((nl = pending.find('\n')) != std::string::npos) {
      std::string line = pending.substr(0, nl);
      pending.erase(0, nl + 1);
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      if (line.rfind("data:", 0) != 0)
        continue;
      const size_t b = line.find_first_not_of(' ', 5);
      const std::string event = b == std::string::npos ? std::string() : line.substr(b);
      if (event == "[DONE]") {
        done = true;
        return false;
      }
      last_event = Clock::now();
      if (!on_event(event)) {
        aborted = true;
        return false;
      }
    }
    if (Clock::now() - last_event > std::chrono::milliseconds(idle_ms)) {
      idle = true;
      return false;
    }
    // 呼叫端要求取消（例如服務要重新部署、精煉器停止）：不等模型說完
    if (LLMCancelled()) {
      aborted = true;
      return false;
    }
    return true;
  };
  std::string error;
  const bool sent = net::Stream(request, on_data, &r, &error);
  if (status_code)
    *status_code = (unsigned long)r.status;
  if ((idle || r.timed_out) && timed_out)
    *timed_out = true;
  const bool success = r.status >= 200 && r.status < 300;
  if (!success && error_body)
    *error_body = all;
  return sent && success && !aborted && !idle;
}

// 全局开发终端实例（供LLMProvider使用）

