#pragma once

// 設定的操作（各平台共用）：模型檔、API 測試、注音方案的 custom.yaml、選字統計、個人詞庫、
// 預測測試、使用者詞典。字串一律 UTF-8；跟作業系統有關的部分經由 Platform。
#include <rime_levers_api.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "platform.h"

namespace settings {

extern const char kDefaultApiUrl[];
extern const char kGrammarUrl[];

// ---------------------------------------------------------------------------
// 文字與路徑

fs::path Path(const std::string& utf8);
std::string U8(const fs::path& path);
std::string ToLower(std::string s);  // 只轉 ASCII
std::string FileNameOf(const std::string& path);
std::string Trim(const std::string& s);  // 去掉首尾空白（含全形空白）與換行
std::string FormatSize(uint64_t bytes);
std::string FormatTime(int64_t t);  // 本機時間 YYYY/MM/DD HH:MM；t <= 0 時為「—」
std::string UrlDecode(const std::string& s);
// 輸入法回報的「目前載入」：本機模型是檔案路徑，只顯示檔名；OpenAI 相容 API 原樣顯示
std::string LoadedModelDisplay(const std::string& model);
// 依檔名猜模型類型：含 base → Base；含 instruct / chat / -it → Instruct；否則 nullptr
const char* GuessModelType(const std::string& path);

// ---------------------------------------------------------------------------
// 模型檔

bool IsGguf(const fs::path& path);
// 模型資料夾（與 current 所在的資料夾）裡的 GGUF，依檔名排序
std::vector<std::string> ScanModels(Platform& platform, const std::string& current);
struct ModelFile {
  std::string path;
  uint64_t size = 0;
};
std::vector<ModelFile> ListModelFiles(Platform& platform);
// 網址 → 下載網址與檔名；Hugging Face 的頁面網址（/blob/）換成下載網址（/resolve/）
std::string NormalizeModelUrl(std::string url, std::string* file_name);
using Progress = std::function<bool(uint64_t done, uint64_t total)>;
bool CopyFileWithProgress(const fs::path& src, const fs::path& dest, const Progress& progress,
                          std::string* error);

// OpenAI 相容 API 連線測試：送一個很小的請求，回傳給人看的結果（✓ / ✗ 開頭）
std::string TestApi(const std::string& api_url, const std::string& api_key, const std::string& model);

// ---------------------------------------------------------------------------
// 注音方案的 custom.yaml：只增刪我們自己的標記區塊，保留使用者原有的設定與註解

bool ApplyRimeBoost(Platform& platform, RimeLeversApi* api, RimeSwitcherSettings* switcher,
                    bool enable, std::string* error);
bool ApplyTypoCorrection(Platform& platform, bool enable, std::string* error);
bool ApplyGrammar(Platform& platform, bool enable, std::string* error);
bool GrammarEnabled(Platform& platform);  // 注音方案裡有我們加的區塊
fs::path GrammarPath(Platform& platform);
bool GrammarReady(Platform& platform);  // 模型檔已下載

// ---------------------------------------------------------------------------
// 選字統計（weasel_stats.txt、weasel_stats_profiles.txt）

struct StatsRow {
  std::string version, settings;
  int64_t commits = 0, changed = 0;
  std::string first_ok, deleted, recommended, llm;  // 百分比或 n／d
  std::string detail;
};
// span_days：1 = 今天、7、30；0 = 全部。有兩種以上的組合時第一列是合計
std::vector<StatsRow> ChoiceStats(Platform& platform, int span_days);
void ResetChoiceStats(Platform& platform);  // 請輸入法清除；輸入法沒回應時直接刪檔
struct ChoiceLogInfo {
  size_t records = 0;
  uint64_t bytes = 0;
};
ChoiceLogInfo ChoiceLogStatus(Platform& platform);  // 只數筆數不解密
bool ClearChoiceLog(Platform& platform);

// ---------------------------------------------------------------------------
// 個人詞庫（經由輸入法讀寫）

fs::path PersonalDir(Platform& platform);
std::string PersonalStatusText(Platform& platform, bool* running);
struct WordRule {
  enum Kind { kAdd, kBlock, kMerge, kSplit } kind;
  std::string from, to;
};
struct PersonalWords {
  bool available = false;  // false：個人詞庫關閉或輸入法沒回應
  std::string error;
  std::vector<std::pair<std::string, double>> words;
  std::vector<WordRule> rules;
};
PersonalWords LoadPersonalWords(Platform& platform);
// 修改：A 加入、M 合併、R 封鎖、D 刪除、X/Y/U/Z 移除合併／加入／封鎖／拆解規則
bool SendWordEdits(Platform& platform, const std::vector<std::string>& lines, std::string* error);

// ---------------------------------------------------------------------------
// 預測測試：寫入請求檔 → 通知輸入法 → 輪詢回覆檔（不等待輸入法的鎖，避免卡住）

bool SendLLMTestRequest(Platform& platform, unsigned id, const std::string& context);
struct LLMTestResponse {
  std::string status;  // disabled 表示 LLM 智慧預測關閉
  std::string model;
  std::string ms;
  std::vector<std::string> candidates;
};
std::optional<LLMTestResponse> PollLLMTestResponse(Platform& platform, unsigned id);

// ---------------------------------------------------------------------------
// 使用者詞典

std::vector<std::string> ListUserDicts(RimeLeversApi* api);
void PrepareUserDictTask();  // 使用者資料同步資料夾要先建立（installation_update）

// 操作使用者詞典期間暫停輸入法
class ServiceMaintenance {
 public:
  explicit ServiceMaintenance(Platform& platform) : platform_(platform) { platform_.StartMaintenance(); }
  ~ServiceMaintenance() { platform_.EndMaintenance(); }
  ServiceMaintenance(const ServiceMaintenance&) = delete;
  ServiceMaintenance& operator=(const ServiceMaintenance&) = delete;

 private:
  Platform& platform_;
};

}  // namespace settings
