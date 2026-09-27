#include "LLMProvider.h"
#include "../base/devlog.h"
#include "../base/utf8.h"
#include <rime_api.h>
#include "../net/http.h"
#include <sstream>

namespace {

std::string EscapeJsonString(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '"')
      out += "\\\"";
    else if (c == '\\')
      out += "\\\\";
    else if (c == '\n')
      out += "\\n";
    else if (c == '\r')
      out += "\\r";
    else if (c == '\t')
      out += "\\t";
    else
      out += c;
  }
  return out;
}

}  // namespace

HFConstraintProvider::HFConstraintProvider()
    : m_enabled(false),
      m_api_url("http://localhost:8000/v1/generate/completions"),
      m_http(std::make_unique<net::Session>()) {}

HFConstraintProvider::~HFConstraintProvider() {
  CloseConnection();
}

void HFConstraintProvider::CloseConnection() {
  m_http->Reset();
}

bool HFConstraintProvider::LoadConfig(const std::string& config_name) {

  RimeApi* rime_api = rime_get_api();
  if (!rime_api) {
    if (g_dev_console && g_dev_console->IsEnabled()) {
      g_dev_console->WriteLine(L"[LLM] LoadConfig失败: rime_api未初始化");
    }
    return false;
  }

  if (g_dev_console && g_dev_console->IsEnabled()) {
    g_dev_console->WriteLine(L"[LLM] 开始从配置文件加载 HF Constraint 配置: " +
                             utf8::ToWide(config_name));
  }

  RimeConfig config = {NULL};
  if (!rime_api->config_open(config_name.c_str(), &config)) {
    if (g_dev_console && g_dev_console->IsEnabled()) {
      g_dev_console->WriteLine(L"[LLM] LoadConfig失败: 无法打开配置文件 " +
                              utf8::ToWide(config_name));
    }
    return false;
  }

  Bool enabled = false;
  bool found_enabled = rime_api->config_get_bool(&config, "llm/enabled", &enabled);

  if (found_enabled) {
    m_enabled = !!enabled;
  } else {
    m_enabled = false;
    if (g_dev_console && g_dev_console->IsEnabled()) {
      g_dev_console->WriteLine(L"[LLM] LoadConfig失败: 未找到配置项 llm/enabled");
    }
    rime_api->config_close(&config);
    return false;
  }

  if (!m_enabled) {
    if (g_dev_console && g_dev_console->IsEnabled()) {
      g_dev_console->WriteLine(L"[LLM] LoadConfig失败: llm/enabled 为 false");
    }
    rime_api->config_close(&config);
    return false;
  }

  const int BUF_SIZE = 512;
  char buffer[BUF_SIZE + 1] = {0};

  bool found_api_url =
      rime_api->config_get_string(&config, "llm/hf_constraint/api_url", buffer, BUF_SIZE);
  if (found_api_url) {
    m_api_url = buffer;
    if (g_dev_console && g_dev_console->IsEnabled()) {
      g_dev_console->WriteLine(L"[LLM] 找到配置项 llm/hf_constraint/api_url = " +
                              utf8::ToWide(m_api_url));
    }
  } else {
    m_api_url = "http://localhost:8000/v1/generate/completions";
    if (g_dev_console && g_dev_console->IsEnabled()) {
      g_dev_console->WriteLine(L"[LLM] 未找到配置项 llm/hf_constraint/api_url，使用默认值 = " +
                              utf8::ToWide(m_api_url));
    }
  }

  CloseConnection();  // URL 可能变化，下次请求时重建连接
  rime_api->config_close(&config);

  if (g_dev_console && g_dev_console->IsEnabled()) {
    g_dev_console->WriteLine(L"[LLM] HF Constraint 配置加载成功");
  }

  return true;
}

