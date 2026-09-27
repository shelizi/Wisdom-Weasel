#include "stdafx.h"
#include "SettingsBackend.h"
#include "Configurator.h"
#include "FontSettingDialog.h"
#include "SettingsOps.h"
#include "UIStyleSettings.h"
#include "resource.h"
#include "../WeaselServer/LLMProvider.h"
#include <WeaselUtility.h>
#include <rime_levers_api.h>
#include <fstream>
#include <iterator>
#include <map>
#include <set>

namespace ops = settings_ops;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

const wchar_t kThemeRegKey[] = L"Software\\Rime\\Weasel";
const wchar_t kThemeRegValue[] = L"SettingsTheme";

std::string U8(const std::wstring& s) {
  return wtou8(s);
}

std::wstring W(const json& j) {
  return j.is_string() ? u8tow(j.get<std::string>()) : std::wstring();
}

std::wstring LoadStr(UINT id) {
  wchar_t buf[512] = {0};
  LoadStringW(GetModuleHandleW(nullptr), id, buf, _countof(buf));
  return buf;
}

std::string Base64(const std::string& data) {
  static const char* kChars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((data.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < data.size(); i += 3) {
    const unsigned v = (unsigned char)data[i] << 16 | (unsigned char)data[i + 1] << 8 |
                       (unsigned char)data[i + 2];
    out += kChars[v >> 18];
    out += kChars[(v >> 12) & 63];
    out += kChars[(v >> 6) & 63];
    out += kChars[v & 63];
  }
  if (i < data.size()) {
    unsigned v = (unsigned char)data[i] << 16;
    if (i + 1 < data.size())
      v |= (unsigned char)data[i + 1] << 8;
    out += kChars[v >> 18];
    out += kChars[(v >> 12) & 63];
    out += i + 1 < data.size() ? kChars[(v >> 6) & 63] : '=';
    out += '=';
  }
  return out;
}

// ---------------------------------------------------------------------------
// Rime 設定 ↔ JSON。Rime 的純量沒有型別，讀出來一律是字串（"true"、"3" 等），由網頁解讀；
// 寫回時字串照原樣、布林與數字交給 Rime 轉成文字。

json ConfigToJson(RimeApi* rime, RimeConfig* config, const std::string& path) {
  RimeConfigIterator it = {0};
  if (rime->config_begin_map(&it, config, path.c_str())) {
    json obj = json::object();
    std::vector<std::pair<std::string, std::string>> children;
    while (rime->config_next(&it))
      children.emplace_back(it.key, it.path);
    rime->config_end(&it);
    for (const auto& [key, child] : children)
      obj[key] = ConfigToJson(rime, config, child);
    return obj;
  }
  if (rime->config_begin_list(&it, config, path.c_str())) {
    json arr = json::array();
    std::vector<std::string> children;
    while (rime->config_next(&it))
      children.emplace_back(it.path);
    rime->config_end(&it);
    for (const auto& child : children)
      arr.push_back(ConfigToJson(rime, config, child));
    return arr;
  }
  if (const char* value = rime->config_get_cstring(config, path.c_str()))
    return std::string(value);
  return nullptr;
}

void JsonToConfig(RimeApi* rime, RimeConfig* config, const std::string& path, const json& value) {
  const std::string prefix = path.empty() ? std::string() : path + "/";
  if (value.is_object()) {
    for (auto it = value.begin(); it != value.end(); ++it)
      JsonToConfig(rime, config, prefix + it.key(), it.value());
  } else if (value.is_array()) {
    for (const auto& item : value)
      JsonToConfig(rime, config, prefix + "@next", item);
  } else if (value.is_boolean()) {
    rime->config_set_bool(config, path.c_str(), value.get<bool>());
  } else if (value.is_number_integer()) {
    rime->config_set_int(config, path.c_str(), value.get<int>());
  } else if (value.is_number()) {
    rime->config_set_double(config, path.c_str(), value.get<double>());
  } else if (value.is_string()) {
    rime->config_set_string(config, path.c_str(), value.get<std::string>().c_str());
  }
}

json FontsJson(const UIStyleSettings& ui) {
  return {{"font_face", U8(ui.font_face)},
          {"font_point", ui.font_point},
          {"label_font_face", U8(ui.label_font_face)},
          {"label_font_point", ui.label_font_point},
          {"comment_font_face", U8(ui.comment_font_face)},
          {"comment_font_point", ui.comment_font_point}};
}

}  // namespace

