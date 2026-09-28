#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// 個人詞庫：從使用者送出的文字學習「常用詞」與「某個詞後面通常接什麼」。
//
// - 以每次送出的文字為單位（依標點切開），記錄單詞頻率與「前一個 → 下一個」的接續次數
// - 分數隨時間衰減（半衰期），久沒用的詞自然淘汰；存檔時只保留分數最高的一批
// - 詞庫與原始輸入紀錄都用 Windows DPAPI 加密存在使用者資料夾，只有目前的帳號能解開
// - 查詢只讀記憶體，不碰硬碟；存檔在背景執行緒進行
class PersonalLexicon {
 public:
  explicit PersonalLexicon(std::filesystem::path dir);
  ~PersonalLexicon();

  // 載入（檔案不存在時為空詞庫）
  bool Load();
  // 立即存檔（服務結束、清除詞庫時呼叫）
  bool Save();

  // 記錄一次送出（= NoteCommit + Learn）。window 用來區分視窗，讓「前一個詞」不會跨視窗接錯
  void Record(const std::wstring& window, const std::wstring& text);
  // 學習過濾把一次送出拆成兩步：
  // NoteCommit：送出當下記下這個視窗最後的詞（NextAfter 馬上用得到），不學
  void NoteCommit(const std::wstring& window, const std::wstring& text);
  // Learn：評分後計入統計並寫原始紀錄。rejected 是擋下的片段，不學、也不拿來接續
  // （原始紀錄照樣保留並標記這些片段）
  void Learn(const std::wstring& window, const std::wstring& text,
             const std::vector<std::wstring>& rejected = {});
  // 學習的單位：依標點切開的片段在 text 裡的起點與長度（太長的片段不含在內）
  static std::vector<std::pair<size_t, size_t>> UnitSpans(const std::wstring& text);
  // 詞目前的分數（不在詞庫裡為 0）
  double WordScore(const std::wstring& word) const;

  // 目前視窗最後送出的詞之後，最常接的詞
  std::vector<std::wstring> NextAfter(const std::wstring& window, size_t max_count) const;
  // 以 prefix 開頭的常用詞，以及 prefix 後面最常接的詞（回傳 prefix + 詞）
  std::vector<std::wstring> CompleteFrom(const std::wstring& prefix, size_t max_count) const;

  void SetHalfLifeDays(double days) { half_life_days_ = days > 0 ? days : 30; }
  void SetKeepRawLog(bool keep) { keep_raw_log_ = keep; }

  // 清空詞庫、原始紀錄（含封存）與精煉結果
  void Clear();

  size_t WordCount() const;
  size_t PairCount() const;
  // 最後一次送出文字的時間（Unix 秒）；定時精煉只在閒置時進行
  int64_t LastActivity() const { return last_activity_; }

  // ---- 原始語料與精煉 ----
  // 原始語料永久保留：累積中的紀錄在 input_log.dat；精煉後封存到 archive/ 並重新累積
  struct RawRecord {
    int64_t time = 0;
    std::wstring window;
    std::wstring text;
    std::vector<std::wstring> rejected;  // 學習過濾擋下的片段（重建時一樣不學）
  };
  const std::filesystem::path& Dir() const { return dir_; }
  std::filesystem::path ActiveLogPath() const { return dir_ / L"input_log.dat"; }
  std::filesystem::path ArchiveDir() const { return dir_ / L"archive"; }
  std::vector<std::filesystem::path> ArchivedLogs() const;  // 依時間排序
  static std::vector<RawRecord> ReadRawLog(const std::filesystem::path& path);
  static size_t CountRawRecords(const std::filesystem::path& path);  // 不解密，只數筆數
  // 把累積中的紀錄移到 archive/（檔名含時間），之後重新累積；沒有紀錄時不做事
  bool ArchiveActiveLog(std::filesystem::path* archived = nullptr);
  // 用原始紀錄從頭重建統計（依紀錄的真實時間計算衰減）。重建期間新送出的文字會在重建後補上
  void Rebuild(std::vector<RawRecord> records);

