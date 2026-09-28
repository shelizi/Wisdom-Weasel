#include "LearnFilter.h"
#include "PersonalLexicon.h"
#include "../llm/LLMProvider.h"

#include <chrono>
#include <cstdio>

namespace {

uint64_t NowMs() {
  return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool IsLowSurrogate(wchar_t c) {
  return sizeof(wchar_t) == 2 && c >= 0xDC00 && c <= 0xDFFF;
}

}  // namespace

LearnFilter::LearnFilter(PersonalLexicon* lexicon, Scorer scorer)
    : lexicon_(lexicon), scorer_(std::move(scorer)) {
  worker_ = std::thread([this] { Loop(); });
}

LearnFilter::~LearnFilter() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  cv_.notify_all();
  if (worker_.joinable())
    worker_.join();
  // 還沒評分的照常學習，不丟掉
  for (const auto& item : queue_)
    lexicon_->Learn(item.window, item.text);
}

void LearnFilter::SetOptions(const Options& options) {
  std::lock_guard<std::mutex> lock(mutex_);
  options_ = options;
}

void LearnFilter::SetLogger(Logger logger) {
  std::lock_guard<std::mutex> lock(mutex_);
  logger_ = std::move(logger);
}

void LearnFilter::Submit(const std::wstring& window, const std::wstring& context,
                         const std::wstring& text) {
  if (text.empty())
    return;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back({window, context, text, NowMs()});
  }
  cv_.notify_all();
}

void LearnFilter::Discard() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
  }
  cv_.notify_all();
}

bool LearnFilter::WaitIdle(unsigned timeout_ms) {
  std::unique_lock<std::mutex> lock(mutex_);
  return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                      [this] { return queue_.empty() && !busy_; });
}

std::vector<std::pair<std::wstring, double>> LearnFilter::UnitScores(
    const std::wstring& text, const std::vector<double>& per_char) {
  std::vector<std::pair<std::wstring, double>> out;
  if (per_char.size() != text.size())
    return out;
  for (const auto& [start, length] : PersonalLexicon::UnitSpans(text)) {
    double sum = 0;
    size_t chars = 0;
    for (size_t i = start; i < start + length; ++i) {
      if (IsLowSurrogate(text[i]))
        continue;  // 擴充字元的分數在前半
      sum += per_char[i];
      ++chars;
    }
    if (chars > 0)
      out.emplace_back(text.substr(start, length), sum / chars);
  }
  return out;
}

std::vector<std::wstring> LearnFilter::LowScoreUnits(const std::wstring& text,
                                                     const std::vector<double>& per_char,
                                                     double min_logprob) {
  std::vector<std::wstring> out;
  for (const auto& [unit, score] : UnitScores(text, per_char)) {
    if (score < min_logprob)
      out.push_back(unit);
  }
  return out;
}

bool LearnFilter::Judge(const Item& item, const Options& options,
                        std::vector<std::wstring>* rejected, std::wstring* log) {
  rejected->clear();
  std::vector<double> per_char;
  const Score result = scorer_ ? scorer_(item.context, item.text, &per_char) : Score::kUnavailable;
  if (result == Score::kBusy)
    return false;
  if (result != Score::kOk)
    return true;
  if (reject_counts_.size() > 5000)
    reject_counts_.clear();
  for (const auto& [unit, score] : UnitScores(item.text, per_char)) {
    wchar_t buf[32];
    std::swprintf(buf, 32, L"(%.1f)", score);
    *log += L" " + unit + buf;
    size_t chars = 0;
    for (wchar_t ch : unit)
      chars += IsLowSurrogate(ch) ? 0 : 1;
    if (score >= options.min_logprob || chars < options.min_chars)
      continue;
    // 常打的詞不擋；同一個片段一再出現，多半是真的要打的
    if (lexicon_->WordScore(unit) >= options.known_score ||
        ++reject_counts_[unit] >= options.accept_after) {
      *log += L"放行";
      continue;
    }
    *log += L"不學";
    rejected->push_back(unit);
  }
  return true;
}

void LearnFilter::Loop() {
  // 關閉時中斷進行中的評分
  LLMCancelScope cancel([this] { return stop_.load(); });
  for (;;) {
    Item item;
    Options options;
    Logger logger;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
      if (stop_)
        return;
      // 送出後先等一下：送出後的預測馬上要用模型
      const uint64_t now = NowMs(), due = queue_.front().submitted_ms + options_.delay_ms;
      if (now < due) {
        cv_.wait_for(lock, std::chrono::milliseconds(due - now), [this] { return stop_.load(); });
        continue;
      }
      item = std::move(queue_.front());
      queue_.pop_front();
      busy_ = true;
      options = options_;
      logger = logger_;
    }
    std::vector<std::wstring> rejected;
    std::wstring log;
    while (!Judge(item, options, &rejected, &log)) {
      if (stop_ || NowMs() - item.submitted_ms > options.give_up_ms) {
        rejected.clear();
        log = L" 模型一直在忙，照常學習";
        break;
      }
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait_for(lock, std::chrono::milliseconds(options.busy_retry_ms),
                   [this] { return stop_.load(); });
    }
    lexicon_->Learn(item.window, item.text, rejected);
    if (logger && !log.empty())
      logger(L"[學習過濾]" + log);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      busy_ = false;
    }
    cv_.notify_all();
  }
}
