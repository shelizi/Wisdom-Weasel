#pragma once

// 設定程式裡與介面無關的操作：舊的設定對話框（SettingsDialog）與網頁版設定（WebSettings）共用。
#include <rime_levers_api.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace settings_ops {

namespace fs = std::filesystem;

extern const wchar_t kDefaultApiUrl[];

// ---------------------------------------------------------------------------
// 文字與路徑

std::wstring ToLower(std::wstring s);
std::wstring FileNameOf(const std::wstring& path);
std::wstring Trim(const std::wstring& s);  // 去掉首尾空白（含全形空白）與換行
std::wstring FormatSize(unsigned long long bytes);
std::wstring FormatTime(int64_t t);  // 本機時間 YYYY/MM/DD HH:MM；t <= 0 時為「—」
// 輸入法回報的「目前載入」：本機模型是檔案路徑，只顯示檔名；OpenAI 相容 API 原樣顯示
std::wstring LoadedModelDisplay(const std::wstring& model);
// 依檔名猜模型類型：含 base → Base；含 instruct / chat / -it → Instruct；否則 nullptr
const wchar_t* GuessModelType(const std::wstring& path);

// ---------------------------------------------------------------------------
// 檔案總管與檔案對話框

// 開啟檔案所在資料夾並選取檔案
void OpenFolderAndSelectItem(std::wstring filepath);
struct FileFilter {
  std::wstring name;
  std::wstring spec;
};
// 開檔／存檔對話框；取消時回傳空字串
std::wstring OpenFileDialog(void* owner, const std::wstring& title,
                            const std::vector<FileFilter>& filters, const wchar_t* file_name,
                            const wchar_t* def_ext);
std::wstring SaveFileDialog(void* owner, const std::wstring& title,
                            const std::vector<FileFilter>& filters, const wchar_t* file_name,
                            const wchar_t* def_ext);
// 移到資源回收筒
bool MoveToRecycleBin(const std::wstring& path);

// ---------------------------------------------------------------------------
// 模型檔（%USERPROFILE%\models 裡的 GGUF）

fs::path ModelsDir();
bool IsGguf(const fs::path& path);
// 模型資料夾（與 current 所在的資料夾）裡的 GGUF，依檔名排序
std::vector<std::wstring> ScanModels(const std::wstring& current);
struct ModelFile {
  std::wstring path;
  unsigned long long size = 0;
};
std::vector<ModelFile> ListModelFiles();
// 網址 → 下載網址與檔名；Hugging Face 的頁面網址（/blob/）換成下載網址（/resolve/）
std::wstring NormalizeModelUrl(std::wstring url, std::wstring* file_name);
// 進度回呼回傳 false 時中止
using Progress = std::function<bool(unsigned long long done, unsigned long long total)>;
// 以 WinHTTP 下載（自動跟隨轉址，例如 Hugging Face → CDN）
bool HttpDownload(const std::wstring& url, const fs::path& dest, const Progress& progress,
                  std::wstring* error);
bool CopyFileWithProgress(const fs::path& src, const fs::path& dest, const Progress& progress,
                          std::wstring* error);
// 檔案被輸入法載入中時會被鎖住，無法刪除
bool FileInUse(const std::wstring& path);

// ---------------------------------------------------------------------------
// OpenAI 相容 API 連線測試：送一個很小的請求，回傳給人看的結果（✓ / ✗ 開頭）
std::wstring TestApi(const std::wstring& api_url, const std::wstring& api_key,
                     const std::wstring& model);

// ---------------------------------------------------------------------------
// 注音方案的 custom.yaml：只增刪我們自己的標記區塊，保留使用者原有的設定與註解

// 注音排序：注音方案改用 terra_pinyin.personal 詞典（個人詞庫的常用詞影響選字）
bool ApplyRimeBoost(RimeLeversApi* api, RimeSwitcherSettings* switcher, bool enable,
                    std::wstring* error);
// 注音容錯：打開 Rime 的拼寫糾錯
bool ApplyTypoCorrection(bool enable, std::wstring* error);
// 選字策略：octagram 語言模型
bool ApplyGrammar(bool enable, std::wstring* error);
bool GrammarEnabled();  // 注音方案裡有我們加的區塊
fs::path GrammarPath();
bool GrammarReady();  // 模型檔已下載
extern const wchar_t kGrammarUrl[];

// ---------------------------------------------------------------------------
// 選字統計（weasel_stats.txt、weasel_stats_profiles.txt）

struct StatsRow {
  std::wstring version, settings;
  int64_t commits = 0, changed = 0;
  std::wstring first_ok, deleted, recommended, llm;  // 百分比或 n／d
  std::wstring detail;
};
// span_days：1 = 今天、7、30；0 = 全部。有兩種以上的組合時第一列是合計
std::vector<StatsRow> ChoiceStats(int span_days);
bool ResetChoiceStats();  // 請輸入法清除；輸入法沒回應時直接刪檔
// 選字紀錄（personal/choice_log.dat）：只數筆數不解密
struct ChoiceLogInfo {
  size_t records = 0;
  unsigned long long bytes = 0;
};
ChoiceLogInfo ChoiceLogStatus();
bool ClearChoiceLog();

// ---------------------------------------------------------------------------
// 個人詞庫（經由輸入法讀寫）

fs::path PersonalDir();
// 1 更新狀態、2 精煉、3 全部重新精煉、4 清除、5 匯出詞彙、6 套用修改、7 產生注音排序詞典、8 清除統計
bool SendPersonalCommand(unsigned long command);
// 狀態文字（與舊設定視窗顯示的相同）；running 表示精煉中
std::wstring PersonalStatusText(bool* running);
struct WordRule {
  enum Kind { kAdd, kBlock, kMerge } kind;
  std::wstring from, to;
};
struct PersonalWords {
  bool available = false;  // false：個人詞庫關閉或輸入法沒回應
  std::wstring error;
  std::vector<std::pair<std::wstring, double>> words;
  std::vector<WordRule> rules;
};
PersonalWords LoadPersonalWords();
// 修改：A 加入、M 合併、R 封鎖、D 刪除、X/Y/U 移除合併／加入／封鎖規則
bool SendWordEdits(const std::vector<std::wstring>& lines, std::wstring* error);

// ---------------------------------------------------------------------------
// 預測測試：寫入請求檔 → 通知輸入法 → 輪詢回覆檔（不等待輸入法的鎖，避免卡住）

bool SendLLMTestRequest(unsigned id, const std::wstring& context);
struct LLMTestResponse {
  std::string status;  // disabled 表示 LLM 智慧預測關閉
  std::wstring model;
  std::string ms;
  std::vector<std::wstring> candidates;
};
std::optional<LLMTestResponse> PollLLMTestResponse(unsigned id);

// ---------------------------------------------------------------------------
// 使用者詞典（原本的「用戶詞典管理」）

std::vector<std::wstring> ListUserDicts(RimeLeversApi* api);
// 暫停輸入法服務（詞典檔由服務開著），解構時恢復
class ServiceMaintenance {
 public:
  ServiceMaintenance();
  ~ServiceMaintenance();
  ServiceMaintenance(const ServiceMaintenance&) = delete;
  ServiceMaintenance& operator=(const ServiceMaintenance&) = delete;
};
// 使用者資料同步資料夾要先建立（installation_update）
void PrepareUserDictTask();

}  // namespace settings_ops
