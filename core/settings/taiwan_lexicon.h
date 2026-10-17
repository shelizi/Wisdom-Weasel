#pragma once

// Bundled vocabulary initialization. No Python/runtime download is needed.
// Only our resource files, preference file and marked schema patches are managed.
#include <rime_api.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

namespace settings::taiwan_lexicon {
namespace fs = std::filesystem;
inline constexpr const char* kBegin = "  # >>> wisdom-taiwan-lexicon";
inline constexpr const char* kEnd = "  # <<< wisdom-taiwan-lexicon";
inline constexpr const char* kFiles[] = {
    "wisdom_taiwan.dict.yaml", "wisdom_taiwan.manifest.json", "wisdom_taiwan.NOTICE.txt",
    "wisdom_taiwan.sources.zip"};

inline std::string Read(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("Cannot read " + path.u8string());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

inline void Write(const fs::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out || !(out << text)) throw std::runtime_error("Cannot write " + path.u8string());
  out.close();
  if (!out) throw std::runtime_error("Cannot flush " + path.u8string());
}

inline std::vector<std::string> Keys(RimeApi* api, RimeConfig* config, const char* path) {
  RimeConfigIterator iter = {};
  std::vector<std::string> keys;
  if (!api->config_begin_map(&iter, config, path))
    throw std::runtime_error("Expected a YAML map at " + std::string(path));
  while (api->config_next(&iter)) keys.emplace_back(iter.key);
  api->config_end(&iter);
  return keys;
}

inline bool Has(const std::vector<std::string>& keys, const char* name) {
  return std::find(keys.begin(), keys.end(), name) != keys.end();
}

inline std::string Patch(RimeApi* api, std::string original, bool enable, bool child) {
  const std::string bom = original.rfind("\xef\xbb\xbf", 0) == 0 ? "\xef\xbb\xbf" : "";
  if (!bom.empty()) original.erase(0, bom.size());
  const std::string newline = original.find("\r\n") != std::string::npos ? "\r\n" : "\n";
  std::vector<std::string> lines;
  bool inside = false;
  for (size_t start = 0; start < original.size();) {
    const size_t end = original.find('\n', start);
    const auto line = original.substr(start, end == std::string::npos ? end : end - start + 1);
    auto content = line;
    while (!content.empty() && (content.back() == '\n' || content.back() == '\r')) content.pop_back();
    if (content == kBegin) {
      if (inside) throw std::runtime_error("Repeated Taiwan vocabulary marker");
      inside = true;
    } else if (content == kEnd) {
      if (!inside) throw std::runtime_error("Unmatched Taiwan vocabulary marker");
      inside = false;
    } else if (!inside) {
      lines.push_back(line);
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  if (inside) throw std::runtime_error("Unclosed Taiwan vocabulary marker");
  std::string cleaned;
  for (const auto& line : lines) cleaned += line;
  const std::regex header("patch:[ \\t]*(#[^\\r\\n]*)?[\\r\\n]*");
  // An empty `patch:` is YAML null (common after removing our only block).
  // Normalize it to a map only for validation; retain the original file bytes.
  auto parse_lines = lines;
  bool has_settings = false;
  for (size_t i = 0; i < parse_lines.size(); ++i) {
    const auto first = parse_lines[i].find_first_not_of(" \t\r\n");
    if (first != std::string::npos && parse_lines[i][first] != '#') has_settings = true;
    if (!std::regex_match(parse_lines[i], header)) continue;
    bool child_value = false;
    for (size_t j = i + 1; j < parse_lines.size(); ++j) {
      const auto next = parse_lines[j].find_first_not_of(" \t\r\n");
      if (next == std::string::npos || parse_lines[j][next] == '#') continue;
      child_value = next > 0;
      break;
    }
    if (!child_value) parse_lines[i] = "patch: {}\n";
  }
  std::string parse_text;
  for (const auto& line : parse_lines) parse_text += line;
  RimeConfig config = {};
  if (!api->config_load_string(&config, has_settings ? parse_text.c_str() : "{}"))
    throw std::runtime_error("Cannot parse schema custom YAML");
  bool replaces_packs = false, has_patch = false;
  try {
    const auto root = Keys(api, &config, "");
    has_patch = Has(root, "patch");
    if (has_patch) {
      // Literal patch keys contain '/': enumerate keys instead of splitting them as paths.
      RimeConfig patch = {};
      if (api->config_get_item(&config, "patch", &patch)) {
        try {
          const auto keys = Keys(api, &patch, "");
          replaces_packs = Has(keys, "translator/packs");
          if (Has(keys, "translator")) replaces_packs |= Has(Keys(api, &patch, "translator"), "packs");
          if (enable && (!child || replaces_packs) && Has(keys, "translator/packs/+"))
            throw std::runtime_error("Custom translator/packs/+ already exists; left unchanged");
        } catch (...) { api->config_close(&patch); throw; }
        api->config_close(&patch);
      }
    }
  } catch (...) { api->config_close(&config); throw; }
  api->config_close(&config);
  if (enable && (!child || replaces_packs)) {
    auto found = std::find_if(lines.begin(), lines.end(), [&](const std::string& line) {
      return std::regex_match(line, header);
    });
    const auto block = std::string(kBegin) + newline + "  translator/packs/+:" + newline +
                       "    - wisdom_taiwan" + newline + kEnd + newline;
    if (found == lines.end()) {
      // Never replace an existing inline patch map or its comments.
      if (has_patch)
        throw std::runtime_error("Inline patch map left unchanged");
      if (!cleaned.empty() && cleaned.back() != '\n') cleaned += newline;
      cleaned += "patch:" + newline + block;
    } else {
      if (found->empty() || found->back() != '\n') *found += newline;
      lines.insert(found + 1, block);
      cleaned.clear();
      for (const auto& line : lines) cleaned += line;
    }
  }
  return bom + cleaned;
}

inline bool Prepare(RimeApi* api, const fs::path& shared, const fs::path& user,
                    std::string* error, bool* changed = nullptr) {
  if (error) error->clear();
  if (changed) *changed = false;
  struct Change { fs::path path; std::string before, after; bool existed; };
  std::vector<Change> changes;
  fs::path temporary;
  try {
    fs::create_directories(user);
    const auto preference = user / "wisdom_taiwan.user.yaml";
    bool enabled = true;
    if (fs::exists(preference)) {
      RimeConfig config = {};
      const auto text = Read(preference);
      if (!api->config_load_string(&config, text.c_str()))
        throw std::runtime_error("Invalid wisdom_taiwan.user.yaml");
      Bool flag = True;
      const bool valid = api->config_get_bool(&config, "enabled", &flag);
      api->config_close(&config);
      if (!valid) throw std::runtime_error("Missing enabled in wisdom_taiwan.user.yaml");
      enabled = !!flag;
    }
    const auto add = [&](const fs::path& path, const std::string& text) {
      const bool exists = fs::exists(path);
      const auto before = exists ? Read(path) : std::string();
      if (!exists || before != text) changes.push_back({path, before, text, exists});
    };
    // Preflight every file before writing anything. Bundled data is installed even
    // when disabled, so the associated attribution always accompanies the data.
    for (const auto name : kFiles) add(user / name, Read(shared / name));
    if (!fs::exists(preference)) add(preference, "# Bundled Taiwan vocabulary; set false to disable.\nenabled: true\n");
    for (const auto schema : {"bopomofo", "bopomofo_express", "bopomofo_tw"}) {
      const auto path = user / (std::string(schema) + ".custom.yaml");
      const bool exists = fs::exists(path);
      const auto before = exists ? Read(path) : std::string();
      const auto after = Patch(api, before, enabled, std::string(schema) != "bopomofo");
      if (exists || !after.empty()) add(path, after);
    }
    if (changes.empty()) return true;
    const auto id = std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const auto backup = user / ("backup-taiwan-native-" + id);
    fs::create_directory(backup);
    for (const auto& change : changes)
      if (change.existed) Write(backup / change.path.filename(), change.before);
    size_t applied = 0;
    try {
      for (const auto& change : changes) {
        temporary = change.path;
        temporary += ".tmp-" + id;
        Write(temporary, change.after);
        fs::rename(temporary, change.path);
        ++applied;
      }
    } catch (...) {
      for (size_t i = 0; i < applied; ++i) {
        if (changes[i].existed) Write(changes[i].path, changes[i].before);
        else fs::remove(changes[i].path);
      }
      throw;
    }
    if (changed) *changed = true;
    return true;
  } catch (const std::exception& ex) {
    if (!temporary.empty()) { std::error_code ec; fs::remove(temporary, ec); }
    if (error) *error = ex.what();
    return false;
  }
}
}  // namespace settings::taiwan_lexicon