// ---------------------------------------------------------------------------

struct SettingsBackend::State {
  Configurator* configurator;
  Emit emit;
  RimeLeversApi* api = nullptr;
  RimeSwitcherSettings* switcher = nullptr;
  std::unique_ptr<UIStyleSettings> ui;
  RimeSchemaList available = {0};
  std::map<std::string, RimeSchemaInfo*> schema_info;

  // 背景工作（模型檔、語言模型下載）：一次各一個，關閉時取消並等候
  std::mutex job_mutex;
  std::thread model_job;
  std::shared_ptr<std::atomic<bool>> model_cancel;
  std::atomic<bool> model_running{false};
  std::thread grammar_job;
  std::shared_ptr<std::atomic<bool>> grammar_cancel;
  std::atomic<unsigned> probe_id{0};

  ~State() {
    for (auto* cancel : {&model_cancel, &grammar_cancel}) {
      if (*cancel)
        **cancel = true;
    }
    if (model_job.joinable())
      model_job.join();
    if (grammar_job.joinable())
      grammar_job.join();
    if (available.size)
      api->schema_list_destroy(&available);
    if (switcher)
      api->custom_settings_destroy((RimeCustomSettings*)switcher);
  }

  void LoadSchemas() {
    if (available.size)
      api->schema_list_destroy(&available);
    available = {0};
    schema_info.clear();
    api->get_available_schema_list(switcher, &available);
    for (size_t i = 0; i < available.size; ++i)
      schema_info.emplace(available.list[i].schema_id, (RimeSchemaInfo*)available.list[i].reserved);
  }

  // 已選的方案在前（依選用的順序），其餘在後
  json SchemasJson() {
    RimeSchemaList selected = {0};
    api->get_selected_schema_list(switcher, &selected);
    json list = json::array();
    std::set<std::string> added;
    for (size_t i = 0; i < selected.size; ++i) {
      const std::string id = selected.list[i].schema_id;
      for (size_t j = 0; j < available.size; ++j) {
        if (id == available.list[j].schema_id && added.insert(id).second)
          list.push_back({{"id", id}, {"name", available.list[j].name}, {"selected", true}});
      }
    }
    for (size_t j = 0; j < available.size; ++j) {
      const std::string id = available.list[j].schema_id;
      if (added.insert(id).second)
        list.push_back({{"id", id}, {"name", available.list[j].name}, {"selected", false}});
    }
    if (selected.size)
      api->schema_list_destroy(&selected);
    const char* hotkeys = api->get_hotkeys(switcher);
    return {{"list", list}, {"hotkeys", hotkeys ? hotkeys : ""}};
  }

  json StyleJson() {
    std::vector<ColorSchemeInfo> presets;
    ui->GetPresetColorSchemes(&presets);
    json schemes = json::array();
    for (const auto& p : presets)
      schemes.push_back({{"id", p.color_scheme_id}, {"name", p.name}, {"author", p.author}});
    return {{"schemes", schemes}, {"active", ui->GetActiveColorScheme()}, {"fonts", FontsJson(*ui)}};
  }

  json LLMJson() {
    RimeApi* rime = rime_get_api();
    RimeConfig config = {0};
    api->settings_get_config(ui->settings(), &config);
    json llm = ConfigToJson(rime, &config, "llm");
    return llm.is_object() ? llm : json::object();
  }

  json GrammarJson() {
    std::error_code ec;
    const auto size = fs::file_size(ops::GrammarPath(), ec);
    return {{"enabled", ops::GrammarEnabled()},
            {"ready", ops::GrammarReady()},
            {"size_mb", ec ? 0 : (int)(size >> 20)},
            {"downloading", grammar_job.joinable() && grammar_cancel && !*grammar_cancel}};
  }

