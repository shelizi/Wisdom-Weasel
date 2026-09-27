#include "ops.h"

#include <PersonalCrypto.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <thread>

#include "../net/http.h"
#include "../third_party/nlohmann/json.hpp"

namespace settings {

using json = nlohmann::json;

const char kDefaultApiUrl[] = "https://api.openai.com/v1/chat/completions";
const char kGrammarUrl[] =
    "https://raw.githubusercontent.com/lotem/rime-octagram-data/hant/zh-hant-t-essay-bgw.gram";

namespace {

std::tm LocalTime(std::time_t t) {
  std::tm tm = {};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  return tm;
}

std::string ReadFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::vector<std::string> Split(const std::string& s, char delim) {
  std::vector<std::string> parts;
  size_t start = 0, pos;
  while ((pos = s.find(delim, start)) != std::string::npos) {
    parts.push_back(s.substr(start, pos - start));
    start = pos + 1;
  }
  parts.push_back(s.substr(start));
  return parts;
}

}  // namespace

// ---------------------------------------------------------------------------
// 文字與路徑

fs::path Path(const std::string& utf8) {
  return fs::u8path(utf8);
}

std::string U8(const fs::path& path) {
  return path.u8string();
}

std::string ToLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return (char)(c < 0x80 ? std::tolower(c) : c); });
  return s;
}

std::string FileNameOf(const std::string& path) {
  return U8(Path(path).filename());
}

std::string Trim(const std::string& s) {
  // 半形空白、Tab、換行與全形空白（U+3000 = E3 80 80）
  size_t b = 0, e = s.size();
  auto space_at = [&](size_t i, bool forward) -> size_t {
    const unsigned char c = (unsigned char)s[i];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
      return 1;
    if (forward ? (i + 3 <= s.size() && s.compare(i, 3, "\xE3\x80\x80") == 0)
                : (i >= 2 && s.compare(i - 2, 3, "\xE3\x80\x80") == 0))
      return 3;
    return 0;
  };
  while (b < e) {
    const size_t n = space_at(b, true);
    if (!n)
      break;
    b += n;
  }
  while (e > b) {
    const size_t n = space_at(e - 1, false);
    if (!n)
      break;
    e -= n;
  }
  return s.substr(b, e - b);
}

std::string FormatSize(uint64_t bytes) {
  char buf[32];
  if (bytes >= (1ULL << 30))
    std::snprintf(buf, sizeof(buf), "%.2f GB", bytes / 1073741824.0);
  else
    std::snprintf(buf, sizeof(buf), "%.0f MB", bytes / 1048576.0);
  return buf;
}

std::string FormatTime(int64_t t) {
  if (t <= 0)
    return "—";
  const std::tm tm = LocalTime((std::time_t)t);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d/%02d/%02d %02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1,
                tm.tm_mday, tm.tm_hour, tm.tm_min);
  return buf;
}

std::string UrlDecode(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size() && std::isxdigit((unsigned char)s[i + 1]) &&
        std::isxdigit((unsigned char)s[i + 2])) {
      out += (char)std::stoi(s.substr(i + 1, 2), nullptr, 16);
      i += 2;
    } else {
      out += s[i];
    }
  }
  return out;
}

std::string LoadedModelDisplay(const std::string& model) {
  if (model.empty())
    return model;
  if (ToLower(U8(Path(model).extension())) == ".gguf")
    return FileNameOf(model);
  return model;
}

const char* GuessModelType(const std::string& path) {
  const std::string name = ToLower(FileNameOf(path));
  if (name.find("base") != std::string::npos)
    return "Base";
  if (name.find("instruct") != std::string::npos || name.find("chat") != std::string::npos ||
      name.find("-it") != std::string::npos)
    return "Instruct";
  return nullptr;
}

// ---------------------------------------------------------------------------
// 模型檔

bool IsGguf(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  char magic[4] = {0};
  return in.read(magic, 4) && std::memcmp(magic, "GGUF", 4) == 0;
}

namespace {
bool IsGgufName(const fs::path& p) {
  return ToLower(U8(p.extension())) == ".gguf";
}
}  // namespace

