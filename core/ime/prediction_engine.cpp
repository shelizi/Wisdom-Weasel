#include "prediction_engine.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <thread>

#include "../base/devlog.h"
#include "../llm/LLMProvider.h"
#include "ZhuyinPreview.h"
#include "rescore.h"
#include "text_rules.h"

namespace ime {

struct PredictionEngine::State {
  Hooks hooks;
  std::atomic<uint64_t> seq{0};
  mutable std::mutex mutex;  // 保護 current
  PredictionSet current;
  std::mutex infer;  // 串行化推理
};

namespace {

bool Logging() {
  return g_dev_console && g_dev_console->IsEnabled();
}

}  // namespace

PredictionEngine::PredictionEngine(Hooks hooks) : state_(std::make_shared<State>()) {
  state_->hooks = std::move(hooks);
}

PredictionEngine::~PredictionEngine() {
  Cancel();
}

uint64_t PredictionEngine::Request(PredictionRequest request) {
  const uint64_t seq = ++state_->seq;
  if (Logging()) {
    g_dev_console->WriteLine(L"[LLM] ========== 开始预测 ==========");
    g_dev_console->WriteLine(L"[LLM] 上下文: " + request.history + request.prefix);
    std::wstring joined;
    for (const auto& p : request.personal)
      joined += (joined.empty() ? L"" : L" / ") + p;
    g_dev_console->WriteLine(L"[LLM] 个人词库候选: " + (joined.empty() ? L"(无)" : joined));
  }
  std::thread([state = state_, seq, request = std::move(request)]() { Run(state, seq, request); })
      .detach();
  return seq;
}

void PredictionEngine::Cancel() {
  std::lock_guard<std::mutex> lock(state_->mutex);
  ++state_->seq;
  state_->current = PredictionSet();
}

double PredictionSet::Gain(size_t i) const {
  return i < gains.size() ? gains[i] : NoConfidence();
}

double PredictionSet::Confidence(size_t i) const {
  return i < confidences.size() ? confidences[i] : NoConfidence();
}

size_t PredictionSet::Count(CandidateKind kind) const {
  return (size_t)std::count(kinds.begin(), kinds.end(), kind);
}

std::wstring PredictionSet::Comment(size_t i) const {
  const CandidateKind kind = Kind(i);
  if (kind == CandidateKind::kPrediction)
    return L"";
  std::wstring text = kind == CandidateKind::kCorrection ? L"校正" : L"推薦";
  const double p = Confidence(i);
  if (HasConfidence(p))
    text += L" " + std::to_wstring((int)std::lround(p * 100)) + L"%";
  return text;
}

bool PredictionEngine::IsCurrent(uint64_t seq) const {
  return seq == state_->seq.load();
}

PredictionSet PredictionEngine::Snapshot() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->current;
}

bool PredictionEngine::HasCandidates() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return !state_->current.candidates.empty();
}

bool PredictionEngine::Take(size_t index, std::wstring* text, bool* recommend, bool* correction,
                            CandidateKind* kind) {
  std::lock_guard<std::mutex> lock(state_->mutex);
  PredictionSet& set = state_->current;
  if (index >= set.candidates.size())
    return false;
  *text = set.candidates[index];
  *recommend = set.IsRecommend(index);
  *correction = set.IsCorrection(index);
  if (kind)
    *kind = set.Kind(index);
  set = PredictionSet();
  return true;
}

std::mutex& PredictionEngine::InferMutex() {
  return state_->infer;
}

