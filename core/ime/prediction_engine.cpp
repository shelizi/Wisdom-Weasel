#include "prediction_engine.h"

#include <atomic>
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

bool PredictionEngine::Take(size_t index, std::wstring* text, bool* recommend, bool* correction) {
  std::lock_guard<std::mutex> lock(state_->mutex);
  PredictionSet& set = state_->current;
  if (index >= set.candidates.size())
    return false;
  *text = set.candidates[index];
  *recommend = set.IsRecommend(index);
  *correction = set.IsCorrection(index);
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
  // 寫入候選並通知呼叫端；已有更新的請求時丟棄
  const auto publish = [&](std::vector<std::wstring> candidates, size_t recommends,
                           size_t corrections) {
    PredictionSet set;
    set.candidates = std::move(candidates);
    set.recommends = recommends;
    set.corrections = corrections;
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

  const std::vector<std::wstring>& personal = req.personal;
  // 1) 先顯示個人詞庫的候選
  if (!personal.empty() && !publish(personal, 0, 0))
    return;
  const bool rescore = req.rescore && !req.prefix.empty();
  const bool correct = req.correct && !req.zhuyin.empty() && !req.prefix.empty();
  if (!req.predict && !correct && !rescore)
    return;
  std::wstring context = req.history + req.prefix;
  std::wstring prefix = req.prefix;

  // 2) 推薦：同音字由呼叫端查（要用 Rime），這時還沒拿推理鎖
  std::vector<std::wstring> recommends;
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
      {
        std::lock_guard<std::mutex> infer_lock(state->infer);
        if (!current())
          return;
        if (LLMProvider* scorer = models().rescore) {
          const std::wstring tail =
              req.history.size() > 30 ? req.history.substr(req.history.size() - 30) : req.history;
          recommended = RescoreSentence(scorer, tail, units, homophones);
        }
      }
      if (!recommended.empty() && recommended != zhuyin_preview::Join(units)) {
        recommends.push_back(recommended);
        if (!publish(MergeCandidates(recommends, personal), recommends.size(), 0))
          return;
        context = req.history + recommended;
        prefix = recommended;
      }
    }
  }
  if (!req.predict && !correct)
    return;

  // 3) 注音整句校正；之後的續寫接在校正結果後面
  std::vector<std::wstring> corrections;
  if (correct) {
    std::wstring corrected;
    {
      std::lock_guard<std::mutex> infer_lock(state->infer);
      LLMProvider* typo = current() ? models().typo : nullptr;
      if (!typo)
        return;
      corrected = CleanCorrection(
          typo->CorrectSentence(req.typo_context, req.zhuyin, req.prefix, req.typo_prompt),
          req.prefix);
    }
    if (Logging())
      g_dev_console->WriteLine(L"[LLM] 整句校正: " + req.prefix + L" → " +
                               (corrected.empty() ? L"(不需校正)" : corrected));
    if (!corrected.empty()) {
      corrections.push_back(corrected);
      const std::vector<std::wstring> head = MergeCandidates(recommends, corrections);
      if (!publish(MergeCandidates(head, personal), recommends.size(),
                   head.size() - recommends.size()))
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
  const std::vector<std::wstring> head = MergeCandidates(recommends, corrections);
  std::vector<std::wstring> merged = MergeCandidates(MergeCandidates(head, personal), candidates);
  if (Logging())
    g_dev_console->WriteLine(L"[LLM] 预测完成，共 " + std::to_wstring(merged.size()) +
                             L" 个候选（个人词库 " + std::to_wstring(personal.size()) + L"）");
  publish(std::move(merged), recommends.size(), head.size() - recommends.size());
}

}  // namespace ime
