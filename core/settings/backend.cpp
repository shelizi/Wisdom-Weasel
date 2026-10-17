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
#include "../base/utf8.h"
#include "../ime/calibration.h"
#include "../ime/ranker.h"
#include "../llm/RemoteLLMProvider.h"
#include "bench.h"
#include "ops.h"
#include "rerank_tune.h"
#include "auto_tune.h"
#include "rank_train.h"
#include "taiwan_lexicon.h"
#ifdef _WIN32
#include <windows.h>
#endif

namespace settings {

using json = nlohmann::json;
using Error = Backend::Error;

namespace {

std::string Str(const json& params, const char* key) {
  const auto it = params.find(key);
  return it != params.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// 設定樹讀出來的純量是字串（ConfigToJson），數字欄要容字串或數字
int64_t CfgInt(const json& o, const char* key, int64_t fallback) {
  const auto it = o.find(key);
  if (it == o.end())
    return fallback;
  if (it->is_number())
    return it->get<int64_t>();
  if (it->is_string())
    return std::atoll(it->get<std::string>().c_str());
  return fallback;
}
double CfgDbl(const json& o, const char* key, double fallback) {
  const auto it = o.find(key);
  if (it == o.end())
    return fallback;
  if (it->is_number())
    return it->get<double>();
  if (it->is_string())
    return std::atof(it->get<std::string>().c_str());
  return fallback;
}

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// 評分要用的本機模型：評分專用 → 預測（llamacpp）→ 校正（借用順序同輸入法與網頁）
bool ScorerSpec(const json& llm, LLMLocalModelSpec* spec) {
  const json choice = llm.value("choice", json::object());
  const json scorer = choice.value("scorer", json::object());
  std::string path = scorer.value("model_path", "");
  std::string type = scorer.value("model_type", "");
  if (path.empty()) {
    const json lc = llm.value("llamacpp", json::object());
    if (llm.value("provider_type", "") != "openai") {
      path = lc.value("model_path", "");
      type = lc.value("model_type", "");
    }
  }
  if (path.empty()) {
    const json typo = llm.value("typo", json::object());
    if (typo.value("model_path", "") != "") {
      path = typo.value("model_path", "");
      type = typo.value("model_type", "");
    }
  }
  if (path.empty())
    return false;
  const json lc = llm.value("llamacpp", json::object());
  spec->model_path = path;
  spec->instruct = type != "Base";
  spec->n_ctx = (int)CfgInt(lc, "n_ctx", 2048);
  spec->n_gpu_layers = (int)CfgInt(lc, "n_gpu_layers", 0);
  spec->n_threads = (int)CfgInt(lc, "n_threads", 4);
  spec->disable_thinking = true;
  return true;
}

json AutoResultJson(const settings_rerank::AutoRunResult& r) {
  json j{{"action", r.action},
         {"top", r.top},
         {"margin", r.margin},
         {"message", r.message},
         {"records", r.records},
         {"cases", r.entry.cases},
         {"check",
          {{"judgeable", r.check.judgeable},
           {"degraded", r.check.degraded},
           {"before", r.check.before_rate},
           {"after", r.check.after_rate},
           {"after_n", r.check.after_n}}}};
  if (r.action == "apply" || r.action == "hold") {
    j["current"] = {{"top", r.decision.current.top},
                    {"margin", r.decision.current.margin},
                    {"accuracy", r.decision.current.accuracy},
                    {"wrong", r.decision.current.wrong_change}};
    j["best"] = {{"top", r.decision.best.top},
                 {"margin", r.decision.best.margin},
                 {"accuracy", r.decision.best.accuracy}};
    j["applied"] = {{"top", r.decision.applied.top},
                    {"margin", r.decision.applied.margin},
                    {"accuracy", r.decision.applied.accuracy}};
  }
  return j;
}

#ifdef _WIN32
// 建立隱藏行程並等它結束；回傳結束碼
int RunQuiet(const std::wstring& cmd) {
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  std::wstring mutable_cmd = cmd;
  if (!CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    return -1;
  WaitForSingleObject(pi.hProcess, 30000);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return (int)code;
}

const wchar_t kAutoTuneTask[] = L"WisdomWeaselAutoTune";
#endif

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
  std::thread bench_job;  // 模型測試
  std::shared_ptr<std::atomic<bool>> bench_cancel;
  std::atomic<bool> bench_running{false};
  std::thread tune_job;  // 整句重排自動調整
  std::shared_ptr<std::atomic<bool>> tune_cancel;
  std::atomic<bool> tune_running{false};
  std::atomic<unsigned> probe_id{0};

  // 內建詞庫（台灣詞庫）快取與部署狀態
  fs::path cached_lexicon_dir;
  fs::file_time_type cached_dict_mtime{};
  bool base_lexicon_loaded = false;
  std::string base_lexicon_error;
  taiwan_lexicon::BaseLexicon base_lexicon;
  taiwan_lexicon::SyllableTable syllable_table;
  std::string last_lexicon_deploy_stage;
  std::string last_lexicon_deploy_error;

  State(Platform& p, Options o, Emit e) : platform(p), options(std::move(o)), emit(std::move(e)) {}

  ~State() {
    for (auto* cancel : {&model_cancel, &grammar_cancel, &bench_cancel, &tune_cancel}) {
      if (*cancel)
        **cancel = true;
    }
    if (model_job.joinable())
      model_job.join();
    if (grammar_job.joinable())
      grammar_job.join();
    if (bench_job.joinable())
      bench_job.join();
    if (tune_job.joinable())
      tune_job.join();
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

  // 這個平台的前端做得到哪些事（設定視窗把做不到的選項藏起來）
  static json PlatformJson() {
#ifdef __APPLE__
    // 鼠鬚管：候選窗是鼠鬚管自己畫的，整句機率、切換提示與選單要鼠鬚管那邊接上（見 mac/README.md）；
    // 所有輸入都在主執行緒，送出時不能等在地化；沒有開發終端
    return {{"os", "mac"}, {"dev_console", false}, {"sentence_probability", false},
            {"simplified_menu", false}, {"commit_wait", false}};
#else
    return {{"os", "windows"}, {"dev_console", true}, {"sentence_probability", true},
            {"simplified_menu", true}, {"commit_wait", true}};
#endif
  }

  // 開發：dev_console/enabled（沒設定時輸入法預設開啟）
  json DevJson() {
    RimeConfig config = {0};
    api->settings_get_config(style, &config);
    Bool console = true;
    if (!rime->config_get_bool(&config, "dev_console/enabled", &console))
      console = true;
    return {{"console", !!console}};
  }

  json GrammarJson() {
    std::error_code ec;
    const auto size = fs::file_size(GrammarPath(platform), ec);
    return {{"enabled", GrammarEnabled(platform)},
            {"ready", GrammarReady(platform)},
            {"size_mb", ec ? 0 : (int)(size >> 20)},
            {"downloading", grammar_job.joinable() && grammar_cancel && !*grammar_cancel}};
  }

  fs::path SharedLexiconDir() {
#ifdef _WIN32
    wchar_t env[MAX_PATH * 2] = {0};
    if (GetEnvironmentVariableW(L"WISDOM_LEXICON_SHARED_DIR", env, _countof(env)) && env[0])
      return fs::path(env);
#else
    if (const char* env = std::getenv("WISDOM_LEXICON_SHARED_DIR")) {
      if (*env)
        return Path(env);
    }
#endif
    return platform.FromRimePath(rime->get_shared_data_dir());
  }

  bool EnsureBaseLexicon(const taiwan_lexicon::BaseLexicon** out_base,
                         const taiwan_lexicon::SyllableTable** out_syllables,
                         std::string* error) {
    const fs::path shared_dir = SharedLexiconDir();
    std::error_code ec;
    for (const char* name : taiwan_lexicon::kFiles) {
      const fs::path f = shared_dir / name;
      if (!fs::exists(f, ec) || fs::is_directory(f, ec)) {
        if (error)
          *error = "內建詞庫資源缺失：找不到 " + std::string(name);
        return false;
      }
    }
    try {
      const std::string m_text = taiwan_lexicon::Read(shared_dir / "wisdom_taiwan.manifest.json");
      const json m_json = json::parse(m_text, nullptr, false);
      if (!m_json.is_object() || m_json.value("words", 0) <= 0 || m_json.value("entries", 0) <= 0) {
        if (error)
          *error = "內建詞庫來源資訊檔 (wisdom_taiwan.manifest.json) 格式損壞。";
        return false;
      }
    } catch (const std::exception& ex) {
      if (error)
        *error = std::string("無法讀取 wisdom_taiwan.manifest.json：") + ex.what();
      return false;
    }

    const fs::path dict_path = shared_dir / "wisdom_taiwan.dict.yaml";
    const auto mtime = fs::last_write_time(dict_path, ec);
    if (!base_lexicon_loaded || cached_lexicon_dir != shared_dir || cached_dict_mtime != mtime) {
      base_lexicon_loaded = false;
      base_lexicon_error.clear();
      try {
        const std::string dict_text = taiwan_lexicon::Read(dict_path);
        if (!taiwan_lexicon::ParseDictText(dict_text, &base_lexicon, &base_lexicon_error)) {
          if (error)
            *error = base_lexicon_error;
          return false;
        }
        const fs::path rime_shared = platform.FromRimePath(rime->get_shared_data_dir());
        taiwan_lexicon::LoadSyllableTable(shared_dir, rime_shared, &syllable_table);
        cached_lexicon_dir = shared_dir;
        cached_dict_mtime = mtime;
        base_lexicon_loaded = true;
      } catch (const std::exception& ex) {
        base_lexicon_error = std::string("無法讀取內建詞庫：") + ex.what();
        if (error)
          *error = base_lexicon_error;
        return false;
      }
    }
    if (out_base)
      *out_base = &base_lexicon;
    if (out_syllables)
      *out_syllables = &syllable_table;
    return true;
  }

  json LexiconJson(bool refresh_service) {
    const fs::path shared_dir = SharedLexiconDir();
    const fs::path user_dir = platform.UserDataDir();
    std::error_code ec;

    const taiwan_lexicon::BaseLexicon* base = nullptr;
    const taiwan_lexicon::SyllableTable* syllables = nullptr;
    std::string resource_error;
    const bool resource_ok = EnsureBaseLexicon(&base, &syllables, &resource_error);

    json manifest = json::object();
    std::string notice;
    uint64_t sources_zip_size = 0;
    const bool sources_zip_exists =
        fs::exists(shared_dir / "wisdom_taiwan.sources.zip", ec) && !ec;
    if (sources_zip_exists)
      sources_zip_size = fs::file_size(shared_dir / "wisdom_taiwan.sources.zip", ec);
    if (resource_ok) {
      try {
        manifest = json::parse(taiwan_lexicon::Read(shared_dir / "wisdom_taiwan.manifest.json"),
                               nullptr, false);
        if (!manifest.is_object())
          manifest = json::object();
      } catch (...) {
      }
      try {
        notice = taiwan_lexicon::Read(shared_dir / "wisdom_taiwan.NOTICE.txt");
      } catch (...) {
      }
    }

    taiwan_lexicon::UserConfig user_cfg;
    std::string user_cfg_error;
    if (!taiwan_lexicon::LoadUserConfig(rime, user_dir, &user_cfg, &user_cfg_error)) {
      user_cfg = taiwan_lexicon::UserConfig{};
    }

    const std::string base_version = (resource_ok && base) ? base->version : "";
    const std::string expected_dict_version =
        taiwan_lexicon::EffectiveDictVersion(base_version, user_cfg.edits, user_cfg.edits_revision);

    json edits_json = json::array();
    size_t conflict_count = 0;
    size_t hidden_count = 0;
    for (const auto& e : user_cfg.edits) {
      json ej = taiwan_lexicon::EditToJson(e);
      bool in_base = false;
      int base_w = e.base_weight;
      if (resource_ok && base) {
        const auto it = base->weights.find({e.word, e.code});
        if (it != base->weights.end()) {
          in_base = true;
          base_w = it->second;
        }
      }
      const bool upstream_removed = resource_ok && !in_base && e.origin_base && !e.hidden;
      const bool dedup_upstream = resource_ok && in_base && !e.origin_base && !e.hidden;
      if (upstream_removed)
        ++conflict_count;
      if (e.hidden)
        ++hidden_count;
      ej["in_base"] = in_base;
      ej["base_weight"] = base_w;
      ej["effective_weight"] = taiwan_lexicon::EffectiveWeight(in_base, base_w, e.pref);
      ej["upstream_removed"] = upstream_removed;
      ej["dedup_upstream"] = dedup_upstream;
      edits_json.push_back(std::move(ej));
    }

    // 檢查目前啟用的輸入方案是否包含適用方案（bopomofo / bopomofo_express / bopomofo_tw）
    const json schemas_info = SchemasJson();
    bool schema_applicable = false;
    json selected_schemas = json::array();
    json selected_applicable = json::array();
    for (const auto& s_item : schemas_info.value("list", json::array())) {
      if (s_item.value("selected", false)) {
        const std::string sid = s_item.value("id", "");
        selected_schemas.push_back(sid);
        for (const char* app : taiwan_lexicon::kApplicableSchemas) {
          if (sid == app) {
            schema_applicable = true;
            selected_applicable.push_back(sid);
          }
        }
      }
    }

    // 檢查磁碟上已部署的檔案狀態
    bool patch_present = false;
    {
      const fs::path bopo_custom = user_dir / "bopomofo.custom.yaml";
      if (fs::exists(bopo_custom, ec)) {
        try {
          const std::string text = taiwan_lexicon::Read(bopo_custom);
          patch_present = text.find(taiwan_lexicon::kBegin) != std::string::npos;
        } catch (...) {
        }
      }
    }
    std::string deployed_dict_version;
    {
      const fs::path user_dict = user_dir / "wisdom_taiwan.dict.yaml";
      if (fs::exists(user_dict, ec)) {
        std::ifstream in(user_dict);
        std::string line;
        for (int i = 0; i < 25 && std::getline(in, line); ++i) {
          const std::string t = Trim(line);
          if (t == "...")
            break;
          if (t.rfind("version:", 0) == 0) {
            std::string v = Trim(t.substr(8));
            if (v.size() >= 2 && ((v.front() == '\'' && v.back() == '\'') ||
                                  (v.front() == '"' && v.back() == '"'))) {
              v = v.substr(1, v.size() - 2);
            }
            deployed_dict_version = v;
            break;
          }
        }
      }
    }
    const bool table_compiled_on_disk =
        fs::exists(user_dir / "build" / "wisdom_taiwan.table.bin", ec) && !ec;
    const bool deployed_on_disk =
        user_cfg.enabled
            ? (patch_present && table_compiled_on_disk &&
               (!resource_ok || deployed_dict_version == expected_dict_version))
            : !patch_present;

    // 向執行中的輸入法服務查詢實際載入狀態（lexicon_runtime.txt）
    auto resolve_rt_path = [&]() -> fs::path {
      const fs::path p1 = user_dir / "lexicon_runtime.txt";
      const fs::path p2 = user_dir / "personal" / "lexicon_runtime.txt";
      std::error_code e1, e2;
      const bool ex1 = fs::exists(p1, e1) && !e1;
      const bool ex2 = fs::exists(p2, e2) && !e2;
      if (ex1 && ex2) {
        return fs::last_write_time(p2, e2) > fs::last_write_time(p1, e1) ? p2 : p1;
      }
      return ex2 ? p2 : p1;
    };
    fs::path rt_path = resolve_rt_path();
    const auto before_mtime =
        fs::exists(rt_path, ec) ? fs::last_write_time(rt_path, ec) : (fs::file_time_type::min)();
    const bool ipc_connected = platform.SendPersonalCommand(1);
    if (ipc_connected && (refresh_service || !fs::exists(rt_path, ec))) {
      for (int i = 0; i < 12; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        rt_path = resolve_rt_path();
        if (fs::exists(rt_path, ec) && fs::last_write_time(rt_path, ec) > before_mtime)
          break;
      }
    }

    int64_t rt_ts = 0;
    bool rt_pack_mounted = false;
    bool rt_table_compiled = false;
    bool rt_force_online = false;
    std::string rt_loaded_version;
    rt_path = resolve_rt_path();
    if (fs::exists(rt_path, ec)) {
      std::ifstream in(rt_path);
      std::string line;
      while (std::getline(in, line)) {
        const std::string t = Trim(line);
        if (t.rfind("ts=", 0) == 0)
          rt_ts = std::atoll(t.substr(3).c_str());
        else if (t.rfind("pack_mounted=", 0) == 0)
          rt_pack_mounted = (t.substr(13) == "1");
        else if (t.rfind("table_compiled=", 0) == 0)
          rt_table_compiled = (t.substr(15) == "1");
        else if (t.rfind("loaded_dict_version=", 0) == 0)
          rt_loaded_version = t.substr(20);
        else if (t.rfind("force_online=", 0) == 0)
          rt_force_online = (t.substr(13) == "1");
      }
    }

    const bool service_responding = (ipc_connected || rt_force_online) && rt_ts > 0;
    bool service_confirmed = false;
    if (service_responding && deployed_on_disk) {
      if (user_cfg.enabled) {
        service_confirmed = rt_pack_mounted && rt_table_compiled &&
                            (!resource_ok || rt_loaded_version == expected_dict_version);
      } else {
        service_confirmed = !rt_pack_mounted;
      }
    }

    const bool deploy_failed = !last_lexicon_deploy_error.empty() || !user_cfg_error.empty();
    const std::string deploy_stage = !last_lexicon_deploy_stage.empty()
                                         ? last_lexicon_deploy_stage
                                         : (!user_cfg_error.empty() ? "讀取使用者詞庫設定檔" : "");
    const std::string deploy_error = !last_lexicon_deploy_error.empty() ? last_lexicon_deploy_error
                                                                        : user_cfg_error;

    // 組合狀態標籤：不適用目前方案與服務載入待確認可同時並存
    json badges = json::array();
    std::string primary_state = "pending_confirmation";
    if (!resource_ok) {
      primary_state = "resource_error";
      badges.push_back({{"code", "resource_error"}, {"label", "資源缺失或損壞"}, {"tone", "danger"}});
    } else if (deploy_failed) {
      primary_state = "deploy_failed";
      badges.push_back(
          {{"code", "deploy_failed"}, {"label", "部署失敗／服務無回應"}, {"tone", "danger"}});
    } else if (!user_cfg.enabled) {
      if (!patch_present) {
        primary_state = "disabled";
        badges.push_back({{"code", "disabled"}, {"label", "已停用"}, {"tone", "muted"}});
      } else {
        primary_state = "pending_confirmation";
        badges.push_back(
            {{"code", "pending_confirmation"}, {"label", "已部署，載入待確認"}, {"tone", "info"}});
      }
    } else {
      if (!schema_applicable) {
        badges.push_back(
            {{"code", "schema_not_applicable"}, {"label", "不適用目前方案"}, {"tone", "warn"}});
      }
      if (service_confirmed && schema_applicable) {
        primary_state = "active";
        badges.push_back({{"code", "active"}, {"label", "已啟用"}, {"tone", "ok"}});
      } else {
        primary_state = !schema_applicable ? "schema_not_applicable" : "pending_confirmation";
        if (!service_confirmed) {
          badges.push_back(
              {{"code", "pending_confirmation"}, {"label", "已部署，載入待確認"}, {"tone", "info"}});
        }
      }
    }

    return {{"id", taiwan_lexicon::kLexiconId},
            {"name", "台灣詞庫"},
            {"description", "繁體中文（台灣）公開常用詞彙擴充包（整合小麥注音、新酷音與本專案補充詞）"},
            {"applicable_schemas", {"bopomofo", "bopomofo_express", "bopomofo_tw"}},
            {"applicable_schema_names", {"注音", "注音（快打）", "注音（臺灣正體）"}},
            {"resource_ok", resource_ok},
            {"resource_error", resource_error},
            {"version", base_version},
            {"words", manifest.value("words", 0)},
            {"entries", manifest.value("entries", resource_ok && base ? (int)base->entries.size() : 0)},
            {"supplement_entries", manifest.value("supplement_entries", 0)},
            {"manifest", manifest},
            {"notice", notice},
            {"sources_zip_exists", sources_zip_exists},
            {"sources_zip_size", sources_zip_size},
            {"sources_zip_size_text", sources_zip_exists ? FormatSize(sources_zip_size) : ""},
            {"enabled", user_cfg.enabled},
            {"user_format_version", user_cfg.version},
            {"edits_revision", user_cfg.edits_revision},
            {"expected_dict_version", expected_dict_version},
            {"edits", edits_json},
            {"edits_count", user_cfg.edits.size()},
            {"conflict_count", conflict_count},
            {"hidden_count", hidden_count},
            {"status",
             {{"state", primary_state},
              {"badges", badges},
              {"enabled", user_cfg.enabled},
              {"schema_applicable", schema_applicable},
              {"selected_schemas", selected_schemas},
              {"selected_applicable_schemas", selected_applicable},
              {"deployed_on_disk", deployed_on_disk},
              {"patch_present", patch_present},
              {"table_compiled", table_compiled_on_disk},
              {"deployed_dict_version", deployed_dict_version},
              {"expected_dict_version", expected_dict_version},
              {"service_responding", service_responding},
              {"service_confirmed", service_confirmed},
              {"runtime_pack_mounted", rt_pack_mounted},
              {"runtime_table_compiled", rt_table_compiled},
              {"runtime_loaded_version", rt_loaded_version},
              {"deploy_failed", deploy_failed},
              {"deploy_stage", deploy_stage},
              {"deploy_error", deploy_error}}}};
  }

  json LexiconsJson(bool refresh_service) {
    return {{"taiwan", LexiconJson(refresh_service)}};
  }

  json SearchLexicon(const json& p) {
    const std::string id = p.value("id", std::string(taiwan_lexicon::kLexiconId));
    if (id != taiwan_lexicon::kLexiconId)
      throw Error("不支援的詞庫識別：「" + id + "」。");
    const taiwan_lexicon::BaseLexicon* base = nullptr;
    const taiwan_lexicon::SyllableTable* syllables = nullptr;
    std::string res_err;
    if (!EnsureBaseLexicon(&base, &syllables, &res_err))
      throw Error(res_err);

    taiwan_lexicon::UserConfig saved_cfg;
    taiwan_lexicon::LoadUserConfig(rime, platform.UserDataDir(), &saved_cfg);
    std::map<std::pair<std::string, std::string>, taiwan_lexicon::UserEdit> saved_map;
    for (const auto& e : saved_cfg.edits)
      saved_map[{e.word, e.code}] = e;

    std::vector<taiwan_lexicon::UserEdit> active_edits;
    if (p.contains("pending_edits") && p["pending_edits"].is_array()) {
      for (const auto& item : p["pending_edits"]) {
        taiwan_lexicon::UserEdit e;
        if (taiwan_lexicon::EditFromJson(item, &e))
          active_edits.push_back(std::move(e));
      }
    } else {
      active_edits = saved_cfg.edits;
    }

    std::map<std::pair<std::string, std::string>, taiwan_lexicon::UserEdit> edit_map;
    for (const auto& e : active_edits)
      edit_map[{e.word, e.code}] = e;

    const std::string q_raw = Str(p, "query");
    const std::string q_trimmed = taiwan_lexicon::TrimSpaces(q_raw);
    const std::string q_compact = taiwan_lexicon::CompactZhuyin(q_trimmed);
    const std::string q_lower = ToLower(q_trimmed);
    const std::string filter = p.value("filter", "all");
    const size_t offset = (size_t)(std::max)(0, p.value("offset", 0));
    const size_t limit = (size_t)(std::min)(200, (std::max)(1, p.value("limit", 50)));

    // 計算統計筆數
    size_t count_modified = active_edits.size();
    size_t count_hidden = 0;
    size_t count_conflicts = 0;
    size_t extra_non_base = 0;
    for (const auto& [key, e] : edit_map) {
      const bool in_base = base->weights.count(key) > 0;
      if (!in_base)
        ++extra_non_base;
      if (e.hidden)
        ++count_hidden;
      if (!in_base && e.origin_base && !e.hidden)
        ++count_conflicts;
    }
    const size_t count_all = base->entries.size() + extra_non_base;

    auto make_row = [&](const std::string& word, const std::string& code,
                        const std::string& zhuyin, bool in_base, int base_w) -> json {
      const std::pair<std::string, std::string> key{word, code};
      const auto eit = edit_map.find(key);
      const bool has_edit = (eit != edit_map.end());
      const auto sit = saved_map.find(key);
      const bool has_saved = (sit != saved_map.end());
      const bool dirty = (has_edit != has_saved) || (has_edit && eit->second != sit->second);

      std::string pref = has_edit ? eit->second.pref : "normal";
      bool hidden = has_edit ? eit->second.hidden : false;
      bool origin_base = has_edit ? eit->second.origin_base : in_base;
      int eff_w = taiwan_lexicon::EffectiveWeight(
          in_base, in_base ? base_w : (has_edit ? eit->second.base_weight : 1000), pref);

      std::string status = "builtin";
      bool upstream_removed = false;
      bool dedup_upstream = false;
      if (has_edit) {
        if (hidden) {
          status = "hidden";
        } else if (!in_base && origin_base) {
          status = "conflict_removed";
          upstream_removed = true;
        } else if (!in_base && !origin_base) {
          status = "added";
        } else {
          status = "modified";
          if (in_base && !origin_base)
            dedup_upstream = true;
        }
      }
      json row = {{"word", word},
                  {"code", code},
                  {"zhuyin", zhuyin},
                  {"in_base", in_base},
                  {"base_weight", in_base ? base_w : (has_edit ? eit->second.base_weight : 1000)},
                  {"effective_weight", eff_w},
                  {"pref", pref},
                  {"hidden", hidden},
                  {"modified", has_edit},
                  {"dirty", dirty},
                  {"origin_base", origin_base},
                  {"status", status},
                  {"upstream_removed", upstream_removed},
                  {"dedup_upstream", dedup_upstream}};
      if (has_edit && !eit->second.replaced_from_word.empty()) {
        row["replaced_from_word"] = eit->second.replaced_from_word;
        row["replaced_from_code"] = eit->second.replaced_from_code;
        row["replaced_from_zhuyin"] = taiwan_lexicon::CodeToZhuyin(eit->second.replaced_from_code);
      }
      return row;
    };

    auto matches_query = [&](const std::string& word, const std::string& code,
                             const std::string& zhuyin, const std::string& zh_compact,
                             int* match_rank) -> bool {
      if (q_trimmed.empty()) {
        *match_rank = 0;
        return true;
      }
      if (word == q_trimmed || zhuyin == q_trimmed ||
          (!q_compact.empty() && zh_compact == q_compact) ||
          (!q_lower.empty() && code == q_lower)) {
        *match_rank = 3;
        return true;
      }
      if (word.rfind(q_trimmed, 0) == 0 || zhuyin.rfind(q_trimmed, 0) == 0 ||
          (!q_compact.empty() && zh_compact.rfind(q_compact, 0) == 0) ||
          (!q_lower.empty() && code.rfind(q_lower, 0) == 0)) {
        *match_rank = 2;
        return true;
      }
      if (word.find(q_trimmed) != std::string::npos ||
          zhuyin.find(q_trimmed) != std::string::npos ||
          (!q_compact.empty() && zh_compact.find(q_compact) != std::string::npos) ||
          (!q_lower.empty() && code.find(q_lower) != std::string::npos)) {
        *match_rank = 1;
        return true;
      }
      return false;
    };

    // 快速路徑：無搜尋字詞且篩選為 all，直接將個人修改置頂後對基底詞庫分頁切片
    if (q_trimmed.empty() && filter == "all") {
      std::vector<json> top_rows;
      for (const auto& [key, e] : edit_map) {
        const auto bit = base->weights.find(key);
        const bool in_base = (bit != base->weights.end());
        const int bw = in_base ? bit->second : e.base_weight;
        top_rows.push_back(
            make_row(e.word, e.code, taiwan_lexicon::CodeToZhuyin(e.code), in_base, bw));
      }
      json rows = json::array();
      size_t cur_idx = 0;
      for (const auto& r : top_rows) {
        if (cur_idx >= offset && rows.size() < limit)
          rows.push_back(r);
        ++cur_idx;
      }
      for (const auto& be : base->entries) {
        if (edit_map.count({be.word, be.code}))
          continue;
        if (cur_idx >= offset && rows.size() < limit)
          rows.push_back(make_row(be.word, be.code, be.zhuyin, true, be.weight));
        ++cur_idx;
        if (rows.size() >= limit && cur_idx >= offset + limit)
          break;
      }
      return {{"total", count_all},
              {"offset", offset},
              {"limit", limit},
              {"counts",
               {{"all", count_all},
                {"modified", count_modified},
                {"hidden", count_hidden},
                {"conflicts", count_conflicts}}},
              {"rows", rows}};
    }

    struct CandidateHit {
      std::string word;
      std::string code;
      std::string zhuyin;
      bool in_base = false;
      int base_weight = 0;
      int effective_weight = 0;
      bool modified = false;
      int match_rank = 0;
    };
    std::vector<CandidateHit> hits;

    // 1. 先掃描不在基底詞庫中的個人修改（新增或上游已移除）
    for (const auto& [key, e] : edit_map) {
      if (base->weights.count(key))
        continue;
      if (filter == "hidden" && !e.hidden)
        continue;
      const std::string zh = taiwan_lexicon::CodeToZhuyin(e.code);
      const std::string zhc = taiwan_lexicon::CompactZhuyin(zh);
      int mrank = 0;
      if (!matches_query(e.word, e.code, zh, zhc, &mrank))
        continue;
      hits.push_back({e.word, e.code, zh, false, e.base_weight,
                      taiwan_lexicon::EffectiveWeight(false, e.base_weight, e.pref), true, mrank});
    }

    // 2. 掃描基底詞庫（若只看修改或已隱藏，只需檢查 edit_map 即可！）
    if (filter == "modified" || filter == "hidden") {
      for (const auto& [key, e] : edit_map) {
        const auto bit = base->weights.find(key);
        if (bit == base->weights.end())
          continue;
        if (filter == "hidden" && !e.hidden)
          continue;
        const std::string zh = taiwan_lexicon::CodeToZhuyin(e.code);
        const std::string zhc = taiwan_lexicon::CompactZhuyin(zh);
        int mrank = 0;
        if (!matches_query(e.word, e.code, zh, zhc, &mrank))
          continue;
        hits.push_back({e.word, e.code, zh, true, bit->second,
                        taiwan_lexicon::EffectiveWeight(true, bit->second, e.pref), true, mrank});
      }
    } else {
      for (const auto& be : base->entries) {
        int mrank = 0;
        if (!matches_query(be.word, be.code, be.zhuyin, be.zhuyin_compact, &mrank))
          continue;
        const auto eit = edit_map.find({be.word, be.code});
        const bool has_edit = (eit != edit_map.end());
        const std::string pref = has_edit ? eit->second.pref : "normal";
        hits.push_back({be.word, be.code, be.zhuyin, true, be.weight,
                        taiwan_lexicon::EffectiveWeight(true, be.weight, pref), has_edit, mrank});
      }
    }

    std::sort(hits.begin(), hits.end(), [](const CandidateHit& a, const CandidateHit& b) {
      if (a.match_rank != b.match_rank)
        return a.match_rank > b.match_rank;
      if (a.modified != b.modified)
        return a.modified > b.modified;
      if (a.effective_weight != b.effective_weight)
        return a.effective_weight > b.effective_weight;
      if (a.word != b.word)
        return a.word < b.word;
      return a.code < b.code;
    });

    json rows = json::array();
    for (size_t i = offset; i < hits.size() && rows.size() < limit; ++i) {
      const auto& h = hits[i];
      rows.push_back(make_row(h.word, h.code, h.zhuyin, h.in_base, h.base_weight));
    }
    return {{"total", hits.size()},
            {"offset", offset},
            {"limit", limit},
            {"counts",
             {{"all", count_all},
              {"modified", count_modified},
              {"hidden", count_hidden},
              {"conflicts", count_conflicts}}},
            {"rows", rows}};
  }

  // 與原本設定視窗的「套用」相同的順序：方案 → llm → 注音排序 → Rime 容錯 → 外觀 → 語言模型 → 內建詞庫 → 部署
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
    if (c.contains("sentences") && c["sentences"].is_number_integer()) {
      std::string error;
      if (!ApplySentenceCandidates(platform, c["sentences"].get<int>(), &error))
        add_error("整句候選：", error);
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
    const bool dev_changed = c.contains("dev_console") && c["dev_console"].is_boolean();
    if (dev_changed)
      api->customize_bool(style, "dev_console/enabled", c["dev_console"].get<bool>());
    if (style_changed || llm_changed || dev_changed) {
      api->save_settings(style);
      saved = true;
    }
    if (c.contains("grammar")) {
      std::string error;
      if (!ApplyGrammar(platform, c["grammar"].get<bool>(), &error))
        add_error("語言模型：", error);
      saved = true;
    }
    if (c.contains("lexicon") && c["lexicon"].is_object()) {
      last_lexicon_deploy_stage.clear();
      last_lexicon_deploy_error.clear();
      const json& lex = c["lexicon"];
      const std::string lex_id = lex.value("id", std::string(taiwan_lexicon::kLexiconId));
      if (lex_id != taiwan_lexicon::kLexiconId) {
        last_lexicon_deploy_stage = "驗證詞庫識別";
        last_lexicon_deploy_error = "不支援的詞庫識別：「" + lex_id + "」";
        throw Error("台灣詞庫（" + last_lexicon_deploy_stage + "）：" + last_lexicon_deploy_error);
      }
      const taiwan_lexicon::BaseLexicon* base = nullptr;
      const taiwan_lexicon::SyllableTable* syllables = nullptr;
      std::string err;
      if (!EnsureBaseLexicon(&base, &syllables, &err)) {
        last_lexicon_deploy_stage = "讀取與驗證內建詞庫資源";
        last_lexicon_deploy_error = err;
        throw Error("台灣詞庫（" + last_lexicon_deploy_stage + "）：" + err);
      }

      const fs::path user_dir = platform.UserDataDir();
      const fs::path pref_path = user_dir / "wisdom_taiwan.user.yaml";
      std::error_code ec;
      const bool pref_existed = fs::exists(pref_path, ec);
      const std::string pref_before = pref_existed ? taiwan_lexicon::Read(pref_path) : std::string();

      taiwan_lexicon::UserConfig old_cfg;
      if (!taiwan_lexicon::LoadUserConfig(rime, user_dir, &old_cfg, &err)) {
        old_cfg = taiwan_lexicon::UserConfig{};
      }

      std::vector<taiwan_lexicon::UserEdit> raw_edits;
      if (lex.contains("edits") && lex["edits"].is_array()) {
        for (const auto& item : lex["edits"]) {
          taiwan_lexicon::UserEdit e;
          if (!taiwan_lexicon::EditFromJson(item, &e)) {
            last_lexicon_deploy_stage = "驗證個人修改項目";
            last_lexicon_deploy_error = "個人修改項目缺少必要欄位（詞彙或讀音）。";
            throw Error("台灣詞庫（" + last_lexicon_deploy_stage + "）：" +
                        last_lexicon_deploy_error);
          }
          raw_edits.push_back(std::move(e));
        }
      } else {
        raw_edits = old_cfg.edits;
      }

      std::vector<taiwan_lexicon::UserEdit> validated_edits;
      if (!taiwan_lexicon::ValidateEditsList(raw_edits, *syllables, &validated_edits, &err)) {
        last_lexicon_deploy_stage = "驗證個人修改與注音讀音";
        last_lexicon_deploy_error = err;
        throw Error("台灣詞庫（" + last_lexicon_deploy_stage + "）：" + err);
      }

      taiwan_lexicon::UserConfig new_cfg = old_cfg;
      new_cfg.enabled = lex.value("enabled", old_cfg.enabled);
      const bool edits_changed = (validated_edits != old_cfg.edits);
      new_cfg.edits = std::move(validated_edits);
      if (edits_changed)
        new_cfg.edits_revision = old_cfg.edits_revision + 1;
      if (!new_cfg.edits.empty() || old_cfg.version >= 1 || new_cfg.edits_revision > 0)
        new_cfg.version = taiwan_lexicon::kUserFormatVersion;
      new_cfg.deployed_base_version = base->version;
      new_cfg.deployed_edits_revision = new_cfg.edits_revision;

      if (!taiwan_lexicon::SaveUserConfigWithMigrationBackup(rime, user_dir, new_cfg, &err)) {
        last_lexicon_deploy_stage = "寫入使用者詞庫設定檔 (wisdom_taiwan.user.yaml)";
        last_lexicon_deploy_error = err;
        throw Error("台灣詞庫（" + last_lexicon_deploy_stage + "）：" + err);
      }

      bool prep_changed = false;
      std::string prep_stage;
      if (!taiwan_lexicon::Prepare(rime, SharedLexiconDir(), user_dir, &err, &prep_changed,
                                   &prep_stage)) {
        // 回滾 wisdom_taiwan.user.yaml 至套用前狀態
        try {
          if (pref_existed)
            taiwan_lexicon::Write(pref_path, pref_before);
          else
            fs::remove(pref_path, ec);
        } catch (...) {
        }
        last_lexicon_deploy_stage =
            prep_stage.empty() ? "產生有效詞庫與更新方案設定" : prep_stage;
        last_lexicon_deploy_error = err;
        throw Error("台灣詞庫部署失敗（階段：" + last_lexicon_deploy_stage + "）：" + err);
      }
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
            {"grammar", GrammarJson()},
            {"dev", DevJson()},
            {"lexicons", LexiconsJson(true)}};
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
  static const std::set<std::string> kUi = {"models.pickFile", "dicts.pickFile", "lexicon.pickFile",
                                            "app.setTheme"};
  static const std::set<std::string> kRime = {
      "init",         "schemas.details",      "schemas.install",    "style.preview",
      "settings.apply", "dicts.list",         "dicts.run",          "llm.reload",
      "rerank.auto",  "rerank.apply",         "rank.train",         "lexicon.status",
      "lexicon.search", "lexicon.validateEdit", "lexicon.exportEdits", "lexicon.importPreview",
      "lexicon.sourceArchive"};
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
            {"dev", s.DevJson()},
            {"lexicons", s.LexiconsJson(false)},
            {"platform", s.PlatformJson()},
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

  // --- 模型測試：設定程式自己載入模型，跑預測與校正的題目 --------------------------
  if (method == "bench.start") {
    std::vector<std::wstring> errors;
    const auto predict = settings_bench::ParsePredictCases(utf8::ToWide(Str(p, "predict")), &errors);
    const auto correct = settings_bench::ParseCorrectCases(utf8::ToWide(Str(p, "correct")), &errors);
    if (!errors.empty()) {
      std::string text;
      for (const auto& e : errors)
        text += (text.empty() ? "" : "\n") + utf8::FromWide(e);
      throw Error(text);
    }
    std::vector<settings_bench::Profile> profiles;
    for (const auto& q : p.value("profiles", json::array())) {
      settings_bench::Profile profile;
      profile.name = q.value("name", "");
      profile.remote = q.value("remote", false);
      profile.model_path = q.value("model_path", "");
      profile.instruct = q.value("model_type", "Instruct") != "Base";
      profile.api_url = q.value("api_url", "");
      profile.api_key = q.value("api_key", "");
      profile.model = q.value("model", "");
      profile.no_think = q.value("no_think", false);
      profile.think_tokens = q.value("think_tokens", 2048);
      profiles.push_back(std::move(profile));
    }
    if (profiles.empty())
      throw Error("請先勾選要測試的模型。");
    if (predict.empty() && correct.empty())
      throw Error("沒有題目。");
    settings_bench::Options options;
    const json o = p.value("options", json::object());
    options.prompt = utf8::ToWide(o.value("prompt", ""));
    options.typo_prompt = utf8::ToWide(o.value("typo_prompt", ""));
    options.n_ctx = o.value("n_ctx", 2048);
    options.n_gpu_layers = o.value("n_gpu_layers", 0);
    options.n_threads = o.value("n_threads", 4);
    options.temperature = o.value("temperature", 0.8);

    std::lock_guard<std::mutex> lock(s.job_mutex);
    if (s.bench_running)
      throw Error("模型測試正在進行中。");
    if (s.bench_job.joinable())
      s.bench_job.join();
    s.bench_cancel = std::make_shared<std::atomic<bool>>(false);
    s.bench_running = true;
    Emit emit = s.emit;
    auto cancel = s.bench_cancel;
    s.bench_job = std::thread([&s, emit, cancel, profiles, options, predict, correct]() {
      // 先請輸入法放掉它的模型（預測、校正、評分），測試的模型才有完整的顯示卡記憶體，
      // 反應時間也不會被輸入法的推理拖慢。輸入法放完會寫 llm_paused.txt；沒在執行就不用等
      const fs::path paused = s.platform.UserDataDir() / "llm_paused.txt";
      std::error_code ec;
      fs::remove(paused, ec);
      if (s.platform.SendPersonalCommand(9)) {
        emit("bench", {{"state", "pause"}, {"message", "暫停輸入法的模型…"}});
        for (int i = 0; i < 100 && !*cancel && !fs::exists(paused, ec); ++i)
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      try {
        settings_bench::Run(profiles, options, predict, correct, *cancel,
                            [&emit](const json& e) { emit("bench", e); });
        emit("bench", {{"state", *cancel ? "cancelled" : "done"}});
      } catch (const std::exception& e) {
        emit("bench", {{"state", "error"}, {"message", e.what()}});
      }
      s.platform.SendPersonalCommand(10);  // 輸入法重新載入模型
      s.bench_running = false;
    });
    return {{"started", true}, {"predict", predict.size()}, {"correct", correct.size()}};
  }
  if (method == "bench.cancel") {
    std::lock_guard<std::mutex> lock(s.job_mutex);
    if (s.bench_cancel)
      *s.bench_cancel = true;
    return nullptr;
  }

  // --- 整句重排自動調整：讀選字紀錄的 Top-K，用現在的評分模型掃門檻 × 評幾句 ------
  // 同 bench.start：背景執行緒先請輸入法放掉模型，評完再請它載回
  if (method == "rerank.tune") {
    const json q = p.value("profile", json::object());
    const bool remote = q.value("remote", false);
    const std::string model_path = q.value("model_path", "");
    if (remote)
      throw Error("整句重排的評分只能用本機模型（llama.cpp / GGUF）。");
    if (model_path.empty())
      throw Error(
          "請先在「語言模型」頁幫評分選一組本機模型（或在「各功能使用的模型」選評分用的模型）。");
    const json o = p.value("options", json::object());
    LLMLocalModelSpec spec;
    spec.model_path = model_path;
    spec.instruct = q.value("model_type", "Instruct") != "Base";
    spec.n_ctx = o.value("n_ctx", 2048);
    spec.n_gpu_layers = o.value("n_gpu_layers", 0);
    spec.n_threads = o.value("n_threads", 4);
    spec.disable_thinking = true;
    const size_t context_chars = (size_t)p.value("context", 30);
    const double length_alpha = p.value("alpha", 0.0);
    const double personal_weight = p.value("personal", 0.0);

    std::lock_guard<std::mutex> lock(s.job_mutex);
    if (s.tune_running)
      throw Error("自動調整正在進行中。");
    if (s.tune_job.joinable())
      s.tune_job.join();
    s.tune_cancel = std::make_shared<std::atomic<bool>>(false);
    s.tune_running = true;
    Emit emit = s.emit;
    auto cancel = s.tune_cancel;
    s.tune_job =
        std::thread([&s, emit, cancel, spec, context_chars, length_alpha, personal_weight]() {
      const fs::path paused = s.platform.UserDataDir() / "llm_paused.txt";
      std::error_code ec;
      fs::remove(paused, ec);
      if (s.platform.SendPersonalCommand(9)) {
        emit("rerank_tune", {{"state", "pause"}, {"message", "暫停輸入法的模型…"}});
        for (int i = 0; i < 100 && !*cancel && !fs::exists(paused, ec); ++i)
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      try {
        std::vector<settings_rerank::LoggedCase> cases;
        settings_rerank::LoadLoggedTopK(PersonalDir(s.platform), &cases);
        if (cases.size() < 30) {
          emit("rerank_tune",
               {{"state", "error"},
                {"message", "有整句候選的選字紀錄只有 " + std::to_string(cases.size()) +
                                " 筆（至少 30 筆）。請開啟「選字紀錄」後打一些字再試"
                                "（重排關著也會記候選）。"}});
        } else {
          RemoteLLMProvider provider("llamacpp");
          const auto t0 = std::chrono::steady_clock::now();
          const bool loaded = provider.LoadModelDirect(spec, 0.0) && provider.IsAvailable();
          const auto load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - t0)
                                   .count();
          emit("rerank_tune", {{"state", "load"}, {"ok", loaded}, {"ms", (int)load_ms}});
          if (loaded) {
            // 取消時進行中的評分也中斷（推理行程會定期檢查）
            LLMCancelScope cancel_scope([&cancel] { return cancel->load(); });
            auto report = settings_rerank::Tune(
                &provider, cases, context_chars, length_alpha, personal_weight, *cancel,
                [emit](size_t done, size_t total) {
                  emit("rerank_tune",
                       {{"state", "score"}, {"done", done}, {"total", total}});
                });
            if (report.cancelled) {
              emit("rerank_tune", {{"state", "cancelled"}});
            } else {
              json grid = json::array();
              for (const auto& g : report.grid)
                grid.push_back({{"top", g.top},
                                {"margin", g.margin},
                                {"accuracy", g.accuracy},
                                {"wrong_change", g.wrong_change}});
              emit("rerank_tune",
                   {{"state", "done"},
                    {"cases", report.cases},
                    {"margin", report.best.margin},
                    {"top", report.best.top},
                    {"accuracy", report.best.accuracy},
                    {"wrong_change", report.best.wrong_change},
                    {"grid", grid}});
            }
          }
        }
      } catch (const std::exception& e) {
        emit("rerank_tune", {{"state", "error"}, {"message", e.what()}});
      }
      s.platform.SendPersonalCommand(10);  // 輸入法重新載入模型
      s.tune_running = false;
    });
    return {{"started", true}};
  }
  if (method == "rerank.cancel") {
    std::lock_guard<std::mutex> lock(s.job_mutex);
    if (s.tune_cancel)
      *s.tune_cancel = true;
    return nullptr;
  }

  // --- 整句重排自動微調：漸進一步＋防劣化回滾 -------------------------------
  // sync=true 時同步跑完回傳結果（/autotune 無頭模式用）；否則背景跑、結果走
  // rerank_auto 事件。判定與 history 在 settings_rerank::AutoTuneRun。
  if (method == "rerank.auto") {
    const bool sync = p.value("sync", false);
    const bool force = p.value("force", false);
    const fs::path personal = PersonalDir(s.platform);
    settings_rerank::AutoState st = settings_rerank::LoadAutoState(personal);
    const int64_t now_ms = NowMs();
    if (!st.enabled && !force)
      return json{{"action", "skip"}, {"reason", "自動微調未開啟"}};
    if (!force && now_ms < st.cooldown_until_ms)
      return json{{"action", "skip"}, {"reason", "回滾冷卻中"}};

    const json llm = s.LLMJson();
    const json choice = llm.value("choice", json::object());
    const size_t cur_top = (size_t)CfgInt(choice, "rerank_sentences", 10);
    const double cur_margin = CfgDbl(choice, "rerank_margin", 2.0);
    const size_t context_chars = (size_t)CfgInt(choice, "rerank_context", 30);
    const double alpha = CfgDbl(choice, "rerank_alpha", 0.0);
    const double personal_w = CfgDbl(choice, "rerank_personal", 0.0);

    // 不用模型的檢查先跑：上次套用的參數有沒有讓修正率變差
    std::vector<settings_rerank::LoggedMeta> meta;
    settings_rerank::LoadLoggedMeta(personal, &meta);
    const auto history = settings_rerank::LoadTuneHistory(personal);
    const settings_rerank::TuneEntry* last_apply = nullptr;
    for (auto it = history.rbegin(); it != history.rend() && !last_apply; ++it)
      if (it->action == "apply")
        last_apply = &*it;
    const auto check = last_apply ? settings_rerank::CheckDegradation(meta, last_apply->apply_rec_ms)
                                  : settings_rerank::DegradeCheck{};
    auto save_run = [&] {
      st.last_run_ms = now_ms;
      st.last_records = meta.size();
      settings_rerank::SaveAutoState(personal, st);
    };
    if (check.degraded) {
      settings_rerank::TuneEntry e;
      e.ts = now_ms;
      e.action = "rollback";
      e.top = last_apply->prev_top;
      e.margin = last_apply->prev_margin;
      e.prev_top = last_apply->top;
      e.prev_margin = last_apply->margin;
      e.apply_rec_ms = meta.empty() ? 0 : meta.back().time;
      char buf[160];
      std::snprintf(buf, sizeof(buf), "修正率 %.1f%% → %.1f%%（%zu 筆後段紀錄）",
                    check.before_rate * 100, check.after_rate * 100, check.after_n);
      e.note = buf;
      settings_rerank::AppendTuneHistory(personal, e);
      st.cooldown_until_ms = now_ms + 7LL * 86400000;  // 回滾後冷卻一週
      save_run();
      settings_rerank::AutoRunResult r;
      r.action = "rollback";
      r.top = e.top;
      r.margin = e.margin;
      r.check = check;
      r.records = meta.size();
      r.entry = e;
      r.message = "偵測到套用後修正率上升，回復到評 " + std::to_string(e.top) + " 句、門檻 " +
                  std::to_string(e.margin);
      return AutoResultJson(r);
    }
    // 學習式排序器的劣化看門狗：模型啟用後修正率明顯上升 → 停用（改名 .bak，
    // 之後的 rank.train 可用更多資料重訓，匯出仍要贏過現行設定）
    {
      const fs::path model_path = personal / "ranker_model.json";
      std::error_code mec;
      if (fs::exists(model_path, mec)) {
        int64_t exported_ts = 0;
        std::ifstream rin(personal / "rank_train.log");
        for (std::string line; std::getline(rin, line);) {
          try {
            const json j = json::parse(line);
            if (j.value("exported", false))
              exported_ts = j.value("ts", (int64_t)0);
          } catch (...) {
          }
        }
        const auto rcheck =
            exported_ts ? settings_rerank::CheckDegradation(meta, exported_ts)
                        : settings_rerank::DegradeCheck{};
        if (rcheck.degraded) {
          std::error_code rec;
          fs::rename(model_path, personal / "ranker_model.json.bak", rec);
          if (!rec) {
            settings_rerank::TuneEntry e;
            e.ts = now_ms;
            e.action = "rollback";
            e.apply_rec_ms = meta.empty() ? 0 : meta.back().time;
            char buf[160];
            std::snprintf(buf, sizeof(buf), "排序器停用：修正率 %.1f%% → %.1f%%（%zu 筆後段紀錄）",
                          rcheck.before_rate * 100, rcheck.after_rate * 100, rcheck.after_n);
            e.note = buf;
            settings_rerank::AppendTuneHistory(personal, e);
          }
        }
      }
    }
    if (!force) {
      if (meta.size() < st.last_records + 150)
        return json{{"action", "skip"}, {"reason", "新紀錄未滿 150 筆"}, {"records", meta.size()}};
      if (now_ms - st.last_run_ms < 3LL * 86400000)
        return json{{"action", "skip"}, {"reason", "距上次執行未滿三天"}, {"records", meta.size()}};
    }
    LLMLocalModelSpec spec;
    if (!ScorerSpec(llm, &spec))
      return json{{"action", "error"},
                  {"reason", "沒有可用的本機評分模型（評分、預測或校正都沒選本機模型）"}};

    // 暫停輸入法模型 → 載入評分模型 → AutoTuneRun → 恢復（sync 與背景共用）。
    // 全部用值捕捉：背景執行緒跑時 Call 已回傳，參照區域變數會懸垂
    const size_t records = meta.size();
    auto run = [&s, personal, spec, cur_top, cur_margin, context_chars, alpha, personal_w,
                records](Emit emit2, const std::atomic<bool>& cancel) {
      const auto ev = [&emit2](const json& e) {
        if (emit2)
          emit2("rerank_auto", e);
      };
      const fs::path paused = s.platform.UserDataDir() / "llm_paused.txt";
      std::error_code ec;
      fs::remove(paused, ec);
      if (s.platform.SendPersonalCommand(9)) {
        ev({{"state", "pause"}, {"message", "暫停輸入法的模型…"}});
        for (int i = 0; i < 100 && !cancel && !fs::exists(paused, ec); ++i)
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      settings_rerank::AutoRunResult r;
      try {
        RemoteLLMProvider provider("llamacpp");
        if (provider.LoadModelDirect(spec, 0.0) && provider.IsAvailable()) {
          LLMCancelScope cancel_scope([&cancel] { return cancel.load(); });
          r = settings_rerank::AutoTuneRun(
              &provider, personal, cur_top, cur_margin, context_chars, alpha, personal_w, cancel,
              [&ev](size_t done, size_t total) {
                ev({{"state", "score"}, {"done", done}, {"total", total}});
              });
        } else {
          r.action = "error";
          r.message = "無法載入評分模型";
        }
      } catch (const std::exception& e) {
        r.action = "error";
        r.message = e.what();
      }
      s.platform.SendPersonalCommand(10);  // 輸入法重新載入模型
      // 更新上次執行狀態（不動開關與冷卻期）
      auto st2 = settings_rerank::LoadAutoState(personal);
      st2.last_run_ms = NowMs();
      st2.last_records = records;
      settings_rerank::SaveAutoState(personal, st2);
      return r;
    };

    if (sync) {
      std::atomic<bool> cancel{false};
      return AutoResultJson(run(s.emit, cancel));
    }
    std::lock_guard<std::mutex> lock(s.job_mutex);
    if (s.tune_running)
      throw Error("調整正在進行中。");
    if (s.tune_job.joinable())
      s.tune_job.join();
    s.tune_cancel = std::make_shared<std::atomic<bool>>(false);
    s.tune_running = true;
    Emit emit = s.emit;
    auto cancel = s.tune_cancel;
    s.tune_job = std::thread([&s, emit, cancel, run]() {
      auto r = run(emit, *cancel);
      json out = AutoResultJson(r);
      out["state"] = "done";
      if (emit)
        emit("rerank_auto", out);
      s.tune_running = false;
    });
    return {{"started", true}};
  }
  // 直接寫入一組重排參數（排程/無頭模式的套用路徑；網頁走表單 settings.apply）
  if (method == "rerank.apply") {
    const size_t top = (size_t)p.value("top", 0);
    const double margin = p.value("margin", 0.0);
    if (top < 2 || !(margin > 0))
      throw Error("參數不完整（top ≥ 2、margin > 0）。");
    RimeConfig cfg = {0};
    s.api->settings_get_config(s.style, &cfg);
    s.rime->config_set_int(&cfg, "llm/choice/rerank_sentences", (int)top);
    s.rime->config_set_double(&cfg, "llm/choice/rerank_margin", margin);
    const bool ok = !!s.api->customize_item(s.style, "llm", &cfg);
    s.rime->config_close(&cfg);
    if (!ok)
      throw Error("LLM 設定儲存失敗。");
    s.api->save_settings(s.style);
    // 整句候選數不夠評這麼多句時補上（只增不減）
    int sentences = 0;
    for (const char* schema : {"bopomofo", "bopomofo_express", "bopomofo_tw"}) {
      const std::string text = [&] {
        std::ifstream in(s.platform.UserDataDir() / (std::string(schema) + ".custom.yaml"));
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      }();
      const auto pos = text.find("translator/max_sentences:");
      if (pos != std::string::npos)
        sentences = (std::max)(sentences, std::atoi(text.c_str() + pos + 25));
    }
    if ((size_t)sentences < top) {
      std::string error;
      if (!ApplySentenceCandidates(s.platform, (int)top, &error))
        throw Error("整句候選數補高失敗：" + error);
    }
    s.platform.Deploy();
    s.api->load_settings(s.style);
    return {{"deployed", true}, {"llm", s.LLMJson()}};
  }
  if (method == "rerank.history") {
    const auto history = settings_rerank::LoadTuneHistory(PersonalDir(s.platform));
    json entries = json::array();
    for (auto it = history.rbegin(); it != history.rend() && entries.size() < 20; ++it) {
      entries.push_back({{"ts", it->ts},
                         {"action", it->action},
                         {"top", it->top},
                         {"margin", it->margin},
                         {"prev_top", it->prev_top},
                         {"prev_margin", it->prev_margin},
                         {"cases", it->cases},
                         {"acc_prev", it->acc_prev},
                         {"acc_best", it->acc_best},
                         {"note", it->note}});
    }
    const auto st = settings_rerank::LoadAutoState(PersonalDir(s.platform));
    std::vector<settings_rerank::LoggedMeta> meta;
    settings_rerank::LoadLoggedMeta(PersonalDir(s.platform), &meta);
    return {{"entries", entries},
            {"auto",
             {{"enabled", st.enabled},
              {"last_run_ms", st.last_run_ms},
              {"last_records", st.last_records},
              {"cooldown_until_ms", st.cooldown_until_ms}}},
            {"records", meta.size()}};
  }
  // 上次自動套用的前一組參數（UI「回復」鈕）
  if (method == "rerank.revert") {
    const auto history = settings_rerank::LoadTuneHistory(PersonalDir(s.platform));
    for (auto it = history.rbegin(); it != history.rend(); ++it) {
      if (it->action == "apply" && it->prev_top >= 2 && it->prev_margin > 0)
        return {{"found", true},
                {"top", it->prev_top},
                {"margin", it->prev_margin},
                {"note", it->note}};
    }
    return {{"found", false}};
  }
  // 自動微調開關＋排程（Windows 工作排程器；每週跑 WeaselDeployer /autotune）
  if (method == "rerank.auto_cfg") {
    const fs::path personal = PersonalDir(s.platform);
    if (p.contains("enabled")) {
      auto st = settings_rerank::LoadAutoState(personal);
      st.enabled = p.value("enabled", false);
      settings_rerank::SaveAutoState(personal, st);
#ifdef _WIN32
      if (st.enabled) {
        wchar_t exe[MAX_PATH] = {0};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const std::wstring cmd = L"schtasks /Create /TN \"" + std::wstring(kAutoTuneTask) +
                                 L"\" /TR \"\\\"" + exe + L"\\\" /autotune\" /SC WEEKLY /IT /F";
        if (RunQuiet(cmd) != 0)
          throw Error("工作排程器註冊失敗（自動微調仍已開啟，但不會定期執行）。");
      } else {
        RunQuiet(L"schtasks /Delete /TN \"" + std::wstring(kAutoTuneTask) + L"\" /F");
      }
#endif
    }
    const auto st = settings_rerank::LoadAutoState(personal);
    bool scheduled = false;
#ifdef _WIN32
    scheduled = RunQuiet(L"schtasks /Query /TN \"" + std::wstring(kAutoTuneTask) + L"\"") == 0;
#endif
    return {{"enabled", st.enabled},
            {"scheduled", scheduled},
            {"last_run_ms", st.last_run_ms},
            {"last_records", st.last_records},
            {"cooldown_until_ms", st.cooldown_until_ms}};
  }

  // 學習式整句排序器：用選字紀錄訓線性/GBDT 小模型；驗證集贏過現行公式且
  // 錯改沒變多才匯出 personal/ranker_model.json。流程同 rerank.auto
  //（暫停輸入法模型 → 載入評分模型 → 訓練 → 恢復）；結果走 rank_train 事件
  if (method == "rank.train") {
    const fs::path personal = PersonalDir(s.platform);
    const json llm = s.LLMJson();
    const json choice = llm.value("choice", json::object());
    const size_t cur_top = (size_t)CfgInt(choice, "rerank_sentences", 10);
    const double cur_margin = CfgDbl(choice, "rerank_margin", 2.0);
    const size_t context_chars = (size_t)CfgInt(choice, "rerank_context", 30);
    const double alpha = CfgDbl(choice, "rerank_alpha", 0.0);
    const double personal_w = CfgDbl(choice, "rerank_personal", 0.0);
    LLMLocalModelSpec spec;
    if (!ScorerSpec(llm, &spec))
      return json{{"action", "error"},
                  {"message", "沒有可用的本機評分模型（評分、預測或校正都沒選本機模型）"}};
    const bool sync = p.value("sync", false);
    auto run = [&s, personal, spec, cur_top, cur_margin, context_chars, alpha, personal_w](
                   Emit emit2, const std::atomic<bool>& cancel) {
      const auto ev = [&emit2](const json& e) {
        if (emit2)
          emit2("rank_train", e);
      };
      const fs::path paused = s.platform.UserDataDir() / "llm_paused.txt";
      std::error_code ec;
      fs::remove(paused, ec);
      if (s.platform.SendPersonalCommand(9)) {
        ev({{"state", "pause"}, {"message", "暫停輸入法的模型…"}});
        for (int i = 0; i < 100 && !cancel && !fs::exists(paused, ec); ++i)
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      settings_rerank::RankTrainReport r;
      try {
        RemoteLLMProvider provider("llamacpp");
        if (provider.LoadModelDirect(spec, 0.0) && provider.IsAvailable()) {
          LLMCancelScope cancel_scope([&cancel] { return cancel.load(); });
          r = settings_rerank::TrainRanker(
              &provider, personal, context_chars, alpha, personal_w, cur_top, cur_margin,
              cancel, [&ev](size_t done, size_t total) {
                ev({{"state", "score"}, {"done", done}, {"total", total}});
              });
        } else {
          r.message = "無法載入評分模型";
        }
      } catch (const std::exception& e) {
        r.message = e.what();
      }
      s.platform.SendPersonalCommand(10);  // 輸入法重新載入模型
      return r;
    };
    const auto report_json = [](const settings_rerank::RankTrainReport& r) {
      return json{{"exported", r.exported}, {"type", r.type},
                  {"cases", r.cases},         {"usable", r.usable},
                  {"train", r.train_cases},   {"val", r.val_cases},
                  {"base_acc", r.base_acc},   {"base_wrong", r.base_wrong},
                  {"lin_acc", r.lin_acc},     {"gbdt_acc", r.gbdt_acc},
                  {"val_acc", r.val_acc},     {"val_wrong", r.val_wrong},
                  {"margin", r.margin},       {"cancelled", r.cancelled},
                  {"message", r.message}};
    };
    if (sync) {
      std::atomic<bool> cancel{false};
      return report_json(run(s.emit, cancel));
    }
    std::lock_guard<std::mutex> lock(s.job_mutex);
    if (s.tune_running)
      throw Error("調整正在進行中。");
    if (s.tune_job.joinable())
      s.tune_job.join();
    s.tune_cancel = std::make_shared<std::atomic<bool>>(false);
    s.tune_running = true;
    Emit emit = s.emit;
    auto cancel = s.tune_cancel;
    s.tune_job = std::thread([&s, emit, cancel, run, report_json]() {
      auto r = run(emit, *cancel);
      json out = report_json(r);
      out["state"] = "done";
      if (emit)
        emit("rank_train", out);
      s.tune_running = false;
    });
    return {{"started", true}};
  }
  // 排序器狀態：模型檔有沒有載得動＋最近訓練紀錄
  if (method == "rank.status") {
    const fs::path personal = PersonalDir(s.platform);
    ime::RankerModel m;
    const bool loaded = m.Load(personal / "ranker_model.json");
    json j{{"loaded", loaded}};
    if (loaded) {
      j["type"] = m.type();
      j["margin"] = m.margin();
      try {
        j["trained"] = json::parse(m.info());
      } catch (...) {
      }
    }
    json hist = json::array();
    std::ifstream in(personal / "rank_train.log");
    std::string line;
    std::vector<std::string> tail;
    while (std::getline(in, line)) {
      if (!line.empty()) {
        tail.push_back(line);
        if (tail.size() > 10)
          tail.erase(tail.begin());
      }
    }
    for (auto it = tail.rbegin(); it != tail.rend(); ++it) {
      try {
        hist.push_back(json::parse(*it));
      } catch (...) {
      }
    }
    j["history"] = hist;
    return j;
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
                      {"rerank", r.rerank},
                      {"detail", r.detail}});
    return rows;
  }
  if (method == "calibration.get") {
    // 推薦／校正的信心校準（輸入法寫的 weasel_calibration.txt）
    ime::CalibrationStore store(platform.UserDataDir());
    json lines = json::array();
    for (const auto kind : {ime::SuggestionKind::kRecommend, ime::SuggestionKind::kCorrection}) {
      const std::string text = store.Summary(kind);
      if (!text.empty())
        lines.push_back(text);
    }
    return lines;
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

  // --- 內建詞庫（台灣詞庫） --------------------------------------------------
  if (method == "lexicon.status") {
    return s.LexiconsJson(p.value("refresh", true));
  }
  if (method == "lexicon.search") {
    return s.SearchLexicon(p);
  }
  if (method == "lexicon.validateEdit") {
    const std::string id = p.value("id", std::string(taiwan_lexicon::kLexiconId));
    if (id != taiwan_lexicon::kLexiconId)
      throw Error("不支援的詞庫識別：「" + id + "」。");
    const taiwan_lexicon::BaseLexicon* base = nullptr;
    const taiwan_lexicon::SyllableTable* syllables = nullptr;
    std::string err;
    if (!s.EnsureBaseLexicon(&base, &syllables, &err))
      throw Error(err);

    const std::string raw_word = Str(p, "word");
    const std::string raw_reading = Str(p, "reading");
    const std::string pref = p.value("pref", "normal");
    if (!taiwan_lexicon::IsValidPref(pref))
      throw Error("無效的排序偏好設定：「" + pref + "」。");

    std::string norm_word, norm_code, norm_zhuyin;
    if (!taiwan_lexicon::ParseAndValidateReading(raw_word, raw_reading, *syllables, &norm_word,
                                                 &norm_code, &norm_zhuyin, &err)) {
      throw Error(err);
    }
    const auto bit = base->weights.find({norm_word, norm_code});
    const bool in_base = (bit != base->weights.end());
    const int base_w = in_base ? bit->second : 1000;
    const int eff_w = taiwan_lexicon::EffectiveWeight(in_base, base_w, pref);

    const std::string orig_word = Str(p, "orig_word");
    const std::string orig_code = Str(p, "orig_code");
    bool orig_in_base = false;
    int orig_base_w = 1000;
    if (!orig_word.empty() && !orig_code.empty()) {
      const auto oit = base->weights.find({orig_word, orig_code});
      if (oit != base->weights.end()) {
        orig_in_base = true;
        orig_base_w = oit->second;
      }
    }

    return {{"word", norm_word},
            {"code", norm_code},
            {"zhuyin", norm_zhuyin},
            {"pref", pref},
            {"in_base", in_base},
            {"base_weight", base_w},
            {"effective_weight", eff_w},
            {"orig_in_base", orig_in_base},
            {"orig_base_weight", orig_base_w}};
  }
  if (method == "lexicon.pickFile") {
    const std::string op = Str(p, "op");
    fs::path path;
    if (op == "export") {
      path = platform.SaveFileDialog(
          owner, "匯出台灣詞庫個人修改",
          {{"詞庫修改備份 (*.json)", "*.json"}, {"所有檔案 (*.*)", "*.*"}},
          "wisdom_taiwan_edits.json", "json");
    } else if (op == "import") {
      path = platform.OpenFileDialog(
          owner, "匯入台灣詞庫個人修改",
          {{"詞庫修改備份 (*.json)", "*.json"}, {"所有檔案 (*.*)", "*.*"}}, "", "json");
    } else if (op == "exportSources") {
      path = platform.SaveFileDialog(
          owner, "匯出台灣詞庫來源包",
          {{"ZIP 壓縮檔 (*.zip)", "*.zip"}, {"所有檔案 (*.*)", "*.*"}},
          "wisdom_taiwan.sources.zip", "zip");
    } else {
      throw Error("bad op");
    }
    return path.empty() ? json(nullptr) : json(U8(path));
  }
  if (method == "lexicon.exportEdits") {
    const std::string id = p.value("id", std::string(taiwan_lexicon::kLexiconId));
    if (id != taiwan_lexicon::kLexiconId)
      throw Error("不支援的詞庫識別：「" + id + "」。");
    const taiwan_lexicon::BaseLexicon* base = nullptr;
    const taiwan_lexicon::SyllableTable* syllables = nullptr;
    std::string err;
    if (!s.EnsureBaseLexicon(&base, &syllables, &err))
      throw Error(err);

    std::vector<taiwan_lexicon::UserEdit> raw_edits;
    for (const auto& item : p.value("edits", json::array())) {
      taiwan_lexicon::UserEdit e;
      if (taiwan_lexicon::EditFromJson(item, &e))
        raw_edits.push_back(std::move(e));
    }
    std::vector<taiwan_lexicon::UserEdit> validated;
    if (!taiwan_lexicon::ValidateEditsList(raw_edits, *syllables, &validated, &err))
      throw Error(err);

    const fs::path dest = Path(Str(p, "path"));
    if (dest.empty())
      throw Error("未指定匯出檔案路徑。");
    const std::string exported_text = taiwan_lexicon::ExportEditsJson(
        base->version, validated, FormatTime((int64_t)std::time(nullptr)));
    try {
      taiwan_lexicon::Write(dest, exported_text);
    } catch (const std::exception& ex) {
      throw Error(std::string("無法寫入匯出檔案：") + ex.what());
    }
    if (p.value("reveal", true))
      platform.Reveal(dest);
    return {{"exported", validated.size()}, {"path", U8(dest)}};
  }
  if (method == "lexicon.importPreview") {
    const std::string id = p.value("id", std::string(taiwan_lexicon::kLexiconId));
    if (id != taiwan_lexicon::kLexiconId)
      throw Error("不支援的詞庫識別：「" + id + "」。");
    const taiwan_lexicon::BaseLexicon* base = nullptr;
    const taiwan_lexicon::SyllableTable* syllables = nullptr;
    std::string err;
    if (!s.EnsureBaseLexicon(&base, &syllables, &err))
      throw Error(err);

    std::string content = Str(p, "content");
    if (content.empty()) {
      const fs::path src = Path(Str(p, "path"));
      if (src.empty())
        throw Error("未指定匯入檔案路徑。");
      std::error_code ec;
      if (!fs::exists(src, ec) || fs::is_directory(src, ec))
        throw Error("找不到指定的匯入檔案。");
      const auto fsize = fs::file_size(src, ec);
      if (!ec && fsize > taiwan_lexicon::kMaxImportBytes)
        throw Error("匯入檔案過大（上限為 2 MB）。");
      try {
        content = taiwan_lexicon::Read(src);
      } catch (const std::exception& ex) {
        throw Error(std::string("無法讀取匯入檔案：") + ex.what());
      }
    }

    std::vector<taiwan_lexicon::UserEdit> cur_edits;
    if (p.contains("current_edits") && p["current_edits"].is_array()) {
      for (const auto& item : p["current_edits"]) {
        taiwan_lexicon::UserEdit e;
        if (taiwan_lexicon::EditFromJson(item, &e))
          cur_edits.push_back(std::move(e));
      }
    } else {
      taiwan_lexicon::UserConfig saved_cfg;
      taiwan_lexicon::LoadUserConfig(s.rime, platform.UserDataDir(), &saved_cfg);
      cur_edits = std::move(saved_cfg.edits);
    }

    json preview;
    if (!taiwan_lexicon::PreviewImportEdits(content, *base, *syllables, cur_edits, &preview, &err))
      throw Error(err);
    return preview;
  }
  if (method == "lexicon.sourceArchive") {
    const std::string id = p.value("id", std::string(taiwan_lexicon::kLexiconId));
    if (id != taiwan_lexicon::kLexiconId)
      throw Error("不支援的詞庫識別：「" + id + "」。");
    const std::string op = Str(p, "op");
    std::error_code ec;
    fs::path zip_path = s.SharedLexiconDir() / "wisdom_taiwan.sources.zip";
    if (!fs::exists(zip_path, ec)) {
      const fs::path user_zip = platform.UserDataDir() / "wisdom_taiwan.sources.zip";
      if (fs::exists(user_zip, ec))
        zip_path = user_zip;
      else
        throw Error("找不到隨附來源包 wisdom_taiwan.sources.zip。");
    }
    if (op == "reveal") {
      platform.Reveal(zip_path);
      return {{"ok", true}, {"path", U8(zip_path)}};
    }
    if (op == "export") {
      const fs::path dest = Path(Str(p, "dest"));
      if (dest.empty())
        throw Error("未指定匯出路徑。");
      fs::copy_file(zip_path, dest, fs::copy_options::overwrite_existing, ec);
      if (ec)
        throw Error("無法匯出來源包檔案：" + ec.message());
      if (p.value("reveal", true))
        platform.Reveal(dest);
      return {{"ok", true}, {"path", U8(dest)}};
    }
    throw Error("bad op");
  }

  throw Error("unknown method: " + method);
}

}  // namespace settings