void PredictionEngine::Run(const std::shared_ptr<State>& state, uint64_t seq,
                           const PredictionRequest& req) {
  const auto current = [&] { return seq == state->seq.load(); };
  // 生成途中又有新請求（繼續打字）：本機模型立即停止，讓新的請求接著開始
  LLMCancelScope cancel_scope([state, seq] { return seq != state->seq.load(); });
  // 防抖：等待期間若又有新請求，直接放棄，不佔用 GPU
  if (req.delay_ms > 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(req.delay_ms));
    if (!current())
      return;
  }
  const auto models = [&] {
    return state->hooks.models ? state->hooks.models() : PredictionModels();
  };
  // 推薦與校正：排在最前，依校準過的採用機率決定先後
  struct Suggestion {
    std::wstring text;
    CandidateKind kind;
    double gain;
    double confidence;
  };
  std::vector<Suggestion> head;
  const auto confidence = [&](SuggestionKind kind, double gain) {
    return state->hooks.confidence && std::isfinite(gain) ? state->hooks.confidence(kind, gain)
                                                          : NoConfidence();
  };
  // 加入一個推薦或校正；機率低於門檻就不顯示，回傳 false（之後的續寫也不接在它後面）
  const auto suggest = [&](std::wstring text, CandidateKind kind, double gain) {
    const double p = confidence(kind == CandidateKind::kRecommend ? SuggestionKind::kRecommend
                                : kind == CandidateKind::kRerank  ? SuggestionKind::kRerank
                                                                  : SuggestionKind::kCorrection,
                                gain);
    if (Logging()) {
      std::wstring line = std::wstring(L"[LLM] ") +
                          (kind == CandidateKind::kCorrection ? L"校正"
                           : kind == CandidateKind::kRerank   ? L"整句重排"
                                                              : L"推薦") +
                          L"信心：gain " + (std::isfinite(gain) ? std::to_wstring(gain) : L"—") + L"，p " +
                          (HasConfidence(p) ? std::to_wstring(p) : L"（尚未校準）");
      g_dev_console->WriteLine(line);
    }
    if (HasConfidence(p) && p < req.min_confidence)
      return false;
    head.push_back({std::move(text), kind, gain, p});
    // 都有機率時高的排前面；還沒校準的維持原本順序（推薦、校正）
    std::stable_sort(head.begin(), head.end(), [](const Suggestion& a, const Suggestion& b) {
      return HasConfidence(a.confidence) && HasConfidence(b.confidence) && a.confidence > b.confidence;
    });
    return true;
  };
  // 寫入候選並通知呼叫端；已有更新的請求時丟棄
  const auto publish = [&](const std::vector<std::wstring>& more) {
    PredictionSet set;
    std::vector<std::wstring> texts;
    for (const auto& s : head) {
      if (std::find(texts.begin(), texts.end(), s.text) != texts.end())
        continue;
      texts.push_back(s.text);
      set.kinds.push_back(s.kind);
      set.gains.push_back(s.gain);
      set.confidences.push_back(s.confidence);
    }
    set.candidates = MergeCandidates(MergeCandidates(texts, req.personal), more);
    set.kinds.resize(set.candidates.size(), CandidateKind::kPrediction);
    set.gains.resize(set.candidates.size(), NoConfidence());
    set.confidences.resize(set.candidates.size(), NoConfidence());
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (!current())
        return false;
      state->current = set;
    }
    if (state->hooks.on_update)
      state->hooks.on_update(req.tag, seq, set);
    return true;
  };

  // 1) 先顯示個人詞庫的候選
  if (!req.personal.empty() && !publish({}))
    return;
  const bool rescore = req.rescore && !req.prefix.empty();
  const bool correct = req.correct && !req.zhuyin.empty() && !req.prefix.empty();
  const bool rerank = (req.rerank || req.rerank_shadow) && !req.prefix.empty();
  if (!req.predict && !correct && !rescore && !rerank)
    return;
  std::wstring context = req.history + req.prefix;
  std::wstring prefix = req.prefix;
  const std::wstring tail =
      req.history.size() > 30 ? req.history.substr(req.history.size() - 30) : req.history;

  // 2a) 整句重排：Rime 的整句候選由呼叫端查（要用 Rime），這時還沒拿推理鎖
  if (rerank && state->hooks.rerank_input && current()) {
    std::vector<std::wstring> sentences;
    if (!state->hooks.rerank_input(req.tag, seq, &sentences)) {
      if (!current())
        return;
      sentences.clear();
    }
    if (sentences.size() >= 2) {
      RerankResult result;
      {
        std::lock_guard<std::mutex> infer_lock(state->infer);
        if (!current())
          return;
        if (LLMProvider* scorer = models().rescore)
          result = RerankSentences(scorer, tail, sentences, req.rerank_options);
      }
      if (req.rerank_shadow) {
        if (state->hooks.on_shadow && current())
          state->hooks.on_shadow(req.tag, seq, sentences[0], result);
      } else if (!result.text.empty() && result.text != sentences[0] &&
                 suggest(result.text, CandidateKind::kRerank, result.gain)) {
        if (!publish({}))
          return;
        context = req.history + result.text;
        prefix = result.text;
      }
    }
  }
  if (!req.predict && !correct && !rescore)
    return;

  // 2) 推薦：同音字由呼叫端查（要用 Rime），這時還沒拿推理鎖
  if (rescore && state->hooks.rescore_input && current()) {
    std::vector<std::wstring> units;
    std::vector<std::vector<std::wstring>> homophones;
    if (!state->hooks.rescore_input(req.tag, seq, &units, &homophones)) {
      if (!current())
        return;
      units.clear();
    }
    if (!units.empty()) {
      std::wstring recommended;
      double gain = NoConfidence();
      {
        std::lock_guard<std::mutex> infer_lock(state->infer);
        if (!current())
          return;
        if (LLMProvider* scorer = models().rescore)
          recommended = RescoreSentence(scorer, tail, units, homophones, &gain);
      }
      if (!recommended.empty() && recommended != zhuyin_preview::Join(units) &&
          suggest(recommended, CandidateKind::kRecommend, gain)) {
        if (!publish({}))
          return;
        context = req.history + recommended;
        prefix = recommended;
      }
    }
  }
  if (!req.predict && !correct)
    return;

  // 3) 注音整句校正；之後的續寫接在校正結果後面
  if (correct) {
    std::wstring corrected;
    double gain = NoConfidence();
    {
      std::lock_guard<std::mutex> infer_lock(state->infer);
      LLMProvider* typo = current() ? models().typo : nullptr;
      if (!typo)
        return;
      corrected = CleanCorrection(
          typo->CorrectSentence(req.typo_context, req.zhuyin, req.prefix, req.typo_prompt),
          req.prefix);
      // 有本機模型時比較校正前後的通順度，給信心校準用（還有沒拼完的注音時不比）
      LLMProvider* scorer = models().rescore;
      const bool spelling = std::any_of(req.prefix.begin(), req.prefix.end(), zhuyin_preview::IsBopomofo);
      double before = 0, after = 0;
      if (!corrected.empty() && !spelling && state->hooks.confidence && scorer && !LLMCancelled() &&
          scorer->ScoreText(tail, req.prefix, &before, nullptr) &&
          scorer->ScoreText(tail, corrected, &after, nullptr))
        gain = after - before;
    }
    if (Logging())
      g_dev_console->WriteLine(L"[LLM] 整句校正: " + req.prefix + L" → " +
                               (corrected.empty() ? L"(不需校正)" : corrected));
    if (!corrected.empty() && suggest(corrected, CandidateKind::kCorrection, gain)) {
      if (!publish({}))
        return;
      context = req.history + corrected;
      prefix = corrected;
    }
  }
  if (!req.predict)
    return;

  // 4) LLM 續寫：排到時若已有更新的請求就放棄
  std::vector<std::wstring> candidates;
  {
    std::lock_guard<std::mutex> infer_lock(state->infer);
    LLMProvider* predict = current() ? models().predict : nullptr;
    if (!predict)
      return;
    candidates = predict->PredictCandidates(context, req.current_input, 5);
  }
  candidates = CleanCandidates(candidates, prefix);

  // 推薦、校正排最前，接著個人詞庫，LLM 續寫接在後面
  if (Logging())
    g_dev_console->WriteLine(L"[LLM] 预测完成，LLM 续写 " + std::to_wstring(candidates.size()) +
                             L" 个（个人词库 " + std::to_wstring(req.personal.size()) + L"）");
  publish(candidates);
}

}  // namespace ime