std::vector<std::wstring> HFConstraintProvider::PredictCandidates(
    const std::wstring& context,
    const std::wstring& current_input,
    size_t max_candidates) {
  std::vector<std::wstring> candidates;

  if (!IsAvailable() || context.empty()) {
    return candidates;
  }

  // 允许空上下文（冷启动），仍向后端发送请求，与 RimeWithWeasel 的“支持冷启动”一致
  std::string prompt_utf8 = utf8::FromWide(context);
  std::string escaped_prompt = EscapeJsonString(prompt_utf8);

  // pinyin_constraints: 当前输入，按空格分割为拼音音节数组
  std::vector<std::string> constraint_parts;
  if (!current_input.empty()) {
    std::wstringstream ss(current_input);
    std::wstring part;
    while (ss >> part) {
      if (!part.empty()) {
        constraint_parts.push_back(utf8::FromWide(part));
      }
    }
    if (constraint_parts.empty()) {
      constraint_parts.push_back(utf8::FromWide(current_input));
    }
  }

  std::ostringstream json;
  json << "{\"prompt\":\"" << escaped_prompt << "\",\"pinyin_constraints\":[";
  for (size_t i = 0; i < constraint_parts.size(); ++i) {
    if (i > 0)
      json << ",";
    json << "\"" << EscapeJsonString(constraint_parts[i]) << "\"";
  }
  json << "]}";

  std::string request_body = json.str();

  if (g_dev_console && g_dev_console->IsEnabled()) {
    g_dev_console->WriteLine(L"[LLM] [HF Constraint] 发送预测请求");
    g_dev_console->WriteLine(L"  上下文: " + context);
    g_dev_console->WriteLine(L"  当前输入: " + current_input);
    g_dev_console->WriteLine(L"  请求URL: " + utf8::ToWide(m_api_url));
    g_dev_console->WriteLine(L"  请求体: " + utf8::ToWide(request_body));
  }

  std::string response_body;
  if (!ExecuteRequest(m_api_url, request_body, response_body)) {
    if (g_dev_console && g_dev_console->IsEnabled()) {
      g_dev_console->WriteLine(L"[LLM] [HF Constraint] 请求失败");
    }
    return candidates;
  }

  if (g_dev_console && g_dev_console->IsEnabled()) {
    g_dev_console->WriteLine(L"[LLM] [HF Constraint] 收到响应");
    g_dev_console->WriteLine(L"  响应内容: " + utf8::ToWide(response_body));
  }

  candidates = ParseResponse(response_body);

  if ((size_t)candidates.size() > max_candidates) {
    candidates.resize(max_candidates);
  }

  return candidates;
}

bool HFConstraintProvider::IsAvailable() const {
  return m_enabled && !m_api_url.empty();
}

bool HFConstraintProvider::ExecuteRequest(const std::string& url,
                                         const std::string& request_body,
                                         std::string& response_body) {
  net::Request request;
  request.method = "POST";
  request.url = url;
  request.headers.push_back({"Content-Type", "application/json"});
  request.body = request_body;
  request.connect_timeout_ms = request.receive_timeout_ms = 10000;
  net::Response response;
  std::string error;
  const bool ok = m_http->Fetch(request, &response, &error);
  response_body = response.body;
  return ok && !response_body.empty();
}

std::vector<std::wstring> HFConstraintProvider::ParseResponse(
    const std::string& json_response) {
  std::vector<std::wstring> candidates;

  // 解析 "responses" 字段，格式如 {"responses":"螃蟹 披 苹果 泡 葡萄"}
  const char* field_names[] = {"\"responses\"", "\"text\"", "\"generated_text\"", "\"content\""};
  size_t content_pos = std::string::npos;

  for (const char* field : field_names) {
    content_pos = json_response.find(field);
    if (content_pos != std::string::npos)
      break;
  }

  if (content_pos == std::string::npos) {
    return candidates;
  }

  size_t colon_pos = json_response.find(':', content_pos);
  if (colon_pos == std::string::npos) {
    return candidates;
  }

  size_t quote_start = json_response.find('"', colon_pos);
  if (quote_start == std::string::npos) {
    return candidates;
  }

  size_t quote_end = quote_start + 1;
  while (quote_end < json_response.size()) {
    size_t next = json_response.find('"', quote_end);
    if (next == std::string::npos)
      break;
    if (json_response[next - 1] != '\\') {
      quote_end = next;
      break;
    }
    quote_end = next + 1;
  }

  if (quote_end >= json_response.size()) {
    return candidates;
  }

  std::string content = json_response.substr(quote_start + 1,
                                             quote_end - quote_start - 1);
  std::wstring content_w = utf8::ToWide(content);

  std::wstringstream ss(content_w);
  std::wstring word;
  while (ss >> word) {
    if (!word.empty()) {
      candidates.push_back(word);
    }
  }

  return candidates;
}
