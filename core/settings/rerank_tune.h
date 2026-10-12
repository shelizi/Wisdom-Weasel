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

// 一種參數組合的成績
struct TuneOutcome {
  size_t top = 0;          // 最多評幾句
  double margin = 0;       // 門檻（整句 log 機率差）
  double accuracy = 0;     // 選出來的句子與使用者送出的一樣的比例
  double wrong_change = 0; // 初稿本來就對、卻被換掉的比例（佔初稿對的題數）
};

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