  // 精煉結果：刪除的詞之後不再學習；合併的寫法之後都算到正確寫法
  void ApplyRefinement(const std::vector<std::wstring>& removals,
                       const std::vector<std::pair<std::wstring, std::wstring>>& merges);
  // 拆解：LLM 把太長的片段（多半是整句）拆成詞或片語，之後打到這個片段時改學拆出來的部分，
  // 部分之間照順序接續。原片段的分數與接續移到拆出來的部分。
  // parts 依序接起來要和原片段相同、至少兩段，否則略過
  using Split = std::pair<std::wstring, std::vector<std::wstring>>;
  void ApplySplits(const std::vector<Split>& splits);
  std::vector<Split> Splits() const;
  std::vector<std::pair<std::wstring, double>> TopWords(size_t max_count) const;
  // 全部的詞（依分數排序），含最後使用時間、是否已被 LLM 審查過、是否已看過要不要拆
  struct WordInfo {
    std::wstring word;
    double score = 0;
    int64_t last = 0;
    bool reviewed = false;
    bool split_checked = false;
  };
  std::vector<WordInfo> WordInfos() const;
  // 記住這些詞已審查過（分批審查：下次只送還沒審查的詞）
  void MarkReviewed(const std::vector<std::wstring>& words);
  // 記住這些片段已請 LLM 看過要不要拆（不用拆的也記，下次不再送）
  void MarkSplitChecked(const std::vector<std::wstring>& words);
  int64_t LastRefine() const;
  void SetLastRefine(int64_t time);

  // ---- 手動維護（設定程式的「詞庫管理」）----
  void AddWord(const std::wstring& word);  // 加入詞（解除封鎖/合併，給起始分數）
  // 只刪除（不留規則）：之後再打到、或從原始紀錄重建時還會再學到
  void DeleteWords(const std::vector<std::wstring>& words);
  void RemoveRules(const std::vector<std::wstring>& unblock,
                   const std::vector<std::wstring>& unmerge,
                   const std::vector<std::wstring>& unadd = {});
  void Rules(std::vector<std::wstring>* removed,
             std::vector<std::pair<std::wstring, std::wstring>>* merged,
             std::vector<std::wstring>* added = nullptr) const;
  // 匯出詞彙與規則（加密）給設定程式；套用設定程式寫的修改檔，回傳處理筆數（-1 = 讀不到）
  bool ExportTo(const std::filesystem::path& path, size_t max_words) const;
  int ApplyEdits(const std::filesystem::path& path);

 private:
  struct Score {
    double value = 0;       // 以 last 時刻計的分數
    int64_t last = 0;       // 最後更新時間（Unix 秒）
  };

  double Decayed(const Score& s, int64_t now) const;
  void Bump(Score& s, int64_t now) const;
  void MaybeSaveAsync();
  bool SaveLocked(std::string* blob) const;  // 在鎖內序列化
  void AppendRawLog(const std::wstring& window, const std::wstring& text,
                    const std::vector<std::wstring>& rejected, int64_t now);
  // 在鎖內把一次送出計入統計（套用精煉結果：略過刪除的詞、合併的寫法換成正確寫法；
  // rejected 的片段也略過）
  void RecordLocked(const std::wstring& window, const std::wstring& text, int64_t now,
                    const std::vector<std::wstring>& rejected = {});
  void NoteLocked(const std::wstring& window, const std::wstring& text);
  // 一個片段實際要學的詞：套用合併與拆解；刪除的詞換成空字串（中斷接續）
  std::vector<std::wstring> ExpandLocked(const std::wstring& unit) const;
  bool SaveRefinementLocked(std::string* blob) const;
  void LoadRefinement();

  static std::vector<std::wstring> SplitUnits(const std::wstring& text, bool* sentence_end);

  std::filesystem::path dir_;
  mutable std::mutex mutex_;
  std::unordered_map<std::wstring, Score> words_;                                  // 單詞
  std::unordered_map<std::wstring, std::unordered_map<std::wstring, Score>> next_;  // 前 → 後
  std::unordered_map<std::wstring, std::wstring> last_word_;   // 每個視窗最後送出的詞（查接續用）
  std::unordered_map<std::wstring, std::wstring> chain_word_;  // 每個視窗最後學到的詞（學接續用）
  double half_life_days_ = 30;
  bool keep_raw_log_ = true;
  int dirty_ = 0;
  int64_t last_save_ = 0;
  bool saving_ = false;
  bool persist_ = true;  // false：Rebuild 用的暫存統計，不存檔
  std::atomic<int64_t> last_activity_{0};

  std::unordered_set<std::wstring> removed_;                // 精煉刪除的詞
  std::unordered_map<std::wstring, std::wstring> merged_;   // 精煉合併：原寫法 → 正確寫法
  std::unordered_set<std::wstring> reviewed_;               // 已被 LLM 審查過的詞
  std::unordered_map<std::wstring, std::vector<std::wstring>> splits_;  // 拆解：片段 → 拆出來的部分
  std::unordered_set<std::wstring> split_checked_;          // 已看過要不要拆的片段
  std::unordered_map<std::wstring, int64_t> added_;         // 手動加入的詞 → 加入時間（重建後補回）
  int64_t last_refine_ = 0;
  bool rebuilding_ = false;
  std::vector<RawRecord> pending_;  // 重建期間送出的文字，重建後補上
};