std::vector<std::string> ScanModels(Platform& platform, const std::string& current) {
  std::vector<fs::path> dirs;
  if (!current.empty())
    dirs.push_back(Path(current).parent_path());
  dirs.push_back(platform.ModelsDir());
  std::vector<std::string> found;
  std::set<std::string> seen;
  for (const auto& dir : dirs) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
      if (entry.is_regular_file(ec) && IsGgufName(entry.path()) &&
          seen.insert(ToLower(U8(entry.path()))).second)
        found.push_back(U8(entry.path()));
    }
  }
  std::sort(found.begin(), found.end(), [](const std::string& a, const std::string& b) {
    return ToLower(FileNameOf(a)) < ToLower(FileNameOf(b));
  });
  return found;
}

std::vector<ModelFile> ListModelFiles(Platform& platform) {
  std::vector<ModelFile> files;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(platform.ModelsDir(), ec)) {
    if (entry.is_regular_file(ec) && IsGgufName(entry.path()))
      files.push_back({U8(entry.path()), (uint64_t)fs::file_size(entry.path(), ec)});
  }
  std::sort(files.begin(), files.end(), [](const ModelFile& a, const ModelFile& b) {
    return ToLower(FileNameOf(a.path)) < ToLower(FileNameOf(b.path));
  });
  return files;
}

std::string NormalizeModelUrl(std::string url, std::string* file_name) {
  url = Trim(url);
  const size_t blob = url.find("/blob/");
  if (url.find("huggingface.co/") != std::string::npos && blob != std::string::npos)
    url.replace(blob, 6, "/resolve/");
  const std::string path = url.substr(0, url.find_first_of("?#"));
  // 百分比編碼的檔名（例如 %2B）
  *file_name = UrlDecode(path.substr(path.find_last_of('/') + 1));
  return url;
}