  // 與舊設定視窗的「套用」相同的順序：方案 → llm → 注音排序 → Rime 容錯 → 外觀 → 語言模型 → 部署
  json Apply(const json& c) {
    RimeApi* rime = rime_get_api();
    bool saved = false;
    std::wstring errors;
    auto add_error = [&](const std::wstring& label, const std::wstring& error) {
      errors += (errors.empty() ? L"" : L"；") + label + error;
    };
    if (c.contains("schemas")) {
      std::vector<std::string> ids = c["schemas"].get<std::vector<std::string>>();
      if (ids.empty())
        throw Error(U8(LoadStr(IDS_STR_ERR_AT_LEAST_ONE_SEL)));
      std::vector<const char*> selection;
      for (const auto& id : ids)
        selection.push_back(id.c_str());
      api->select_schemas(switcher, selection.data(), (int)selection.size());
      api->save_settings((RimeCustomSettings*)switcher);
      saved = true;
    }
    const bool llm_changed = c.contains("llm");
    if (llm_changed) {
      RimeConfig llm = {0};
      rime->config_init(&llm);
      JsonToConfig(rime, &llm, "", c["llm"]);
      const bool ok = !!api->customize_item(ui->settings(), "llm", &llm);
      rime->config_close(&llm);
      if (!ok)
        throw Error(u8"LLM 設定儲存失敗。");
    }
    if (c.contains("rime_boost")) {
      std::wstring error;
      ops::ApplyRimeBoost(api, switcher, c["rime_boost"].get<bool>(), &error);
      if (!error.empty())
        add_error(L"注音排序：", error);
    }
    if (c.contains("typo_rime")) {
      std::wstring error;
      if (!ops::ApplyTypoCorrection(c["typo_rime"].get<bool>(), &error))
        add_error(L"Rime 容錯：", error);
    }
    bool style_changed = false;
    if (c.contains("style")) {
      const json& style = c["style"];
      if (style.contains("color_scheme"))
        ui->SelectColorScheme(style["color_scheme"].get<std::string>());
      if (style.contains("fonts")) {
        const json& f = style["fonts"];
        ui->font_face = W(f.value("font_face", json()));
        ui->label_font_face = W(f.value("label_font_face", json()));
        ui->comment_font_face = W(f.value("comment_font_face", json()));
        ui->font_point = f.value("font_point", ui->font_point);
        ui->label_font_point = f.value("label_font_point", ui->label_font_point);
        ui->comment_font_point = f.value("comment_font_point", ui->comment_font_point);
        ui->SetFontFace("style/font_face", U8(ui->font_face));
        ui->SetFontFace("style/label_font_face", U8(ui->label_font_face));
        ui->SetFontFace("style/comment_font_face", U8(ui->comment_font_face));
        ui->SetFontPoint("style/font_point", ui->font_point);
        ui->SetFontPoint("style/label_font_point", ui->label_font_point);
        ui->SetFontPoint("style/comment_font_point", ui->comment_font_point);
      }
      style_changed = true;
    }
    if (style_changed || llm_changed) {
      api->save_settings(ui->settings());
      saved = true;
    }
    if (c.contains("grammar")) {
      std::wstring error;
      if (!ops::ApplyGrammar(c["grammar"].get<bool>(), &error))
        add_error(L"語言模型：", error);
      saved = true;
    }
    if (!saved)
      return {{"deployed", false}, {"message", u8"沒有需要套用的變更。"}};
    configurator->UpdateWorkspace(true);
    return {{"deployed", true},
            {"message", U8(errors.empty() ? L"已套用。" : L"已套用；" + errors)},
            {"llm", LLMJson()},
            {"grammar", GrammarJson()}};
  }

