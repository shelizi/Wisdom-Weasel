#include "bench.h"

#include <algorithm>
#include <chrono>

#include "../base/utf8.h"
#include "../ime/text_rules.h"
#include "../llm/RemoteLLMProvider.h"

namespace settings_bench {

namespace {

using json = nlohmann::json;

std::wstring Trim(const std::wstring& s) {
  const wchar_t* ws = L" \t\r\n　";
  const size_t b = s.find_first_not_of(ws);
  if (b == std::wstring::npos)
    return std::wstring();
  return s.substr(b, s.find_last_not_of(ws) - b + 1);
}

// 一行依 | 切開（全形｜也可以），去掉每欄前後空白；空行與 # 開頭的行回傳空的
std::vector<std::wstring> Fields(const std::wstring& line) {
  const std::wstring t = Trim(line);
  if (t.empty() || t[0] == L'#')
    return {};
  std::vector<std::wstring> out;
  std::wstring cur;
  for (wchar_t c : t) {
    if (c == L'|' || c == L'｜') {
      out.push_back(Trim(cur));
      cur.clear();
    } else {
      cur += c;
    }
  }
  out.push_back(Trim(cur));
  return out;
}

template <typename F>
void ForEachLine(const std::wstring& text, F f) {
  size_t start = 0, n = 0;
  while (start <= text.size()) {
    size_t end = text.find(L'\n', start);
    if (end == std::wstring::npos)
      end = text.size();
    f(text.substr(start, end - start), ++n);
    start = end + 1;
  }
}

bool StartsWith(const std::wstring& s, const std::wstring& prefix) {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

long long Ms(std::chrono::steady_clock::time_point since) {
  return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - since)
      .count();
}

}  // namespace

std::vector<PredictCase> ParsePredictCases(const std::wstring& text,
                                           std::vector<std::wstring>* errors) {
  std::vector<PredictCase> out;
  ForEachLine(text, [&](const std::wstring& line, size_t n) {
    const auto f = Fields(line);
    if (f.empty())
      return;
    if (f.size() != 2 || f[0].empty() || f[1].empty()) {
      errors->push_back(L"預測第 " + std::to_wstring(n) + L" 行要是「前文|期望的詞」");
      return;
    }
    out.push_back({f[0], f[1]});
  });
  return out;
}

std::vector<CorrectCase> ParseCorrectCases(const std::wstring& text,
                                           std::vector<std::wstring>* errors) {
  std::vector<CorrectCase> out;
  ForEachLine(text, [&](const std::wstring& line, size_t n) {
    const auto f = Fields(line);
    if (f.empty())
      return;
    if (f.size() != 4 || f[1].empty() || f[2].empty() || f[3].empty()) {
      errors->push_back(L"校正第 " + std::to_wstring(n) + L" 行要是「前文|注音|初稿|正確的句子」");
      return;
    }
    out.push_back({f[0], f[1], f[2], f[3]});
  });
  return out;
}

bool PredictionHit(const std::vector<std::wstring>& candidates, const std::wstring& expected,
                   size_t top) {
  for (size_t i = 0; i < candidates.size() && i < top; ++i) {
    const std::wstring& c = candidates[i];
    // 候選比期望的長（多接了幾個字）也算；候選只是期望的開頭時至少要兩個字
    if (StartsWith(c, expected) ||
        (!c.empty() && StartsWith(expected, c) && c.size() >= (std::min)((size_t)2, expected.size())))
      return true;
  }
  return false;
}

bool CorrectionOk(const std::wstring& cleaned, const CorrectCase& c) {
  if (c.expected == c.draft)  // 初稿本來就對：不該亂改
    return cleaned.empty() || cleaned == c.draft;
  return cleaned == c.expected;
}

void Run(const std::vector<Profile>& profiles, const Options& options,
         const std::vector<PredictCase>& predict, const std::vector<CorrectCase>& correct,
         const std::atomic<bool>& cancel, const std::function<void(const json&)>& report) {
  // 取消時進行中的推理也中斷（推理行程每產生一個 token 檢查一次）
  LLMCancelScope cancel_scope([&cancel] { return cancel.load(); });
  for (size_t p = 0; p < profiles.size() && !cancel; ++p) {
    const Profile& profile = profiles[p];
    RemoteLLMProvider provider(profile.remote ? "openai" : "llamacpp");
    const auto load_start = std::chrono::steady_clock::now();
    if (profile.remote) {
      provider.ConfigureDirect(profile.api_url, profile.api_key, profile.model, options.prompt,
                               profile.no_think, profile.think_tokens);
    } else {
      LLMLocalModelSpec spec;
      spec.model_path = profile.model_path;
      spec.instruct = profile.instruct;
      spec.n_ctx = options.n_ctx;
      spec.n_gpu_layers = options.n_gpu_layers;
      spec.n_threads = options.n_threads;
      spec.disable_thinking = profile.no_think;
      spec.think_tokens = profile.think_tokens;
      if (provider.LoadModelDirect(spec, options.temperature))
        provider.SetPromptPrefix(options.prompt);
    }
    const bool loaded = provider.IsAvailable();
    report({{"state", "load"},
            {"profile", p},
            {"ok", loaded},
            {"ms", Ms(load_start)},
            {"error", loaded ? "" : profile.remote ? "API 設定不完整" : "無法載入模型（檔案不存在、記憶體不足或格式不支援）"}});
    if (!loaded)
      continue;

    for (size_t i = 0; i < predict.size() && !cancel; ++i) {
      const auto start = std::chrono::steady_clock::now();
      const auto candidates =
          ime::CleanCandidates(provider.PredictCandidates(predict[i].context, L"", 5), L"");
      const long long ms = Ms(start);
      if (cancel)
        break;
      json list = json::array();
      for (const auto& c : candidates)
        list.push_back(utf8::FromWide(c));
      report({{"state", "predict"},
              {"profile", p},
              {"index", i},
              {"top1", PredictionHit(candidates, predict[i].expected, 1)},
              {"ok", PredictionHit(candidates, predict[i].expected, 5)},
              {"ms", ms},
              {"candidates", list}});
    }
    for (size_t i = 0; i < correct.size() && !cancel; ++i) {
      const CorrectCase& c = correct[i];
      const auto start = std::chrono::steady_clock::now();
      const std::wstring cleaned = ime::CleanCorrection(
          provider.CorrectSentence(c.context, c.zhuyin, c.draft, options.typo_prompt), c.draft);
      const long long ms = Ms(start);
      if (cancel)
        break;
      report({{"state", "correct"},
              {"profile", p},
              {"index", i},
              {"ok", CorrectionOk(cleaned, c)},
              {"ms", ms},
              {"output", utf8::FromWide(cleaned.empty() ? c.draft : cleaned)},
              {"changed", !cleaned.empty() && cleaned != c.draft}});
    }
  }
}

}  // namespace settings_bench