bool CopyFileWithProgress(const fs::path& src, const fs::path& dest, const Progress& progress,
                          std::string* error) {
  std::ifstream in(src, std::ios::binary);
  std::ofstream out(dest, std::ios::binary | std::ios::trunc);
  if (!in || !out) {
    *error = !in ? "無法讀取來源檔" : "無法寫入檔案";
    return false;
  }
  std::error_code ec;
  const uint64_t total = fs::file_size(src, ec);
  std::unique_ptr<char[]> buf(new char[1 << 20]);
  uint64_t done = 0;
  while (in) {
    in.read(buf.get(), 1 << 20);
    const std::streamsize n = in.gcount();
    if (n <= 0)
      break;
    out.write(buf.get(), n);
    if (!out) {
      *error = "複製失敗（磁碟空間不足？）";
      return false;
    }
    done += (uint64_t)n;
    if (!progress(done, total)) {
      *error = "已取消";
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// API 連線測試

namespace {

// chat/completions 網址 → models 網址
std::string ModelsUrl(const std::string& url) {
  const std::string tail = "/chat/completions";
  const size_t p = url.rfind(tail);
  return p == std::string::npos ? std::string() : url.substr(0, p) + "/models";
}

std::string ListModels(const std::string& url, const std::string& key) {
  const std::string models_url = ModelsUrl(url);
  if (models_url.empty())
    return "";
  net::Request request;
  request.url = models_url;
  if (!key.empty())
    request.headers.push_back({"Authorization", "Bearer " + key});
  net::Response response;
  std::string error;
  if (!net::Fetch(request, &response, &error) || response.status != 200)
    return "";
  const json j = json::parse(response.body, nullptr, false);
  std::vector<std::string> ids;
  if (j.is_object() && j.contains("data") && j["data"].is_array()) {
    for (const auto& m : j["data"]) {
      if (m.is_object() && m.contains("id") && m["id"].is_string())
        ids.push_back(m["id"].get<std::string>());
    }
  }
  if (ids.empty())
    return "";
  std::string names;
  for (size_t i = 0; i < ids.size() && i < 8; ++i)
    names += (i ? "、" : "") + ids[i];
  return "可用的模型：" + names + (ids.size() > 8 ? "…（共 " + std::to_string(ids.size()) + " 個）" : "");
}

// UTF-8 字串截到 n 個字（碼點）
std::string Clip(const std::string& s, size_t n) {
  size_t count = 0, i = 0;
  for (; i < s.size() && count < n; ++count) {
    const unsigned char c = (unsigned char)s[i];
    i += c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
  }
  return i >= s.size() ? s : s.substr(0, i) + "…";
}

}  // namespace

std::string TestApi(const std::string& api_url, const std::string& api_key, const std::string& model) {
  net::Request request;
  request.method = "POST";
  request.url = api_url;
  request.headers.push_back({"Content-Type", "application/json"});
  if (!api_key.empty())
    request.headers.push_back({"Authorization", "Bearer " + api_key});
  request.body = json{{"model", model},
                      {"messages", json::array({{{"role", "user"}, {"content", "請只回覆「OK」兩個字。"}}})},
                      {"max_tokens", 64},
                      {"temperature", 0},
                      {"stream", false}}
                     .dump();
  request.receive_timeout_ms = 60000;
  const auto t0 = std::chrono::steady_clock::now();
  net::Response response;
  std::string error;
  if (!net::Fetch(request, &response, &error))
    return "✗ " + error;
  const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - t0)
                           .count();
  const json j = json::parse(response.body, nullptr, false);
  std::string server_message;
  if (j.is_object() && j.contains("error")) {
    const json& e = j["error"];
    if (e.is_object() && e.contains("message") && e["message"].is_string())
      server_message = Clip(e["message"].get<std::string>(), 120);
    else if (e.is_string())
      server_message = Clip(e.get<std::string>(), 120);
  }
  if (response.status == 200) {
    const json* content = nullptr;
    if (j.is_object() && j.contains("choices") && j["choices"].is_array() && !j["choices"].empty()) {
      const json& c = j["choices"][0];
      if (c.contains("message") && c["message"].contains("content"))
        content = &c["message"]["content"];
    }
    if (!content)
      return "✗ 連線成功，但回應不是 OpenAI 相容格式（網址是否指向 /v1/chat/completions？）";
    const std::string text = content->is_string() ? Clip(Trim(content->get<std::string>()), 40) : "";
    return "✓ 連線成功（" + std::to_string(ms) + " ms）：" +
           (text.empty() ? "回覆是空的（推理模型可能需要較多 token）" : "模型回覆「" + text + "」");
  }
  std::string reason;
  switch (response.status) {
    case 401:
    case 403: reason = "金鑰錯誤或沒有權限"; break;
    case 404: reason = "網址或模型名稱不正確"; break;
    case 400: reason = "請求被拒絕（常見原因：模型名稱不正確）"; break;
    case 429: reason = "超過使用頻率或額度"; break;
    default: reason = response.status >= 500 ? "伺服器錯誤" : "失敗";
  }
  std::string text = "✗ HTTP " + std::to_string(response.status) + " " + reason;
  if (!server_message.empty())
    text += "：" + server_message;
  if (response.status == 400 || response.status == 404 || model.empty()) {
    const std::string models = ListModels(api_url, api_key);
    if (!models.empty())
      text += "\n" + models;
  }
  return text;
}

// ---------------------------------------------------------------------------
// 注音方案的 custom.yaml

namespace {

const char* const kZhuyinSchemas[] = {"bopomofo", "bopomofo_express", "bopomofo_tw"};
const char kBoostBegin[] = "  # >>> weasel-personal-dict";
const char kBoostEnd[] = "  # <<< weasel-personal-dict";
const char kTypoBegin[] = "  # >>> weasel-typo-correction";
const char kTypoEnd[] = "  # <<< weasel-typo-correction";
const char kGrammarBegin[] = "  # >>> weasel-grammar";
const char kGrammarEnd[] = "  # <<< weasel-grammar";
const char kGrammarFile[] = "zh-hant-t-essay-bgw.gram";
const uint64_t kGrammarMinBytes = 30ull * 1024 * 1024;  // 完整的檔案約 41 MB

// 改寫一個方案的 custom.yaml：拿掉 begin ~ end 標記的區塊，block 非空時再加在 patch: 下面。
// 使用者自己設定過 conflict_keys 其中一項時不修改，回傳 false
bool PatchSchemaBlock(const fs::path& file, const char* begin, const char* end,
                      const std::vector<std::string>& block,
                      const std::vector<std::string>& conflict_keys, std::string* error) {
  const bool enable = !block.empty();
  std::string text = ReadFile(file);
  if (text.empty() && !enable)
    return true;
  if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0)
    text.erase(0, 3);
  // 拆行並拿掉我們之前加的區塊
  std::vector<std::string> lines;
  {
    std::istringstream in(text);
    bool inside = false;
    for (std::string line; std::getline(in, line);) {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      if (line.rfind(begin, 0) == 0) {
        inside = true;
        continue;
      }
      if (inside) {
        if (line.rfind(end, 0) == 0)
          inside = false;
        continue;
      }
      lines.push_back(line);
    }
  }
  const std::string name = U8(file.filename());
  if (enable) {
    for (const auto& line : lines) {
      for (const auto& key : conflict_keys) {
        if (line.find(key) != std::string::npos) {
          *error = name + " 已自行設定 " + key + "，沒有修改";
          return false;
        }
      }
    }
    auto patch = std::find_if(lines.begin(), lines.end(),
                              [](const std::string& l) { return l.rfind("patch:", 0) == 0; });
    if (patch != lines.end() && patch->find_first_not_of(" \t", 6) != std::string::npos &&
        (*patch)[patch->find_first_not_of(" \t", 6)] != '#') {
      *error = name + " 的 patch 格式無法自動修改";
      return false;
    }
    if (patch == lines.end()) {
      lines.push_back("patch:");
      patch = lines.end() - 1;
    }
    lines.insert(patch + 1, block.begin(), block.end());
  }
  std::string result;
  for (const auto& line : lines)
    result += line + "\n";
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  if (!out) {
    *error = "無法寫入 " + name;
    return false;
  }
  out << result;
  return true;
}

// 注音排序：改用 terra_pinyin.personal 詞典
bool PatchSchemaDictionary(const fs::path& file, bool enable, std::string* error) {
  std::vector<std::string> block;
  if (enable)
    block = {
        std::string(kBoostBegin) + "：個人詞庫的常用詞影響選字排序（小狼毫設定自動管理）",
        "  translator/dictionary: terra_pinyin.personal",
        "  translator/user_dict: terra_pinyin",
        kBoostEnd,
    };
  return PatchSchemaBlock(file, kBoostBegin, kBoostEnd, block,
                          {"translator/dictionary", "translator/user_dict"}, error);
}

// 注音容錯：打開 Rime 的拼寫糾錯（依鍵盤鄰鍵與編輯距離找相近的音節）
bool PatchSchemaCorrection(const fs::path& file, bool enable, std::string* error) {
  std::vector<std::string> block;
  if (enable)
    block = {
        std::string(kTypoBegin) + "：打錯注音時找相近的音節（小狼毫設定自動管理）",
        "  translator/enable_correction: true",
        kTypoEnd,
    };
  return PatchSchemaBlock(file, kTypoBegin, kTypoEnd, block, {"translator/enable_correction"}, error);
}

// 在方案加上 octagram 語言模型（設定同 rime-octagram-data 的 grammar:/hant）
bool PatchSchemaGrammar(const fs::path& file, bool enable, std::string* error) {
  std::vector<std::string> block;
  if (enable)
    block = {
        std::string(kGrammarBegin) + "：語言模型改善整句選字（小狼毫設定自動管理）",
        "  grammar:",
        "    language: zh-hant-t-essay-bgw",
        "  translator/contextual_suggestions: true",
        "  translator/max_homophones: 7",
        "  translator/max_homographs: 7",
        kGrammarEnd,
    };
  return PatchSchemaBlock(file, kGrammarBegin, kGrammarEnd, block,
                          {"grammar:", "translator/contextual_suggestions"}, error);
}

// 三個注音方案都改；關閉時只還原已有的檔案
bool PatchAllZhuyin(Platform& platform, bool enable, std::string* error,
                    bool (*patch)(const fs::path&, bool, std::string*)) {
  const fs::path user_dir = platform.UserDataDir();
  bool ok = true;
  for (const char* schema : kZhuyinSchemas) {
    const fs::path file = user_dir / (std::string(schema) + ".custom.yaml");
    std::error_code ec;
    if (!enable && !fs::exists(file, ec))
      continue;
    if (!patch(file, enable, error))
      ok = false;
  }
  return ok;
}

}  // namespace

bool ApplyRimeBoost(Platform& platform, RimeLeversApi* api, RimeSwitcherSettings* switcher,
                    bool enable, std::string* error) {
  const fs::path user_dir = platform.UserDataDir();
  const fs::path dict = user_dir / "terra_pinyin.personal.dict.yaml";
  // 目前選用的方案
  std::set<std::string> selected;
  RimeSchemaList list = {0};
  if (api->get_selected_schema_list(switcher, &list)) {
    for (size_t i = 0; i < list.size; ++i)
      selected.insert(list.list[i].schema_id);
    api->schema_list_destroy(&list);
  }
  if (enable) {
    // 先準備詞典檔：請輸入法產生；輸入法沒回應時放一份空的，確保方案編譯得過
    std::error_code ec;
    fs::remove(dict, ec);
    platform.SendPersonalCommand(7);
    if (!fs::exists(dict, ec)) {
      std::ofstream f(dict, std::ios::binary | std::ios::trunc);
      f << "# 小狼毫個人詞庫：你常打的詞（由輸入法自動產生，請勿手動修改）\n"
           "---\nname: terra_pinyin.personal\nversion: \"1\"\nsort: by_weight\n"
           "use_preset_vocabulary: true\nmax_phrase_length: 7\nmin_phrase_weight: 100\n"
           "import_tables:\n  - terra_pinyin\ncolumns:\n  - text\n  - weight\n...\n";
    }
  }
  bool ok = true;
  for (const char* schema : kZhuyinSchemas) {
    const fs::path file = user_dir / (std::string(schema) + ".custom.yaml");
    std::error_code ec;
    if (enable && !selected.count(schema)) {
      PatchSchemaDictionary(file, false, error);  // 沒選用的方案不改（之前改過的還原）
      continue;
    }
    if (!enable && !fs::exists(file, ec))
      continue;
    if (!PatchSchemaDictionary(file, enable, error))
      ok = false;
  }
  if (!enable) {
    std::error_code ec;
    fs::remove(dict, ec);
  }
  return ok;
}

bool ApplyTypoCorrection(Platform& platform, bool enable, std::string* error) {
  // 三個注音方案都改（沒選用的也改，之後改選時不必再套用一次）
  return PatchAllZhuyin(platform, enable, error, PatchSchemaCorrection);
}

bool ApplyGrammar(Platform& platform, bool enable, std::string* error) {
  return PatchAllZhuyin(platform, enable, error, PatchSchemaGrammar);
}

bool GrammarEnabled(Platform& platform) {
  return ReadFile(platform.UserDataDir() / "bopomofo_express.custom.yaml").find("# >>> weasel-grammar") !=
         std::string::npos;
}

fs::path GrammarPath(Platform& platform) {
  return platform.UserDataDir() / kGrammarFile;
}

bool GrammarReady(Platform& platform) {
  std::error_code ec;
  return fs::file_size(GrammarPath(platform), ec) >= kGrammarMinBytes && !ec;
}

// ---------------------------------------------------------------------------
// 選字統計

std::vector<StatsRow> ChoiceStats(Platform& platform, int span_days) {
  // weasel_stats.txt（輸入法寫的）：日期 組合代碼 送出 字數 換字 LLM出現 LLM採用 校正採用 Backspace
  //   送出後刪除 逐字選字 推薦出現 推薦套用；舊格式沒有組合代碼
  // weasel_stats_profiles.txt：組合代碼 版本 編譯時間 版本說明 設定 第一次出現
  struct Sum {
    int64_t commits = 0, chars = 0, changed = 0, offered = 0, used = 0, corrections = 0, backs = 0,
            deleted = 0, focus = 0, rec_offered = 0, rec_used = 0;
    void Add(const Sum& s) {
      commits += s.commits;
      chars += s.chars;
      changed += s.changed;
      offered += s.offered;
      used += s.used;
      corrections += s.corrections;
      backs += s.backs;
      deleted += s.deleted;
      focus += s.focus;
      rec_offered += s.rec_offered;
      rec_used += s.rec_used;
    }
  };
  struct Profile {
    std::string version, time, subject, settings;
  };
  const fs::path user_dir = platform.UserDataDir();
  std::map<std::string, Profile> profiles;
  {
    std::ifstream in(user_dir / "weasel_stats_profiles.txt", std::ios::binary);
    for (std::string line; std::getline(in, line);) {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      std::vector<std::string> f = Split(line, '\t');
      const std::string key = f[0];
      f.erase(f.begin());
      f.resize(5);
      if (!key.empty())
        profiles[key] = {f[0], f[1], f[2], f[3]};
    }
  }
  // 期間的起始日（本機時間，n 天前）
  auto date_before = [](int n) {
    const std::tm tm = LocalTime(std::time(nullptr) - (std::time_t)n * 24 * 3600);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return std::string(buf);
  };
  const std::string from = span_days > 0 ? date_before(span_days - 1) : std::string();

  std::map<std::string, Sum> sums;          // 組合代碼 → 期間內合計
  std::map<std::string, std::string> last;  // 組合代碼 → 最後使用日
  {
    std::ifstream in(user_dir / "weasel_stats.txt", std::ios::binary);
    for (std::string line; std::getline(in, line);) {
      std::istringstream f(line);
      std::string date, key;
      if (!(f >> date >> key) || date < from)
        continue;
      std::istringstream numbers;
      if (key.find_first_not_of("0123456789") == std::string::npos) {
        numbers.str(line.substr(line.find(date) + date.size()));
        key = "legacy";
      } else {
        numbers.str(line.substr(line.find(key) + key.size()));
      }
      Sum s;
      if (numbers >> s.commits >> s.chars >> s.changed >> s.offered >> s.used >> s.corrections >> s.backs) {
        numbers >> s.deleted >> s.focus >> s.rec_offered >> s.rec_used;  // 較新的欄位
        sums[key].Add(s);
        last[key] = (std::max)(last[key], date);
      }
    }
  }

  auto percent = [](int64_t n, int64_t d) {
    if (!d)
      return std::string("—");
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f%%", 100.0 * n / d);
    return std::string(buf);
  };
  auto ratio = [](int64_t n, int64_t d) {
    return d ? std::to_string(n) + "／" + std::to_string(d) : std::string("—");
  };
  auto counts = [](const Sum& t) {
    std::ostringstream out;
    out << t.chars << " 字、逐字選字 " << t.focus << "、推薦出現 " << t.rec_offered << "、LLM 出現 "
        << t.offered << "（校正採用 " << t.corrections << "）、Backspace " << t.backs << "、送出後刪除 "
        << t.deleted << " 次";
    return out.str();
  };

  // 最近用過的組合排前面
  std::vector<std::string> keys;
  for (const auto& [key, s] : sums)
    keys.push_back(key);
  std::sort(keys.begin(), keys.end(), [&](const std::string& a, const std::string& b) {
    return last[a] != last[b] ? last[a] > last[b] : a < b;
  });

  std::vector<StatsRow> rows;
  auto add_row = [&](const std::string& version, const std::string& settings_text, const Sum& t,
                     const std::string& detail) {
    StatsRow row;
    row.version = version;
    row.settings = settings_text;
    row.commits = t.commits;
    row.first_ok = percent(t.commits - t.changed, t.commits);
    row.deleted = percent(t.deleted, t.chars);
    row.changed = t.changed;
    row.recommended = ratio(t.rec_used, t.rec_offered);
    row.llm = ratio(t.used, t.offered);
    row.detail = detail;
    rows.push_back(std::move(row));
  };
  if (keys.size() > 1) {
    Sum total;
    for (const auto& [key, s] : sums)
      total.Add(s);
    add_row("（合計）", "所有組合", total, "所有版本與設定組合的合計\n" + counts(total));
  }
  for (const auto& key : keys) {
    const Sum& t = sums[key];
    std::string version, settings_text, detail;
    if (key == "legacy") {
      version = "舊資料";
      settings_text = "（未記錄）";
      detail = "分版本統計之前的紀錄，沒有版本與設定資訊\n";
    } else {
      auto p = profiles.find(key);
      const Profile info = p != profiles.end() ? p->second : Profile{"?", "", "", "?"};
      version = info.version;
      // 列表只放短的：方案名稱留給詳細資訊
      settings_text = info.settings;
      const size_t bar = settings_text.find("｜");
      if (bar != std::string::npos)
        settings_text = settings_text.substr(bar + std::strlen("｜"));
      detail = "版本 " + info.version;
      if (!info.version.empty() && info.version.back() == '*')
        detail += "（含未提交的修改）";
      if (!info.time.empty())
        detail += "，" + info.time + " 編譯";
      if (!info.subject.empty())
        detail += "：" + info.subject;
      detail += "\n設定：" + info.settings + "\n";
    }
    add_row(version, settings_text, t, detail + counts(t));
  }
  return rows;
}

void ResetChoiceStats(Platform& platform) {
  if (!platform.SendPersonalCommand(8)) {
    std::error_code ec;
    fs::remove(platform.UserDataDir() / "weasel_stats.txt", ec);
    fs::remove(platform.UserDataDir() / "weasel_stats_profiles.txt", ec);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(200));  // 等輸入法刪檔
}

ChoiceLogInfo ChoiceLogStatus(Platform& platform) {
  const fs::path file = PersonalDir(platform) / "choice_log.dat";
  ChoiceLogInfo info;
  std::error_code ec;
  info.bytes = fs::file_size(file, ec);
  if (ec) {
    info.bytes = 0;
    return info;
  }
  std::ifstream in(file, std::ios::binary);
  uint32_t len = 0;
  while (in.read((char*)&len, sizeof(len)) && in.seekg(len, std::ios::cur))
    ++info.records;
  return info;
}

bool ClearChoiceLog(Platform& platform) {
  std::error_code ec;
  fs::remove(PersonalDir(platform) / "choice_log.dat", ec);
  return !ec;
}

// ---------------------------------------------------------------------------
// 個人詞庫

fs::path PersonalDir(Platform& platform) {
  return platform.UserDataDir() / "personal";
}

std::string PersonalStatusText(Platform& platform, bool* running) {
  std::map<std::string, std::string> v;
  {
    std::ifstream in(PersonalDir(platform) / "status.txt", std::ios::binary);
    for (std::string line; std::getline(in, line);) {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      const size_t eq = line.find('=');
      if (eq != std::string::npos)
        v[line.substr(0, eq)] = line.substr(eq + 1);
    }
  }
  auto num = [&](const char* key) { return std::strtoll(v[key].c_str(), nullptr, 10); };
  std::ostringstream text;
  *running = false;
  if (v.empty()) {
    text << "無法取得狀態：請確認小狼毫正在執行。";
  } else if (v.count("disabled")) {
    text << "個人詞庫目前關閉。勾選上方的「啟用個人詞庫」並按「套用」即可開始學習。";
  } else {
    *running = v["running"] == "1";
    text << "詞庫：" << num("words") << " 個詞、" << num("pairs") << " 組接續\n"
         << "原始紀錄：累積中 " << num("active_records") << " 筆；已封存 " << num("archived_files")
         << " 批、共 " << num("archived_records") << " 筆\n";
    const int64_t last_refine = num("last_refine");
    const double interval = std::atof(v["interval_days"].c_str());
    text << "上次精煉：" << FormatTime(last_refine);
    if (interval > 0 && last_refine > 0)
      text << "　下次：" << FormatTime(last_refine + (int64_t)(interval * 86400)) << " 之後的閒置時間";
    else
      text << "　（自動精煉已關閉，只手動）";
    if (v["rime_boost"] == "1")
      text << "\n注音排序：已加入 " << num("rime_words") << " 個常打的詞"
           << (num("rime_updated") > 0 ? "（更新於 " + FormatTime(num("rime_updated")) + "）" : "");
    const std::string method = v["method"];
    text << "\n精煉方式：" << (method.empty() ? "只做統計整理（未選擇精煉模型）" : method) << "\n";
    if (*running)
      text << "精煉中…" << (v["progress"].empty() ? "" : "（" + v["progress"] + "）");
    else if (!v["last_result"].empty())
      text << "上次結果：" << v["last_result"];
  }
  return text.str();
}

PersonalWords LoadPersonalWords(Platform& platform) {
  PersonalWords result;
  const fs::path file = PersonalDir(platform) / "export.dat";
  std::error_code ec;
  fs::remove(file, ec);
  std::string plain;
  if (!platform.SendPersonalCommand(5)) {
    result.error = "無法連線到輸入法服務。";
  } else if (!personal_crypto::ReadProtected(file, &plain)) {
    // 個人詞庫關閉時輸入法不匯出
  } else {
    result.available = true;
    std::istringstream lines(plain);
    for (std::string line; std::getline(lines, line);) {
      const std::vector<std::string> f = Split(line, '\t');
      if (f.size() == 3 && f[0] == "W")
        result.words.emplace_back(f[1], std::atof(f[2].c_str()));
      else if (f.size() == 2 && f[0] == "A")
        result.rules.push_back({WordRule::kAdd, f[1], ""});
      else if (f.size() == 2 && f[0] == "R")
        result.rules.push_back({WordRule::kBlock, f[1], ""});
      else if (f.size() == 3 && f[0] == "M")
        result.rules.push_back({WordRule::kMerge, f[1], f[2]});
    }
  }
  fs::remove(file, ec);
  return result;
}

bool SendWordEdits(Platform& platform, const std::vector<std::string>& lines, std::string* error) {
  std::string plain = "WWPE1\n";
  for (const auto& line : lines)
    plain += line + "\n";
  if (!personal_crypto::WriteProtected(PersonalDir(platform) / "edit.dat", plain)) {
    *error = "無法寫入修改檔。";
    return false;
  }
  if (!platform.SendPersonalCommand(6)) {
    *error = "無法連線到輸入法服務。";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// 預測測試

bool SendLLMTestRequest(Platform& platform, unsigned id, const std::string& context) {
  const fs::path dir = platform.UserDataDir();
  std::error_code ec;
  fs::remove(dir / "llm_test_response.txt", ec);
  {
    std::ofstream out(dir / "llm_test_request.txt", std::ios::binary | std::ios::trunc);
    out << id << "\n" << context;
  }
  return platform.NotifyLLMTest();
}

std::optional<LLMTestResponse> PollLLMTestResponse(Platform& platform, unsigned id) {
  const fs::path file = platform.UserDataDir() / "llm_test_response.txt";
  std::error_code ec;
  if (!fs::exists(file, ec))
    return std::nullopt;
  std::string response_id;
  LLMTestResponse r;
  std::istringstream lines(ReadFile(file));
  for (std::string line; std::getline(lines, line);) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    const size_t eq = line.find('=');
    if (eq == std::string::npos)
      continue;
    const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
    if (key == "id") response_id = value;
    else if (key == "status") r.status = value;
    else if (key == "model") r.model = value;
    else if (key == "ms") r.ms = value;
    else if (key == "cand") r.candidates.push_back(value);
  }
  if (response_id != std::to_string(id))
    return std::nullopt;  // 還沒輪到這次的回覆
  return r;
}

// ---------------------------------------------------------------------------
// 使用者詞典

std::vector<std::string> ListUserDicts(RimeLeversApi* api) {
  std::vector<std::string> dicts;
  RimeUserDictIterator iter = {0};
  api->user_dict_iterator_init(&iter);
  while (const char* dict = api->next_user_dict(&iter))
    dicts.push_back(dict);
  api->user_dict_iterator_destroy(&iter);
  return dicts;
}

void PrepareUserDictTask() {
  static bool ready = false;
  RimeApi* rime = rime_get_api();
  if (!ready && RIME_API_AVAILABLE(rime, run_task)) {
    rime->run_task("installation_update");
    ready = true;
  }
}

}  // namespace settings