  void StartModelJob(const std::wstring& src, const fs::path& dest, bool download) {
    std::lock_guard<std::mutex> lock(job_mutex);
    if (model_running)
      throw Error(u8"已有檔案正在處理中。");
    if (model_job.joinable())
      model_job.join();
    std::error_code ec;
    fs::create_directories(ops::ModelsDir(), ec);
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    model_cancel = cancel;
    model_running = true;
    Emit emit_copy = emit;
    model_job = std::thread([this, emit_copy, cancel, src, dest, download]() {
      // 先寫到 .part，完成並確認是 GGUF 後才改名，避免留下半個模型檔
      fs::path part = dest;
      part += L".part";
      ULONGLONG last_tick = 0;
      auto progress = [&](unsigned long long done, unsigned long long total) {
        const ULONGLONG now = GetTickCount64();
        if (now - last_tick >= 300) {
          last_tick = now;
          std::wstring text = (download ? L"下載中 " : L"複製中 ") + ops::FormatSize(done);
          if (total) {
            wchar_t pct[16];
            swprintf_s(pct, L"%.0f%%", done * 100.0 / total);
            text += L" / " + ops::FormatSize(total) + L"（" + pct + L"）";
          }
          emit_copy("modelJob", {{"state", "progress"}, {"message", U8(text)}});
        }
        return !cancel->load();
      };
      emit_copy("modelJob", {{"state", "progress"}, {"message", download ? u8"連線中…" : u8"複製中…"}});
      std::wstring error;
      bool ok = download ? ops::HttpDownload(src, part, progress, &error)
                         : ops::CopyFileWithProgress(src, part, progress, &error);
      std::error_code ec2;
      if (ok && !ops::IsGguf(part)) {
        ok = false;
        error = L"下載的內容不是 GGUF 模型檔（網址可能指向網頁）";
      }
      if (ok && !MoveFileExW(part.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        ok = false;
        error = L"無法取代原本的檔案（可能正在使用中）";
      }
      if (!ok)
        fs::remove(part, ec2);
      const std::wstring name = dest.filename().wstring();
      emit_copy("modelJob",
                {{"state", ok ? "done" : "error"},
                 {"path", ok ? U8(dest.wstring()) : ""},
                 {"message", U8(ok ? (download ? L"下載完成：" : L"已加入：") + name
                                   : (download ? L"下載失敗：" : L"加入失敗：") + error)}});
      model_running = false;
    });
  }
};

SettingsBackend::SettingsBackend(Configurator* configurator, Emit emit) : s_(new State) {
  s_->configurator = configurator;
  s_->emit = std::move(emit);
  RimeModule* levers = rime_get_api()->find_module("levers");
  if (!levers || !(s_->api = (RimeLeversApi*)levers->get_api()))
    throw Error(u8"無法載入 Rime 設定模組（levers）。");
  s_->switcher = s_->api->switcher_settings_init();
  s_->ui = std::make_unique<UIStyleSettings>();
  if (!s_->api->load_settings((RimeCustomSettings*)s_->switcher) ||
      !s_->api->load_settings(s_->ui->settings()))
    throw Error(u8"無法讀取 Rime 設定。");
  s_->LoadSchemas();
}

SettingsBackend::~SettingsBackend() = default;

SettingsBackend::Thread SettingsBackend::ThreadOf(const std::string& method) {
  static const std::set<std::string> kUi = {"fonts.pick", "models.pickFile", "dicts.pickFile",
                                            "app.setTheme"};
  static const std::set<std::string> kRime = {"init", "schemas.details", "schemas.install",
                                              "style.preview", "settings.apply", "dicts.list",
                                              "dicts.run", "llm.reload"};
  if (kUi.count(method))
    return Thread::kUi;
  if (kRime.count(method))
    return Thread::kRime;
  return Thread::kTask;
}

json SettingsBackend::Call(const std::string& method, const json& p, void* owner) {
  State& s = *s_;

  // --- 整體 -----------------------------------------------------------------
  if (method == "init") {
    DWORD theme = 0, size = sizeof(theme);
    if (RegGetValueW(HKEY_CURRENT_USER, kThemeRegKey, kThemeRegValue, RRF_RT_REG_DWORD, NULL,
                     &theme, &size) != ERROR_SUCCESS ||
        theme > 2)
      theme = 0;
    return {{"theme", theme},
            {"schemas", s.SchemasJson()},
            {"style", s.StyleJson()},
            {"llm", s.LLMJson()},
            {"grammar", s.GrammarJson()},
            {"defaults",
             {{"correct_instruction", U8(kLLMCorrectInstruction)},
              {"api_url", U8(ops::kDefaultApiUrl)},
              {"models_dir", U8(ops::ModelsDir().wstring())}}}};
  }
  if (method == "llm.reload")
    return s.LLMJson();
  if (method == "settings.apply")
    return s.Apply(p);
  if (method == "app.setTheme") {
    DWORD theme = p.value("theme", 0);
    RegSetKeyValueW(HKEY_CURRENT_USER, kThemeRegKey, kThemeRegValue, REG_DWORD, &theme,
                    sizeof(theme));
    return nullptr;
  }

  // --- 輸入方案 ---------------------------------------------------------------
  if (method == "schemas.details") {
    auto it = s.schema_info.find(p.value("id", std::string()));
    if (it == s.schema_info.end())
      return nullptr;
    const char* name = s.api->get_schema_name(it->second);
    const char* author = s.api->get_schema_author(it->second);
    const char* description = s.api->get_schema_description(it->second);
    return {{"name", name ? name : ""},
            {"author", author ? author : ""},
            {"description", description ? description : ""}};
  }
  if (method == "schemas.install") {
    // 與舊設定視窗的「取得更多輸入方案」相同：執行 rime-install.bat，結束後重新讀取方案清單
    HKEY key;
    const std::wstring reg =
        is_wow64() ? L"Software\\WOW6432Node\\Rime\\Weasel" : L"Software\\Rime\\Weasel";
    if (RegOpenKeyW(HKEY_LOCAL_MACHINE, reg.c_str(), &key) != ERROR_SUCCESS)
      throw Error(u8"找不到小狼毫的安裝資料夾。");
    wchar_t root[MAX_PATH] = {0};
    DWORD len = sizeof(root), type = 0;
    const bool found = RegQueryValueExW(key, L"WeaselRoot", NULL, &type, (LPBYTE)root, &len) ==
                           ERROR_SUCCESS &&
                       type == REG_SZ;
    RegCloseKey(key);
    if (!found)
      throw Error(u8"找不到小狼毫的安裝資料夾。");
    const std::wstring parameters = std::wstring(L"/k \"") + root + L"\\rime-install.bat\"";
    SHELLEXECUTEINFOW cmd = {sizeof(cmd)};
    cmd.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    cmd.lpVerb = L"open";
    cmd.lpFile = L"cmd";
    cmd.lpParameters = parameters.c_str();
    cmd.nShow = SW_SHOW;
    if (ShellExecuteExW(&cmd) && cmd.hProcess) {
      WaitForSingleObject(cmd.hProcess, INFINITE);
      CloseHandle(cmd.hProcess);
    }
    s.api->load_settings((RimeCustomSettings*)s.switcher);
    s.LoadSchemas();
    return s.SchemasJson();
  }

  // --- 外觀 -----------------------------------------------------------------
  if (method == "style.preview") {
    // 路徑是 ANSI 編碼，不是 UTF-8
    const std::string file = s.ui->GetColorSchemePreview(p.value("id", std::string()));
    if (file.empty())
      return nullptr;
    std::ifstream in(acptow(file), std::ios::binary);
    if (!in)
      return nullptr;
    const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return "data:image/png;base64," + Base64(data);
  }
  if (method == "fonts.pick") {
    // 沿用原本的字型設定視窗；網頁上尚未套用的值先帶進去
    const json& f = p;
    UIStyleSettings& ui = *s.ui;
    const auto saved = FontsJson(ui);
    ui.font_face = W(f.value("font_face", json()));
    ui.label_font_face = W(f.value("label_font_face", json()));
    ui.comment_font_face = W(f.value("comment_font_face", json()));
    ui.font_point = f.value("font_point", ui.font_point);
    ui.label_font_point = f.value("label_font_point", ui.label_font_point);
    ui.comment_font_point = f.value("comment_font_point", ui.comment_font_point);
    FontSettingDialog dialog(&ui, (HWND)owner);
    json result = nullptr;
    if (dialog.ShowDialog() == IDOK)
      result = {{"font_face", U8(dialog.m_font_face)},
                {"font_point", dialog.m_font_point},
                {"label_font_face", U8(dialog.m_label_font_face)},
                {"label_font_point", dialog.m_label_font_point},
                {"comment_font_face", U8(dialog.m_comment_font_face)},
                {"comment_font_point", dialog.m_comment_font_point}};
    // 真正寫入等「套用」
    ui.font_face = W(saved["font_face"]);
    ui.label_font_face = W(saved["label_font_face"]);
    ui.comment_font_face = W(saved["comment_font_face"]);
    ui.font_point = saved["font_point"];
    ui.label_font_point = saved["label_font_point"];
    ui.comment_font_point = saved["comment_font_point"];
    return result;
  }

  // --- 智慧預測：透過正在執行的輸入法 ------------------------------------------
  if (method == "llm.probe") {
    // 詢問輸入法目前載入的模型；context 非空時順便預測（測試）
    const unsigned id = (unsigned)GetTickCount() + ++s.probe_id;
    const std::wstring context = W(p.value("context", json()));
    if (!ops::SendLLMTestRequest(id, context))
      return {{"connected", false}};
    const ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < 30000) {
      if (auto r = ops::PollLLMTestResponse(id)) {
        json candidates = json::array();
        for (const auto& c : r->candidates)
          candidates.push_back(U8(c));
        return {{"connected", true},
                {"status", r->status},
                {"model", U8(ops::LoadedModelDisplay(r->model))},
                {"ms", r->ms},
                {"candidates", candidates}};
      }
      Sleep(150);
    }
    return {{"connected", true}, {"timeout", true}};
  }

