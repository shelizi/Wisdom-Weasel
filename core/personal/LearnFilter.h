#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

class PersonalLexicon;

// 學習過濾：送出的文字先請本機模型評分，太不通順的片段（亂按、選錯字的怪句子）不學。
// - 以個人詞庫的學習單位（依標點切開的片段）為準：片段裡每個字的平均 log 機率低於門檻就擋下
// - 在背景執行緒依序處理，不擋打字；送出後先等一下（讓送出後的預測先用模型），模型忙時再等
// - 沒有可用的本機模型時照常全部學習
// - 已經常打的詞（詞庫分數夠高）不擋；同一個片段被擋幾次就視為真的要打的（人名、專有名詞）
// - 被擋下的片段仍寫進原始紀錄並標記，重建詞庫時一樣不學
class LearnFilter {
 public:
  enum class Score {
    kOk,           // per_char 為每個 wchar_t 的 log 機率
    kBusy,         // 模型正在忙（預測中），等一下再試
    kUnavailable,  // 沒有可用的本機模型，照常學習
  };
  // 評分：text 接在 context 後面時每個字的 log 機率（在背景執行緒呼叫）
  using Scorer = std::function<Score(const std::wstring& context, const std::wstring& text,
                                     std::vector<double>* per_char)>;

  struct Options {
    // 片段每字平均 log 機率低於這個就不學。MiniCPM5-2B 實測：通順的句子約 -1.5～-6.5，
    // 亂湊的字（蛤肚蝨吧、及和網那個）約 -9～-13，錯字句（謝謝你的邦忙）-7～-9
    double min_logprob = -7.5;
    size_t min_chars = 2;  // 單一個字沒有前文時分數很不準（「用」-12），不評
    unsigned delay_ms = 1500;    // 送出後等多久才評分
    unsigned busy_retry_ms = 200;
    unsigned give_up_ms = 30000;  // 模型一直在忙：超過這麼久就不評了，照常學習
    int accept_after = 3;         // 同一個片段被擋這麼多次後照常學習
    double known_score = 2.0;     // 詞庫分數到這裡的詞不擋
  };

  LearnFilter(PersonalLexicon* lexicon, Scorer scorer);
  ~LearnFilter();  // 停止；還沒評分的照常學習
  LearnFilter(const LearnFilter&) = delete;
  LearnFilter& operator=(const LearnFilter&) = delete;

  void SetOptions(const Options& options);
  // 每筆的評分結果（開發終端顯示用，在背景執行緒呼叫）
  using Logger = std::function<void(const std::wstring&)>;
  void SetLogger(Logger logger);
  // 排入一次送出：context 是這次送出之前的前文
  void Submit(const std::wstring& window, const std::wstring& context, const std::wstring& text);
  // 丟掉還沒評分的（清除個人詞庫時）
  void Discard();
  // 等佇列清空（測試用）
  bool WaitIdle(unsigned timeout_ms);

  // 每個片段的每字平均 log 機率（per_char 與 text 長度不同時回傳空的）
  static std::vector<std::pair<std::wstring, double>> UnitScores(
      const std::wstring& text, const std::vector<double>& per_char);
  // 平均低於門檻的片段
  static std::vector<std::wstring> LowScoreUnits(const std::wstring& text,
                                                 const std::vector<double>& per_char,
                                                 double min_logprob);

 private:
  struct Item {
    std::wstring window;
    std::wstring context;
    std::wstring text;
    uint64_t submitted_ms = 0;
  };
  void Loop();
  // 評分並挑出這次要擋的片段（log 附上各片段的分數）；回傳 false 表示模型在忙、要再等
  bool Judge(const Item& item, const Options& options, std::vector<std::wstring>* rejected,
             std::wstring* log);

  PersonalLexicon* lexicon_;
  Scorer scorer_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Item> queue_;
  Options options_;
  Logger logger_;
  bool busy_ = false;  // 背景執行緒正在處理一筆（已從佇列取出）
  std::unordered_map<std::wstring, int> reject_counts_;  // 只在背景執行緒用
  std::atomic<bool> stop_{false};
  std::thread worker_;
};
