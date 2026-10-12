#pragma once

// 整句重排的自動調整（設定程式「選字策略」頁的「自動調整」按鈕）：
// 讀選字紀錄裡使用者自己的 Rime Top-K 整句（整句重排評過分的那些筆），
// 用現在的評分模型重新評分，掃「門檻 × 評幾句」的網格，回傳 Top-1 最高的組合。
// 資料是使用者自己的文字（有選字記憶的 Rime 的結果），正是文件說沙盒評估看不到的部分。
// 推理在呼叫端的執行緒跑；cancel 設 true 時盡快停止。
#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

class LLMProvider;

namespace settings_rerank {

// 一筆可用的選字紀錄：當時的整句候選（sentences[0] 是預設轉換）與使用者送出的句子
struct LoggedCase {
  int64_t time = 0;
  std::wstring context;
  std::wstring context_key;  // 前文視窗 key（同一份前文的送出序列；訓練重建用）
  std::wstring zhuyin;
  std::string method;  // 送出方式（direct/changed/focus/llm/correction/mixed）
  std::string rec;     // 顯示中的建議（kind:gain；"offered" = 出現過沒記 gain）
  std::string took;    // 套用的建議種類
  std::wstring draft;                   // Rime 當時的預設轉換
  std::wstring truth;                   // 使用者實際送出（reverted 配對成功時是修正後的）
  std::vector<std::wstring> sentences;  // 當時的整句候選（最多 20 句）
  std::vector<double> personal;         // 每句的個人詞庫分數（舊紀錄沒有是空的）
};

// 從 personal_dir/choice_log.dat 讀出有 Top-K 的紀錄（解密或格式不對的略過）。
// 回傳略過的筆數
size_t LoadLoggedTopK(const std::filesystem::path& personal_dir,
                      std::vector<LoggedCase>* cases);

// 每筆紀錄的輕量資訊（只取需要的欄位；劣化偵測、資料量統計用）
struct LoggedMeta {
  int64_t time = 0;       // 毫秒
  std::string method;     // direct/changed/focus/llm/correction/mixed/reverted
  bool has_topk = false;  // 有整句候選
};
// 讀全部紀錄的 meta（不管有沒有 Top-K；解密失敗略過）。回傳略過的筆數
size_t LoadLoggedMeta(const std::filesystem::path& personal_dir,
                      std::vector<LoggedMeta>* out);

// 「評幾句」與「門檻」的網格（Tune 的掃描空間，也是漸進調整的步長）
const std::vector<size_t>& TopGrid();
const std::vector<double>& MarginGrid();
constexpr size_t kMaxRecordedTop = 20;  // 紀錄與調參的句數上限

// 一種組合選出的句子：非第一句比第一句好超過 margin 才換；換不了就維持初稿。
// 計分比照線上的 RerankScore（長度正規化＋個人詞庫加分）；第一句不是預設轉換時不換（保守）
const std::wstring& Choose(const LoggedCase& c, const std::vector<double>& lm, size_t top,
                           double margin, double alpha, double personal_weight);

// 一筆已用模型重評分過的紀錄
struct ScoredCase {
  const LoggedCase* c;
  std::vector<double> lm;  // 各候選的模型分數（失敗的是 NaN）
};
// 每筆一次 ScoreBatch；評分失敗整筆補 NaN（fail-open 同輸入法）；可取消、回報進度
std::vector<ScoredCase> ScoreCases(LLMProvider* scorer,
                                   const std::vector<LoggedCase>& cases,
                                   size_t context_chars,
                                   const std::atomic<bool>& cancel,
                                   const std::function<void(size_t, size_t)>& progress);

// 一種參數組合的成績
struct TuneOutcome {
  size_t top = 0;          // 最多評幾句
  double margin = 0;       // 門檻（整句 log 機率差）
  double accuracy = 0;     // 選出來的句子與使用者送出的一樣的比例
  double wrong_change = 0; // 初稿本來就對、卻被換掉的比例（佔初稿對的題數）
};

// 一組參數在已評分資料上的成績
TuneOutcome EvalParams(const std::vector<ScoredCase>& items, size_t top, double margin,
                       double alpha, double personal_weight);

struct TuneReport {
  size_t cases = 0;    // 拿來評的題數（評分成功的紀錄）
  size_t scored = 0;   // 評分的句子數
  bool cancelled = false;
  TuneOutcome best;               // 準確率最高；同分取門檻較大、再取評較少句的
  std::vector<TuneOutcome> grid;  // 全部組合（top、margin 都由小到大）
};

// 每筆的整句送 ScoreBatch 重新評分後掃網格；每評完一筆呼叫 progress(done, total)。
// length_alpha／personal_weight 要給表單目前的值：紀錄裡有每句的個人詞庫分數，
// 掃出來的門檻才對得上線上的計分方式。
TuneReport Tune(LLMProvider* scorer,
                const std::vector<LoggedCase>& cases,
                size_t context_chars,
                double length_alpha,
                double personal_weight,
                const std::atomic<bool>& cancel,
                const std::function<void(size_t, size_t)>& progress);

}  // namespace settings_rerank