  // --- 語言模型 -------------------------------------------------------------
  if (method == "models.scan") {
    json list = json::array();
    for (const auto& path : ops::ScanModels(W(p.value("current", json()))))
      list.push_back(U8(path));
    return list;
  }
  if (method == "models.files") {
    json list = json::array();
    for (const auto& f : ops::ListModelFiles())
      list.push_back({{"path", U8(f.path)},
                      {"name", U8(ops::FileNameOf(f.path))},
                      {"size", U8(ops::FormatSize(f.size))}});
    return list;
  }
  if (method == "models.guessType") {
    const wchar_t* type = ops::GuessModelType(W(p.value("path", json())));
    return type ? json(U8(type)) : json(nullptr);
  }
  if (method == "models.pickFile") {
    const std::wstring path =
        ops::OpenFileDialog(owner, L"選擇模型檔", {{L"GGUF 模型 (*.gguf)", L"*.gguf"}, {L"所有檔案 (*.*)", L"*.*"}},
                            nullptr, L"gguf");
    return path.empty() ? json(nullptr) : json(U8(path));
  }
  if (method == "models.add") {
    const fs::path src = W(p.value("path", json()));
    if (!ops::IsGguf(src))
      throw Error(u8"這不是 GGUF 模型檔。");
    const fs::path dest = ops::ModelsDir() / src.filename();
    std::error_code ec;
    if (fs::equivalent(src, dest, ec))
      throw Error(u8"這個檔案已經在模型資料夾裡。");
    if (fs::exists(dest, ec) && !p.value("replace", false))
      return {{"exists", U8(src.filename().wstring())}};
    s.StartModelJob(src.wstring(), dest, false);
    return {{"started", true}};
  }
  if (method == "models.download") {
    std::wstring name;
    const std::wstring url = ops::NormalizeModelUrl(W(p.value("url", json())), &name);
    if (url.rfind(L"http://", 0) != 0 && url.rfind(L"https://", 0) != 0)
      throw Error(u8"請貼上以 https:// 開頭的下載網址。");
    if (ops::ToLower(fs::path(name).extension().wstring()) != L".gguf" ||
        name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos)
      throw Error(u8"網址要指向 .gguf 檔（例如 …/resolve/main/model.gguf）。");
    const fs::path dest = ops::ModelsDir() / name;
    std::error_code ec;
    if (fs::exists(dest, ec) && !p.value("replace", false))
      return {{"exists", U8(name)}};
    s.StartModelJob(url, dest, true);
    return {{"started", true}};
  }
  if (method == "models.cancel") {
    std::lock_guard<std::mutex> lock(s.job_mutex);
    if (s.model_cancel)
      *s.model_cancel = true;
    return nullptr;
  }
  if (method == "models.delete") {
    const std::wstring path = W(p.value("path", json()));
    if (ops::FileInUse(path))
      throw Error(u8"檔案正在使用中（輸入法已載入這個模型）。請先改用其他模型並套用。");
    if (!ops::MoveToRecycleBin(path))
      throw Error(u8"移除失敗。");
    return U8(L"已移到資源回收筒：" + ops::FileNameOf(path));
  }
  if (method == "shell.reveal") {
    const std::wstring path = W(p.value("path", json()));
    std::error_code ec;
    if (!path.empty() && fs::exists(path, ec)) {
      ops::OpenFolderAndSelectItem(path);
    } else {
      fs::create_directories(ops::ModelsDir(), ec);
      ShellExecuteW(NULL, L"open", ops::ModelsDir().c_str(), NULL, NULL, SW_SHOWNORMAL);
    }
    return nullptr;
  }
  if (method == "api.test")
    return U8(ops::TestApi(W(p.value("api_url", json())), W(p.value("api_key", json())),
                           W(p.value("model", json()))));

