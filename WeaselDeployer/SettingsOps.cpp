// 舊的設定視窗（SettingsDialog）用的介面：UTF-16 字串，內部轉給共用的 core/settings
// 與 Windows 平台功能（WinSettingsPlatform）。網頁版設定直接用 core/settings。
#include "stdafx.h"
#include "SettingsOps.h"
#include "WinSettingsPlatform.h"
#include "../core/net/http.h"
#include "../core/settings/ops.h"
#include <WeaselUtility.h>
#include <algorithm>

namespace settings_ops {

namespace core = settings;

namespace {

WinSettingsPlatform& Platform() {
  return WinSettingsPlatform::Shared();
}

std::string U8(const std::wstring& s) {
  return wtou8(s);
}

std::wstring W(const std::string& s) {
  return u8tow(s);
}

std::vector<std::wstring> W(const std::vector<std::string>& list) {
  std::vector<std::wstring> result;
  for (const auto& s : list)
    result.push_back(W(s));
  return result;
}

std::vector<core::FileFilter> Filters(const std::vector<FileFilter>& filters) {
  std::vector<core::FileFilter> result;
  for (const auto& f : filters)
    result.push_back({U8(f.name), U8(f.spec)});
  return result;
}

// 錯誤訊息等輸出參數：轉成 UTF-16
struct ErrorOut {
  std::wstring* target;
  std::string value;
  explicit ErrorOut(std::wstring* t) : target(t) {}
  ~ErrorOut() {
    if (target && !value.empty())
      *target = W(value);
  }
};

core::Progress Wrap(const Progress& progress) {
  return [progress](uint64_t done, uint64_t total) { return progress(done, total); };
}

}  // namespace

const wchar_t kDefaultApiUrl[] = L"https://api.openai.com/v1/chat/completions";
const wchar_t kGrammarUrl[] =
    L"https://raw.githubusercontent.com/lotem/rime-octagram-data/hant/zh-hant-t-essay-bgw.gram";

// ---------------------------------------------------------------------------
// 文字與路徑

std::wstring ToLower(std::wstring s) {
  std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
  return s;
}

std::wstring FileNameOf(const std::wstring& path) {
  return fs::path(path).filename().wstring();
}

std::wstring Trim(const std::wstring& s) {
  return W(core::Trim(U8(s)));
}

std::wstring FormatSize(unsigned long long bytes) {
  return W(core::FormatSize(bytes));
}

std::wstring FormatTime(int64_t t) {
  return W(core::FormatTime(t));
}

std::wstring LoadedModelDisplay(const std::wstring& model) {
  return W(core::LoadedModelDisplay(U8(model)));
}

const wchar_t* GuessModelType(const std::wstring& path) {
  const char* type = core::GuessModelType(U8(path));
  return !type ? nullptr : std::string(type) == "Base" ? L"Base" : L"Instruct";
}

// ---------------------------------------------------------------------------
// 檔案總管與檔案對話框

void OpenFolderAndSelectItem(std::wstring filepath) {
  Platform().Reveal(filepath);
}

std::wstring OpenFileDialog(void* owner, const std::wstring& title,
                            const std::vector<FileFilter>& filters, const wchar_t* file_name,
                            const wchar_t* def_ext) {
  return Platform()
      .OpenFileDialog(owner, U8(title), Filters(filters), file_name ? U8(file_name) : "",
                      def_ext ? U8(def_ext) : "")
      .wstring();
}

std::wstring SaveFileDialog(void* owner, const std::wstring& title,
                            const std::vector<FileFilter>& filters, const wchar_t* file_name,
                            const wchar_t* def_ext) {
  return Platform()
      .SaveFileDialog(owner, U8(title), Filters(filters), file_name ? U8(file_name) : "",
                      def_ext ? U8(def_ext) : "")
      .wstring();
}

bool MoveToRecycleBin(const std::wstring& path) {
  return Platform().MoveToTrash(path);
}

std::vector<std::wstring> ListSystemFonts() {
  return W(Platform().SystemFonts());
}

// ---------------------------------------------------------------------------
// 模型檔

fs::path ModelsDir() {
  return Platform().ModelsDir();
}

bool IsGguf(const fs::path& path) {
  return core::IsGguf(path);
}

std::vector<std::wstring> ScanModels(const std::wstring& current) {
  return W(core::ScanModels(Platform(), U8(current)));
}

std::vector<ModelFile> ListModelFiles() {
  std::vector<ModelFile> files;
  for (const auto& f : core::ListModelFiles(Platform()))
    files.push_back({W(f.path), f.size});
  return files;
}

std::wstring NormalizeModelUrl(std::wstring url, std::wstring* file_name) {
  std::string name;
  const std::string result = core::NormalizeModelUrl(U8(url), &name);
  *file_name = W(name);
  return W(result);
}

bool HttpDownload(const std::wstring& url, const fs::path& dest, const Progress& progress,
                  std::wstring* error) {
  ErrorOut out(error);
  return net::Download(U8(url), dest, Wrap(progress), &out.value);
}

bool CopyFileWithProgress(const fs::path& src, const fs::path& dest, const Progress& progress,
                          std::wstring* error) {
  ErrorOut out(error);
  return core::CopyFileWithProgress(src, dest, Wrap(progress), &out.value);
}

bool FileInUse(const std::wstring& path) {
  return Platform().FileInUse(path);
}

std::wstring TestApi(const std::wstring& api_url, const std::wstring& api_key,
                     const std::wstring& model) {
  std::wstring text = W(core::TestApi(U8(api_url), U8(api_key), U8(model)));
  // 多行編輯框要 \r\n
  for (size_t pos = 0; (pos = text.find(L'\n', pos)) != std::wstring::npos; pos += 2)
    text.insert(pos, 1, L'\r');
  return text;
}

// ---------------------------------------------------------------------------
// 注音方案的 custom.yaml

bool ApplyRimeBoost(RimeLeversApi* api, RimeSwitcherSettings* switcher, bool enable,
                    std::wstring* error) {
  ErrorOut out(error);
  return core::ApplyRimeBoost(Platform(), api, switcher, enable, &out.value);
}

bool ApplyTypoCorrection(bool enable, std::wstring* error) {
  ErrorOut out(error);
  return core::ApplyTypoCorrection(Platform(), enable, &out.value);
}

bool ApplyGrammar(bool enable, std::wstring* error) {
  ErrorOut out(error);
  return core::ApplyGrammar(Platform(), enable, &out.value);
}

bool GrammarEnabled() {
  return core::GrammarEnabled(Platform());
}

fs::path GrammarPath() {
  return core::GrammarPath(Platform());
}

bool GrammarReady() {
  return core::GrammarReady(Platform());
}

// ---------------------------------------------------------------------------
// 選字統計

std::vector<StatsRow> ChoiceStats(int span_days) {
  std::vector<StatsRow> rows;
  for (const auto& r : core::ChoiceStats(Platform(), span_days)) {
    StatsRow row;
    row.version = W(r.version);
    row.settings = W(r.settings);
    row.commits = r.commits;
    row.changed = r.changed;
    row.first_ok = W(r.first_ok);
    row.deleted = W(r.deleted);
    row.recommended = W(r.recommended);
    row.llm = W(r.llm);
    row.detail = W(r.detail);
    rows.push_back(std::move(row));
  }
  return rows;
}

bool ResetChoiceStats() {
  core::ResetChoiceStats(Platform());
  return true;
}

ChoiceLogInfo ChoiceLogStatus() {
  const auto info = core::ChoiceLogStatus(Platform());
  return {info.records, info.bytes};
}

bool ClearChoiceLog() {
  return core::ClearChoiceLog(Platform());
}

// ---------------------------------------------------------------------------
// 個人詞庫

fs::path PersonalDir() {
  return core::PersonalDir(Platform());
}

bool SendPersonalCommand(unsigned long command) {
  return Platform().SendPersonalCommand(command);
}

std::wstring PersonalStatusText(bool* running) {
  return W(core::PersonalStatusText(Platform(), running));
}

PersonalWords LoadPersonalWords() {
  const auto data = core::LoadPersonalWords(Platform());
  PersonalWords result;
  result.available = data.available;
  result.error = W(data.error);
  for (const auto& [word, score] : data.words)
    result.words.emplace_back(W(word), score);
  for (const auto& r : data.rules)
    result.rules.push_back({r.kind == core::WordRule::kAdd     ? WordRule::kAdd
                            : r.kind == core::WordRule::kBlock ? WordRule::kBlock
                                                               : WordRule::kMerge,
                            W(r.from), W(r.to)});
  return result;
}

bool SendWordEdits(const std::vector<std::wstring>& lines, std::wstring* error) {
  std::vector<std::string> utf8;
  for (const auto& line : lines)
    utf8.push_back(U8(line));
  ErrorOut out(error);
  return core::SendWordEdits(Platform(), utf8, &out.value);
}

// ---------------------------------------------------------------------------
// 預測測試

bool SendLLMTestRequest(unsigned id, const std::wstring& context) {
  return core::SendLLMTestRequest(Platform(), id, U8(context));
}

std::optional<LLMTestResponse> PollLLMTestResponse(unsigned id) {
  const auto r = core::PollLLMTestResponse(Platform(), id);
  if (!r)
    return std::nullopt;
  LLMTestResponse result;
  result.status = r->status;
  result.model = W(r->model);
  result.ms = r->ms;
  result.candidates = W(r->candidates);
  return result;
}

// ---------------------------------------------------------------------------
// 使用者詞典

std::vector<std::wstring> ListUserDicts(RimeLeversApi* api) {
  return W(core::ListUserDicts(api));
}

ServiceMaintenance::ServiceMaintenance() {
  Platform().StartMaintenance();
}

ServiceMaintenance::~ServiceMaintenance() {
  Platform().EndMaintenance();
}

void PrepareUserDictTask() {
  core::PrepareUserDictTask();
}

}  // namespace settings_ops
