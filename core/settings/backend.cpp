#include "backend.h"

#include <rime_api.h>
#include <rime_levers_api.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "../net/http.h"
#include "ops.h"

namespace settings {

using json = nlohmann::json;
using Error = Backend::Error;

namespace {

std::string Str(const json& params, const char* key) {
  const auto it = params.find(key);
  return it != params.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::string Base64(const std::string& data) {
  static const char* kChars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((data.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < data.size(); i += 3) {
    const unsigned v =
        (unsigned char)data[i] << 16 | (unsigned char)data[i + 1] << 8 | (unsigned char)data[i + 2];
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

// 三種文字的字型設定鍵
const char* const kFontKeys[] = {"font_face", "label_font_face", "comment_font_face"};
const char* const kPointKeys[] = {"font_point", "label_font_point", "comment_font_point"};

}  // namespace

// ---------------------------------------------------------------------------

struct Backend::State {
  Platform& platform;
  Options options;
  Emit emit;
  RimeApi* rime = rime_get_api();
  RimeLeversApi* api = nullptr;
  RimeSwitcherSettings* switcher = nullptr;
  RimeCustomSettings* style = nullptr;  // 前端的設定（weasel / squirrel）
  RimeSchemaList available = {0};
  std::map<std::string, RimeSchemaInfo*> schema_info;

  // 背景工作（模型檔、語言模型下載）：一次各一個，結束時取消並等候
  std::mutex job_mutex;
  std::thread model_job;
  std::shared_ptr<std::atomic<bool>> model_cancel;
  std::atomic<bool> model_running{false};
  std::thread grammar_job;
  std::shared_ptr<std::atomic<bool>> grammar_cancel;
  std::atomic<unsigned> probe_id{0};

  State(Platform& p, Options o, Emit e) : platform(p), options(std::move(o)), emit(std::move(e)) {}

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
    if (style)
      api->custom_settings_destroy(style);
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

  // 字型：部署後的設定（標籤、註解沒有設定大小時跟候選字一樣）
  json FontsJson() {
    RimeConfig config = {0};
    json fonts = json::object();
    if (!rime->config_open(options.config_id.c_str(), &config))
      return fonts;
    for (const char* key : kFontKeys) {
      const char* value = rime->config_get_cstring(&config, ("style/" + std::string(key)).c_str());
      fonts[key] = value ? value : "";
    }
    int point = 0;
    rime->config_get_int(&config, "style/font_point", &point);
    fonts["font_point"] = point;
    for (const char* key : {"label_font_point", "comment_font_point"}) {
      int value = point;
      if (!rime->config_get_int(&config, ("style/" + std::string(key)).c_str(), &value))
        value = point;
      fonts[key] = value;
    }
    rime->config_close(&config);
    return fonts;
  }

  json StyleJson() {
    RimeConfig config = {0};
    api->settings_get_config(style, &config);
    json schemes = json::array();
    RimeConfigIterator preset = {0};
    if (rime->config_begin_map(&preset, &config, "preset_color_schemes")) {
      while (rime->config_next(&preset)) {
        const std::string base = preset.path;
        const char* name = rime->config_get_cstring(&config, (base + "/name").c_str());
        const char* author = rime->config_get_cstring(&config, (base + "/author").c_str());
        if (name)
          schemes.push_back({{"id", preset.key}, {"name", name}, {"author", author ? author : ""}});
      }
      rime->config_end(&preset);
    }
    const char* active = rime->config_get_cstring(&config, "style/color_scheme");
    return {{"schemes", schemes}, {"active", active ? active : ""}, {"fonts", FontsJson()}};
  }

  // 配色預覽圖：使用者資料夾優先，其次共用資料夾
  std::string PreviewDataUrl(const std::string& id) {
    const std::string name = "color_scheme_" + id + ".png";
    fs::path file = platform.FromRimePath(rime->get_user_data_dir()) / "preview" / name;
    std::error_code ec;
    if (!fs::exists(file, ec))
      file = platform.FromRimePath(rime->get_shared_data_dir()) / "preview" / name;
    std::ifstream in(file, std::ios::binary);
    if (!in)
      return std::string();
    const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return "data:image/png;base64," + Base64(data);
  }

  json LLMJson() {
    RimeConfig config = {0};
    api->settings_get_config(style, &config);
    json llm = ConfigToJson(rime, &config, "llm");
    return llm.is_object() ? llm : json::object();
  }

  json GrammarJson() {
    std::error_code ec;
    const auto size = fs::file_size(GrammarPath(platform), ec);
    return {{"enabled", GrammarEnabled(platform)},
            {"ready", GrammarReady(platform)},
            {"size_mb", ec ? 0 : (int)(size >> 20)},
            {"downloading", grammar_job.joinable() && grammar_cancel && !*grammar_cancel}};
  }

  // 與原本設定視窗的「套用」相同的順序：方案 → llm → 注音排序 → Rime 容錯 → 外觀 → 語言模型 → 部署
  json Apply(const json& c) {
    bool saved = false;
    std::string errors;
    auto add_error = [&](const std::string& label, const std::string& error) {
      errors += (errors.empty() ? "" : "；") + label + error;
    };
    if (c.contains("schemas")) {
      const std::vector<std::string> ids = c["schemas"].get<std::vector<std::string>>();
      if (ids.empty())
        throw Error("至少要選用一項吧。");
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
      const bool ok = !!api->customize_item(style, "llm", &llm);
      rime->config_close(&llm);
      if (!ok)
        throw Error("LLM 設定儲存失敗。");
    }
    if (c.contains("rime_boost")) {
      std::string error;
      ApplyRimeBoost(platform, api, switcher, c["rime_boost"].get<bool>(), &error);
      if (!error.empty())
        add_error("注音排序：", error);
    }
    if (c.contains("typo_rime")) {
      std::string error;
      if (!ApplyTypoCorrection(platform, c["typo_rime"].get<bool>(), &error))
        add_error("Rime 容錯：", error);
    }
    const bool style_changed = c.contains("style");
    if (style_changed) {
      const json& s = c["style"];
      if (s.contains("color_scheme"))
        api->customize_string(style, "style/color_scheme", Str(s, "color_scheme").c_str());
      if (s.contains("fonts")) {
        const json& f = s["fonts"];
        for (const char* key : kFontKeys)
          api->customize_string(style, ("style/" + std::string(key)).c_str(), Str(f, key).c_str());
        for (const char* key : kPointKeys) {
          if (f.contains(key) && f[key].is_number_integer())
            api->customize_int(style, ("style/" + std::string(key)).c_str(), f[key].get<int>());
        }
      }
    }
    if (style_changed || llm_changed) {
      api->save_settings(style);
      saved = true;
    }
    if (c.contains("grammar")) {
      std::string error;
      if (!ApplyGrammar(platform, c["grammar"].get<bool>(), &error))
        add_error("語言模型：", error);
      saved = true;
    }
    if (!saved)
      return {{"deployed", false}, {"message", "沒有需要套用的變更。"}};
    platform.Deploy();
    // settings_get_config 是開啟設定時載入的部署結果，不含剛存的修改：部署完重新載入，
    // 回傳給網頁的 llm 才是新的（否則表單會顯示舊值，例如關掉的整句校正又顯示開啟）
    api->load_settings(style);
    return {{"deployed", true},
            {"message", errors.empty() ? "已套用。" : "已套用；" + errors},
            {"llm", LLMJson()},
            {"grammar", GrammarJson()}};
  }

  void StartModelJob(const std::string& src, const fs::path& dest, bool download) {
    std::lock_guard<std::mutex> lock(job_mutex);
    if (model_running)
      throw Error("已有檔案正在處理中。");
    if (model_job.joinable())
      model_job.join();
    std::error_code ec;
    fs::create_directories(platform.ModelsDir(), ec);
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    model_cancel = cancel;
    model_running = true;
    Emit emit_copy = emit;
    model_job = std::thread([this, emit_copy, cancel, src, dest, download]() {
      // 先寫到 .part，完成並確認是 GGUF 後才改名，避免留下半個模型檔
      fs::path part = dest;
      part += ".part";
      auto last = std::chrono::steady_clock::now() - std::chrono::seconds(1);
      auto progress = [&](uint64_t done, uint64_t total) {
        const auto now = std::chrono::steady_clock::now();
        if (now - last >= std::chrono::milliseconds(300)) {
          last = now;
          std::string text = (download ? "下載中 " : "複製中 ") + FormatSize(done);
          if (total) {
            char pct[16];
            std::snprintf(pct, sizeof(pct), "%.0f%%", done * 100.0 / total);
            text += " / " + FormatSize(total) + "（" + pct + "）";
          }
          emit_copy("modelJob", {{"state", "progress"}, {"message", text}});
        }
        return !cancel->load();
      };
      emit_copy("modelJob", {{"state", "progress"}, {"message", download ? "連線中…" : "複製中…"}});
      std::string error;
      bool ok = download ? net::Download(src, part, progress, &error)
                         : CopyFileWithProgress(Path(src), part, progress, &error);
      std::error_code ec2;
      if (ok && !IsGguf(part)) {
        ok = false;
        error = "下載的內容不是 GGUF 模型檔（網址可能指向網頁）";
      }
      if (ok) {
        fs::rename(part, dest, ec2);  // 取代原本的檔案
        if (ec2) {
          ok = false;
          error = "無法取代原本的檔案（可能正在使用中）";
        }
      }
      if (!ok)
        fs::remove(part, ec2);
      const std::string name = U8(dest.filename());
      emit_copy("modelJob", {{"state", ok ? "done" : "error"},
                             {"path", ok ? U8(dest) : ""},
                             {"message", ok ? (download ? "下載完成：" : "已加入：") + name
                                            : (download ? "下載失敗：" : "加入失敗：") + error}});
      model_running = false;
    });
  }
};

Backend::Backend(Platform& platform, Options options, Emit emit)
    : s_(new State(platform, std::move(options), std::move(emit))) {
  RimeModule* levers = s_->rime->find_module("levers");
  if (!levers || !(s_->api = (RimeLeversApi*)levers->get_api()))
    throw Error("無法載入 Rime 設定模組（levers）。");
  s_->switcher = s_->api->switcher_settings_init();
  s_->style = s_->api->custom_settings_init(s_->options.config_id.c_str(), s_->options.generator_id.c_str());
  // load_settings 在 <設定>.custom.yaml 還不存在時回傳 false（基本設定照樣載入）：還沒自訂過是正常的，
  // 例如鼠鬚管沒有 default.custom.yaml。套用時 save_settings 會建立它
  s_->api->load_settings((RimeCustomSettings*)s_->switcher);
  s_->api->load_settings(s_->style);
  s_->LoadSchemas();
}

Backend::~Backend() = default;

Backend::Thread Backend::ThreadOf(const std::string& method) {
  static const std::set<std::string> kUi = {"models.pickFile", "dicts.pickFile", "app.setTheme"};
  static const std::set<std::string> kRime = {"init", "schemas.details", "schemas.install", "style.preview",
                                              "settings.apply", "dicts.list", "dicts.run", "llm.reload"};
  if (kUi.count(method))
    return Thread::kUi;
  if (kRime.count(method))
    return Thread::kRime;
  return Thread::kTask;
}

json Backend::Call(const std::string& method, const json& p, void* owner) {
  State& s = *s_;
  Platform& platform = s.platform;

  // --- 整體 -----------------------------------------------------------------
  if (method == "init") {
    int theme = platform.GetPreference("SettingsTheme", 0);
    if (theme < 0 || theme > 2)
      theme = 0;
    return {{"theme", theme},
            {"schemas", s.SchemasJson()},
            {"style", s.StyleJson()},
            {"llm", s.LLMJson()},
            {"grammar", s.GrammarJson()},
            {"defaults",
             {{"correct_instruction", s.options.correct_instruction},
              {"api_url", std::string(kDefaultApiUrl)},
              {"models_dir", U8(platform.ModelsDir())}}}};
  }
  if (method == "llm.reload")
    return s.LLMJson();
  if (method == "settings.apply")
    return s.Apply(p);
  if (method == "app.setTheme") {
    platform.SetPreference("SettingsTheme", p.value("theme", 0));
    return nullptr;
  }

  // --- 輸入方案 ---------------------------------------------------------------
  if (method == "schemas.details") {
    auto it = s.schema_info.find(Str(p, "id"));
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
    std::string error;
    if (!platform.InstallSchemas(&error))
      throw Error(error);
    s.api->load_settings((RimeCustomSettings*)s.switcher);
    s.LoadSchemas();
    return s.SchemasJson();
  }

  // --- 外觀 -----------------------------------------------------------------
  if (method == "style.preview") {
    const std::string url = s.PreviewDataUrl(Str(p, "id"));
    return url.empty() ? json(nullptr) : json(url);
  }
  if (method == "fonts.list")
    return platform.SystemFonts();

  // --- 智慧預測：透過正在執行的輸入法 ------------------------------------------
  if (method == "llm.probe") {
    // 詢問輸入法目前載入的模型；context 非空時順便預測（測試）
    const unsigned id =
        (unsigned)std::chrono::steady_clock::now().time_since_epoch().count() + ++s.probe_id;
    if (!SendLLMTestRequest(platform, id, Str(p, "context")))
      return {{"connected", false}};
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(30)) {
      if (auto r = PollLLMTestResponse(platform, id))
        return {{"connected", true},
                {"status", r->status},
                {"model", LoadedModelDisplay(r->model)},
                {"ms", r->ms},
                {"candidates", r->candidates}};
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    return {{"connected", true}, {"timeout", true}};
  }

  // --- 語言模型 -------------------------------------------------------------
  if (method == "models.scan")
    return ScanModels(platform, Str(p, "current"));
  if (method == "models.files") {
    json list = json::array();
    for (const auto& f : ListModelFiles(platform))
      list.push_back({{"path", f.path}, {"name", FileNameOf(f.path)}, {"size", FormatSize(f.size)}});
    return list;
  }
  if (method == "models.guessType") {
    const char* type = GuessModelType(Str(p, "path"));
    return type ? json(type) : json(nullptr);
  }
  if (method == "models.pickFile") {
    const fs::path path = platform.OpenFileDialog(
        owner, "選擇模型檔", {{"GGUF 模型 (*.gguf)", "*.gguf"}, {"所有檔案 (*.*)", "*.*"}}, "", "gguf");
    return path.empty() ? json(nullptr) : json(U8(path));
  }
  if (method == "models.add") {
    const fs::path src = Path(Str(p, "path"));
    if (!IsGguf(src))
      throw Error("這不是 GGUF 模型檔。");
    const fs::path dest = platform.ModelsDir() / src.filename();
    std::error_code ec;
    if (fs::equivalent(src, dest, ec))
      throw Error("這個檔案已經在模型資料夾裡。");
    if (fs::exists(dest, ec) && !p.value("replace", false))
      return {{"exists", U8(src.filename())}};
    s.StartModelJob(U8(src), dest, false);
    return {{"started", true}};
  }
  if (method == "models.download") {
    std::string name;
    const std::string url = NormalizeModelUrl(Str(p, "url"), &name);
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)
      throw Error("請貼上以 https:// 開頭的下載網址。");
    if (ToLower(U8(Path(name).extension())) != ".gguf" || name.find_first_of("\\/:*?\"<>|") != std::string::npos)
      throw Error("網址要指向 .gguf 檔（例如 …/resolve/main/model.gguf）。");
    const fs::path dest = platform.ModelsDir() / Path(name);
    std::error_code ec;
    if (fs::exists(dest, ec) && !p.value("replace", false))
      return {{"exists", name}};
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
    const fs::path path = Path(Str(p, "path"));
    if (platform.FileInUse(path))
      throw Error("檔案正在使用中（輸入法已載入這個模型）。請先改用其他模型並套用。");
    if (!platform.MoveToTrash(path))
      throw Error("移除失敗。");
    return "已移到資源回收筒：" + U8(path.filename());
  }
  if (method == "shell.reveal") {
    const fs::path path = Path(Str(p, "path"));
    std::error_code ec;
    if (!path.empty() && fs::exists(path, ec)) {
      platform.Reveal(path);
    } else {
      fs::create_directories(platform.ModelsDir(), ec);
      platform.OpenFolder(platform.ModelsDir());
    }
    return nullptr;
  }
  if (method == "api.test")
    return TestApi(Str(p, "api_url"), Str(p, "api_key"), Str(p, "model"));

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
    const fs::path dest = GrammarPath(platform);
    s.grammar_job = std::thread([emit, cancel, dest]() {
      fs::path part = dest;
      part += ".part";
      int last_percent = -1;
      std::string error;
      const bool ok = net::Download(
          kGrammarUrl, part,
          [&](uint64_t done, uint64_t total) {
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
          error = "無法儲存模型檔";
      } else {
        fs::remove(part, ec);
      }
      const bool success = ok && error.empty();
      *cancel = true;
      emit("grammar", {{"state", success ? "done" : "error"}, {"message", error}});
    });
    return {{"started", true}};
  }
  if (method == "stats.get") {
    json rows = json::array();
    for (const auto& r : ChoiceStats(platform, p.value("span", 7)))
      rows.push_back({{"version", r.version},
                      {"settings", r.settings},
                      {"commits", r.commits},
                      {"first_ok", r.first_ok},
                      {"deleted", r.deleted},
                      {"changed", r.changed},
                      {"recommended", r.recommended},
                      {"llm", r.llm},
                      {"detail", r.detail}});
    return rows;
  }
  if (method == "stats.reset") {
    ResetChoiceStats(platform);
    return nullptr;
  }
  if (method == "choicelog.status") {
    const auto info = ChoiceLogStatus(platform);
    return {{"records", info.records}, {"kb", (info.bytes + 1023) / 1024}};
  }
  if (method == "choicelog.clear") {
    if (!ClearChoiceLog(platform))
      throw Error("無法刪除選字紀錄。");
    return nullptr;
  }

  // --- 個人詞庫 -------------------------------------------------------------
  if (method == "personal.status") {
    // refresh：先請輸入法重寫狀態檔（詞數會隨打字變動）
    if (p.value("refresh", false)) {
      platform.SendPersonalCommand(1);
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    bool running = false;
    const std::string text = PersonalStatusText(platform, &running);
    return {{"text", text}, {"running", running}};
  }
  if (method == "personal.command") {
    const unsigned command = p.value("command", 0u);
    if (command < 2 || command > 4)
      throw Error("bad command");
    if (!platform.SendPersonalCommand(command))
      throw Error("無法連線到輸入法服務。");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return nullptr;
  }
  if (method == "words.load") {
    const auto data = LoadPersonalWords(platform);
    json words = json::array();
    for (const auto& [word, score] : data.words)
      words.push_back({word, score});
    json rules = json::array();
    for (const auto& r : data.rules)
      rules.push_back({{"kind", r.kind == WordRule::kAdd     ? "add"
                                : r.kind == WordRule::kBlock ? "block"
                                : r.kind == WordRule::kSplit ? "split"
                                                             : "merge"},
                       {"from", r.from},
                       {"to", r.to}});
    return {{"available", data.available}, {"error", data.error}, {"words", words}, {"rules", rules}};
  }
  if (method == "words.edit") {
    std::vector<std::string> lines;
    for (const auto& line : p.value("lines", json::array()))
      lines.push_back(line.is_string() ? line.get<std::string>() : std::string());
    std::string error;
    if (!SendWordEdits(platform, lines, &error))
      throw Error(error);
    return nullptr;
  }

  // --- 使用者詞典 -----------------------------------------------------------
  if (method == "dicts.list") {
    PrepareUserDictTask();
    return ListUserDicts(s.api);
  }
  if (method == "dicts.pickFile") {
    const std::string op = Str(p, "op");
    const std::string name = Str(p, "name");
    fs::path path;
    if (op == "restore") {
      path = platform.OpenFileDialog(owner, "開啓",
                                     {{"詞典快照 (*.userdb.txt)", "*.userdb.txt"},
                                      {"KCSS格式詞典快照 (*.userdb.kct.snapshot)", "*.userdb.kct.snapshot"},
                                      {"所有檔案", "*.*"}},
                                     "", "snapshot");
    } else {
      const std::vector<FileFilter> filter = {{"文字文件 (*.txt)", "*.txt"}, {"所有檔案", "*.*"}};
      const std::string file_name = name + "_export.txt";
      path = op == "export" ? platform.SaveFileDialog(owner, "另存新檔", filter, file_name, "txt")
                            : platform.OpenFileDialog(owner, "開啓", filter, file_name, "txt");
    }
    return path.empty() ? json(nullptr) : json(U8(path));
  }
  if (method == "dicts.run") {
    const std::string op = Str(p, "op");
    const std::string name = Str(p, "name");
    const fs::path path = Path(Str(p, "path"));
    const std::string path_u8 = U8(path);
    fs::path reveal;
    std::string report, error;
    {
      ServiceMaintenance maintenance(platform);
      PrepareUserDictTask();  // 建立使用者資料同步資料夾
      std::error_code ec;
      if (op == "backup") {
        char dir[1024] = {0};
        s.rime->get_user_data_sync_dir(dir, sizeof(dir));
        const fs::path sync_dir = platform.FromRimePath(dir);
        if (!fs::exists(sync_dir, ec) && !fs::create_directories(sync_dir, ec)) {
          error = "未能完成導出操作。會不會是同步文件夾無法訪問？";
        } else {
          const fs::path file = sync_dir / Path(name + ".userdb.txt");
          if (!s.api->backup_user_dict(name.c_str()))
            error = "不知道哪裏出錯了，未能完成導出操作。";
          else if (!fs::exists(file, ec))
            error = "啛，輸出的快照文件找不着了。";
          else
            reveal = file, report = "已備份「" + name + "」。";
        }
      } else if (op == "restore") {
        if (!s.api->restore_user_dict(path_u8.c_str()))
          error = "不知道哪裏出錯了，未能完成操作。";
        else
          report = "已還原詞典快照。";
      } else if (op == "export") {
        const int result = s.api->export_user_dict(name.c_str(), path_u8.c_str());
        if (result < 0)
          error = "不知道哪裏出錯了，未能完成操作。";
        else if (!fs::exists(path, ec))
          error = "啛，導出的文件找不着了。";
        else
          reveal = path, report = "導出了 " + std::to_string(result) + " 條記錄。";
      } else if (op == "import") {
        const int result = s.api->import_user_dict(name.c_str(), path_u8.c_str());
        if (result < 0)
          error = "不知道哪裏出錯了，未能完成操作。";
        else
          report = "導入了 " + std::to_string(result) + " 條記錄。";
      } else {
        throw Error("bad op");
      }
    }
    if (!error.empty())
      throw Error(error);
    if (!reveal.empty())
      platform.Reveal(reveal);
    return report;
  }

  throw Error("unknown method: " + method);
}

}  // namespace settings