  // --- 選字策略 -------------------------------------------------------------
  if (method == "grammar.status")
    return s.GrammarJson();
  if (method == "grammar.download") {
    std::lock_guard<std::mutex> lock(s.job_mutex);
    if (s.grammar_cancel && !*s.grammar_cancel) {
      *s.grammar_cancel = true;  // 下載中：這個按鈕是「取消下載」
      return {{"cancelled", true}};
    }
    if (s.grammar_job.joinable())
      s.grammar_job.join();
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    s.grammar_cancel = cancel;
    Emit emit = s.emit;
    s.grammar_job = std::thread([emit, cancel]() {
      const fs::path dest = ops::GrammarPath();
      fs::path part = dest;
      part += L".part";
      int last_percent = -1;
      std::wstring error;
      const bool ok = ops::HttpDownload(
          ops::kGrammarUrl, part,
          [&](unsigned long long done, unsigned long long total) {
            const int percent = total ? (int)(done * 100 / total) : 0;
            if (percent != last_percent) {
              last_percent = percent;
              emit("grammar", {{"state", "progress"}, {"percent", percent}});
            }
            return !cancel->load();
          },
          &error);
      std::error_code ec;
      if (ok) {
        fs::rename(part, dest, ec);
        if (ec)
          error = L"無法儲存模型檔";
      } else {
        fs::remove(part, ec);
      }
      const bool success = ok && error.empty();
      *cancel = true;
      emit("grammar", {{"state", success ? "done" : "error"}, {"message", U8(error)}});
    });
    return {{"started", true}};
  }
  if (method == "stats.get") {
    json rows = json::array();
    for (const auto& r : ops::ChoiceStats(p.value("span", 7)))
      rows.push_back({{"version", U8(r.version)},
                      {"settings", U8(r.settings)},
                      {"commits", r.commits},
                      {"first_ok", U8(r.first_ok)},
                      {"deleted", U8(r.deleted)},
                      {"changed", r.changed},
                      {"recommended", U8(r.recommended)},
                      {"llm", U8(r.llm)},
                      {"detail", U8(r.detail)}});
    return rows;
  }
  if (method == "stats.reset") {
    ops::ResetChoiceStats();
    return nullptr;
  }
  if (method == "choicelog.status") {
    const auto info = ops::ChoiceLogStatus();
    return {{"records", info.records}, {"kb", (info.bytes + 1023) / 1024}};
  }
  if (method == "choicelog.clear") {
    if (!ops::ClearChoiceLog())
      throw Error(u8"無法刪除選字紀錄。");
    return nullptr;
  }

