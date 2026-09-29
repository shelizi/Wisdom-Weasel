#pragma once

// 選字統計（設定程式的「選字策略」頁顯示）：每天一行寫在使用者資料夾，只有次數、不含打字內容。
// 依「組合」（版本＋當時的設定）分開累計。
//
// weasel_stats.txt 每行：日期 \t 組合代碼 \t 送出 \t 字數 \t 換字 \t LLM 出現 \t LLM 採用 \t
//   校正採用 \t Backspace \t 送出後刪除 \t 逐字選字 \t 推薦出現 \t 推薦套用 \t
//   重排評估 \t 重排會改 \t 重排改對 \t 重排改錯（原本就對）
// weasel_stats_profiles.txt 每行：組合代碼 \t 版本 \t 編譯時間 \t 版本說明 \t 設定 \t 第一次出現
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>

namespace ime {

struct ChoiceStats {
  int64_t commits = 0;            // 送出次數
  int64_t chars = 0;              // 送出字數
  int64_t changed = 0;            // 其中換過候選（沒直接用第一候選）的次數
  int64_t llm_offered = 0;        // 其中出現過 LLM 候選的次數
  int64_t llm_used = 0;           // 採用 LLM 候選（Tab）的次數
  int64_t corrections_used = 0;   // 其中採用整句校正的次數
  int64_t backspaces = 0;         // 組字中按 Backspace 的次數
  int64_t deleted_after = 0;      // 送出後 10 秒內在應用程式裡按 Backspace 的次數（送錯字）
  int64_t focus_uses = 0;         // 用逐字選字（←/→ 框字）的次數
  int64_t recommend_offered = 0;  // 出現「推薦」的次數（每次組字最多算一次）
  int64_t recommend_used = 0;     // 按 Tab 套用推薦的次數
  // 整句重排 shadow 模式（只算不顯示）：送出時對照
  int64_t shadow_total = 0;        // 送出時有重排結果的次數
  int64_t shadow_changed = 0;      // 其中重排會換掉 Rime 第一句的次數
  int64_t shadow_rerank_right = 0;  // 會換，而且使用者送出的就是重排的句子
  int64_t shadow_rime_right = 0;    // 會換，但使用者送出的是 Rime 第一句（換了就錯）
};

// 本地日期 YYYY-MM-DD，days_ago 天前
std::string DateString(int days_ago = 0);

// 組合代碼：版本與設定的 FNV-1a 雜湊（8 個十六進位字）
std::string ProfileKey(const std::string& version, const std::string& settings);

class ChoiceStatsStore {
 public:
  explicit ChoiceStatsStore(std::filesystem::path dir) : dir_(std::move(dir)) {}

  // 今天、這個組合的統計（第一次用到時讀檔）
  ChoiceStats& Today(const std::string& profile);

  // 組合第一次出現時記到 weasel_stats_profiles.txt
  void AddProfile(const std::string& key,
                  const std::string& version,
                  const std::string& build_time,
                  const std::string& subject,
                  const std::string& settings);

  // 寫回 weasel_stats.txt，只留最近 90 天
  void Save();

  // 清除統計與兩個檔案
  void Reset();

 private:
  void Load();

  std::filesystem::path dir_;
  bool loaded_ = false;
  std::map<std::string, ChoiceStats> stats_;       // 日期 \t 組合代碼 → 統計
  std::map<std::string, std::string> profiles_;    // 組合代碼 → 整行
};

}  // namespace ime
