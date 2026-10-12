#pragma once

// 整句重排的自動漸進調整與防劣化（「自動微調」）：
// 排程或手動觸發時重評選字紀錄，只准「驗證不退化」的一步移動——每次最多
// 朝網格上更好的方向走一格（margin 與 top 各自）。套用後若接下來的紀錄
// 顯示修正率明顯變差，下次執行先回滾到前一組參數。
// 所有狀態都在 personal/ 下（tune_history.log 是 JSON lines、auto_tune.json
// 是開關與上次執行狀態）；不含使用者文字，不用加密。
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "rerank_tune.h"

class LLMProvider;

namespace settings_rerank {

// tune_history.log 的一筆：這次執行的決定
struct TuneEntry {
  int64_t ts = 0;            // 寫入時間（ms）
  std::string action;        // "apply"｜"rollback"｜"hold"｜"skip"
  size_t top = 0;            // 這次生效的「評幾句」（apply/rollback）
  double margin = 0;         // 這次生效的門檻
  size_t prev_top = 0;       // 前一組參數（rollback 時還原用）
  double prev_margin = 0;
  size_t cases = 0;          // 這次評的筆數
  double acc_prev = 0;       // 前組參數在這批資料上的準確率
  double acc_best = 0;       // 網格最佳的準確率（參考）
  double wrong_next = 0;     // 套用組的錯改率
  int64_t apply_rec_ms = 0;  // 套用時最新紀錄的時間（切分前後窗口）
  std::string note;          // 人看的說明
};

std::vector<TuneEntry> LoadTuneHistory(const std::filesystem::path& personal_dir);
bool AppendTuneHistory(const std::filesystem::path& personal_dir, const TuneEntry& e);

// 自動微調的開關與執行狀態（personal/auto_tune.json）
struct AutoState {
  bool enabled = false;          // 使用者開了「自動微調」
  int64_t last_run_ms = 0;       // 上次執行（ms）
  size_t last_records = 0;       // 上次執行時的紀錄筆數
  int64_t cooldown_until_ms = 0; // 回滾後的冷卻期（這之前不再自動調）
};
AutoState LoadAutoState(const std::filesystem::path& personal_dir);
bool SaveAutoState(const std::filesystem::path& personal_dir, const AutoState& s);

// 劣化偵測：套用點前後的「修正率」（method 是 reverted/changed/focus 的比例）。
// 後段紀錄不夠多時不下判斷（judgeable=false）
struct DegradeCheck {
  bool judgeable = false;
  bool degraded = false;
  double before_rate = 0;  // 套用前窗口的修正率
  double after_rate = 0;   // 套用後到現在的修正率
  size_t after_n = 0;
};
DegradeCheck CheckDegradation(const std::vector<LoggedMeta>& meta, int64_t apply_rec_ms,
                              size_t min_after = 50);

// 漸進決策：網格上比現在好且錯改沒變差的組合，每維只走一格
struct GuardedDecision {
  bool apply = false;          // 要不要換
  size_t top = 0;              // 建議的新參數（走一步後的）
  double margin = 0;
  size_t best_top = 0;         // 網格最佳（只走一格，未必到）
  double best_margin = 0;
  TuneOutcome current;         // 目前參數的成績
  TuneOutcome best;            // 網格最佳的成績
  TuneOutcome applied;         // 建議組（一步後）的成績
  std::string reason;
};
GuardedDecision DecideGuarded(const std::vector<ScoredCase>& items,
                              size_t cur_top, double cur_margin,
                              double alpha, double personal_weight);

// 自動執行一輪（呼叫端已載好評分模型；pause/resume 由呼叫端處理）：
// 1) 劣化偵測 → 該回滾就回 {action:"rollback", params}
// 2) 否則評分＋防護決策 → {action:"apply"|"hold", params}
// 不寫設定檔：回傳的 params 由呼叫端套用（settings.apply / rerank.apply）
struct AutoRunResult {
  std::string action;        // "apply"｜"rollback"｜"hold"｜"skip"｜"error"
  size_t top = 0;            // action 是 apply/rollback 時要套用的參數
  double margin = 0;
  TuneEntry entry;           // 已寫進 history 的這筆
  DegradeCheck check;
  GuardedDecision decision;
  size_t records = 0;        // 目前紀錄總筆數
  std::string message;       // 人看的結果
};
AutoRunResult AutoTuneRun(LLMProvider* scorer,
                          const std::filesystem::path& personal_dir,
                          size_t cur_top, double cur_margin,
                          size_t context_chars, double alpha, double personal_weight,
                          const std::atomic<bool>& cancel,
                          const std::function<void(size_t, size_t)>& progress);

}  // namespace settings_rerank