  // --- 個人詞庫 -------------------------------------------------------------
  if (method == "personal.status") {
    // refresh：先請輸入法重寫狀態檔（詞數會隨打字變動）
    if (p.value("refresh", false)) {
      ops::SendPersonalCommand(1);
      Sleep(100);
    }
    bool running = false;
    const std::wstring text = ops::PersonalStatusText(&running);
    return {{"text", U8(text)}, {"running", running}};
  }
  if (method == "personal.command") {
    const unsigned command = p.value("command", 0u);
    if (command < 2 || command > 4)
      throw Error("bad command");
    if (!ops::SendPersonalCommand(command))
      throw Error(u8"無法連線到輸入法服務。");
    Sleep(100);
    return nullptr;
  }
  if (method == "words.load") {
    const auto data = ops::LoadPersonalWords();
    json words = json::array();
    for (const auto& [word, score] : data.words)
      words.push_back({U8(word), score});
    json rules = json::array();
    for (const auto& r : data.rules)
      rules.push_back({{"kind", r.kind == ops::WordRule::kAdd     ? "add"
                                : r.kind == ops::WordRule::kBlock ? "block"
                                                                  : "merge"},
                       {"from", U8(r.from)},
                       {"to", U8(r.to)}});
    return {{"available", data.available},
            {"error", U8(data.error)},
            {"words", words},
            {"rules", rules}};
  }
  if (method == "words.edit") {
    std::vector<std::wstring> lines;
    for (const auto& line : p.value("lines", json::array()))
      lines.push_back(W(line));
    std::wstring error;
    if (!ops::SendWordEdits(lines, &error))
      throw Error(U8(error));
    return nullptr;
  }

  // --- 使用者詞典 -----------------------------------------------------------
  if (method == "dicts.list") {
    ops::PrepareUserDictTask();
    json list = json::array();
    for (const auto& d : ops::ListUserDicts(s.api))
      list.push_back(U8(d));
    return list;
  }
  if (method == "dicts.pickFile") {
    const std::string op = p.value("op", std::string());
    const std::wstring name = W(p.value("name", json()));
    const std::wstring all = LoadStr(IDS_STR_ALL_FILES);
    std::wstring path;
    if (op == "restore") {
      path = ops::OpenFileDialog(owner, LoadStr(IDS_STR_OPEN),
                                 {{LoadStr(IDS_STR_DICT_SNAPSHOT) + L" (*.userdb.txt)", L"*.userdb.txt"},
                                  {LoadStr(IDS_STR_KCSS_DICT_SNAPSHOT) + L" (*.userdb.kct.snapshot)",
                                   L"*.userdb.kct.snapshot"},
                                  {all, L"*.*"}},
                                 nullptr, L"snapshot");
    } else {
      const std::vector<ops::FileFilter> filter = {
          {LoadStr(IDS_STR_TXT_FILES) + L" (*.txt)", L"*.txt"}, {all, L"*.*"}};
      const std::wstring file_name = name + L"_export.txt";
      path = op == "export" ? ops::SaveFileDialog(owner, LoadStr(IDS_STR_SAVE_AS), filter,
                                                  file_name.c_str(), L"txt")
                            : ops::OpenFileDialog(owner, LoadStr(IDS_STR_OPEN), filter,
                                                  file_name.c_str(), L"txt");
    }
    return path.empty() ? json(nullptr) : json(U8(path));
  }
  if (method == "dicts.run") {
    const std::string op = p.value("op", std::string());
    const std::wstring name = W(p.value("name", json()));
    const std::wstring path = W(p.value("path", json()));
    const std::string name_u8 = U8(name), path_u8 = U8(path);
    RimeApi* rime = rime_get_api();
    std::wstring reveal, report;
    UINT error_ids = 0;
    {
      ops::ServiceMaintenance maintenance;
      ops::PrepareUserDictTask();  // 建立使用者資料同步資料夾
      if (op == "backup") {
        char dir[MAX_PATH] = {0};
        rime->get_user_data_sync_dir(dir, _countof(dir));
        WCHAR wdir[MAX_PATH] = {0};
        MultiByteToWideChar(CP_ACP, 0, dir, -1, wdir, _countof(wdir));
        std::wstring file = wdir;
        if (_waccess_s(file.c_str(), 0) != 0 && !CreateDirectoryW(file.c_str(), NULL) &&
            GetLastError() == ERROR_PATH_NOT_FOUND) {
          error_ids = IDS_STR_ERREXPORT_SYNC_UV;
        } else {
          file += L"\\" + name + L".userdb.txt";
          if (!s.api->backup_user_dict(name_u8.c_str()))
            error_ids = IDS_STR_ERR_EXPORT_UNKNOWN;
          else if (_waccess(file.c_str(), 0) != 0)
            error_ids = IDS_STR_ERR_EXPORT_SNAP_LOST;
          else
            reveal = file, report = L"已備份「" + name + L"」。";
        }
      } else if (op == "restore") {
        if (!s.api->restore_user_dict(path_u8.c_str()))
          error_ids = IDS_STR_ERR_UNKNOWN;
        else
          report = L"已還原詞典快照。";
      } else if (op == "export") {
        const int result = s.api->export_user_dict(name_u8.c_str(), path_u8.c_str());
        if (result < 0)
          error_ids = IDS_STR_ERR_UNKNOWN;
        else if (_waccess(path.c_str(), 0) != 0)
          error_ids = IDS_STR_ERR_EXPORT_FILE_LOST;
        else
          reveal = path, report = LoadStr(IDS_STR_EXPORTED) + L" " + std::to_wstring(result) +
                                  L" " + LoadStr(IDS_STR_RECORD_COUNT);
      } else if (op == "import") {
        const int result = s.api->import_user_dict(name_u8.c_str(), path_u8.c_str());
        if (result < 0)
          error_ids = IDS_STR_ERR_UNKNOWN;
        else
          report = LoadStr(IDS_STR_IMPORTED) + L" " + std::to_wstring(result) + L" " +
                   LoadStr(IDS_STR_RECORD_COUNT);
      } else {
        throw Error("bad op");
      }
    }
    if (error_ids)
      throw Error(U8(LoadStr(error_ids)));
    if (!reveal.empty())
      ops::OpenFolderAndSelectItem(reveal);
    return U8(report);
  }

  throw Error("unknown method: " + method);
}
