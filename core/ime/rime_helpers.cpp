#include "rime_helpers.h"

#include <algorithm>
#include <cstring>

#include "../base/utf8.h"

namespace ime {

ZhuyinSpeller LoadZhuyinSpeller(RimeApi* api, const char* schema_id) {
  ZhuyinSpeller sp;
  RimeConfig config = {NULL};
  if (!schema_id || !api->schema_open(schema_id, &config))
    return sp;
  if (const char* finals = api->config_get_cstring(&config, "speller/finals"))
    sp.finals = finals;
  if (const char* alphabet = api->config_get_cstring(&config, "speller/alphabet"))
    sp.alphabet = alphabet;
  if (const char* delim = api->config_get_cstring(&config, "speller/delimiter"))
    if (*delim)
      sp.delimiter = *delim;
  const size_t n = api->config_list_size(&config, "translator/preedit_format");
  for (size_t i = 0; i < n; ++i) {
    const std::string key = "translator/preedit_format/@" + std::to_string(i);
    const char* rule = api->config_get_cstring(&config, key.c_str());
    if (!rule || std::strncmp(rule, "xlit", 4) != 0 || !rule[4])
      continue;
    const std::string r(rule + 5);
    const char sep = rule[4];
    const size_t mid = r.find(sep);
    if (mid == std::string::npos)
      continue;
    const size_t end = r.find(sep, mid + 1);
    const std::wstring from = utf8::ToWide(r.substr(0, mid));
    const std::wstring to = utf8::ToWide(r.substr(mid + 1, end - mid - 1));
    for (size_t k = 0; k < from.size() && k < to.size(); ++k)
      sp.xlit[from[k]] = to[k];
  }
  api->config_close(&config);
  return sp;
}

std::wstring TakeComposition(RimeApi* api, RimeSessionId session_id) {
  std::wstring text;
  api->commit_composition(session_id);
  RIME_STRUCT(RimeCommit, commit);
  if (api->get_commit(session_id, &commit)) {
    if (commit.text)
      text = utf8::ToWide(commit.text);
    api->free_commit(&commit);
  }
  return text;
}

bool SelectText(RimeApi* api,
                RimeSessionId session_id,
                const std::string& input,
                const std::vector<std::wstring>& units) {
  api->clear_composition(session_id);
  api->set_input(session_id, input.c_str());
  size_t pos = 0;
  while (pos < units.size()) {
    int best = -1, best_len = 0, idx = 0;
    RimeCandidateListIterator iter = {0};
    if (api->candidate_list_begin(session_id, &iter)) {
      while (idx < 300 && api->candidate_list_next(&iter)) {
        if (iter.candidate.text) {
          const std::wstring text = utf8::ToWide(iter.candidate.text);
          const int len = (int)zhuyin_preview::SplitChars(text).size();
          if (len > best_len && pos + len <= units.size() &&
              text == zhuyin_preview::Join(units, pos, pos + len)) {
            best = idx;
            best_len = len;
          }
        }
        ++idx;
      }
      api->candidate_list_end(&iter);
    }
    if (best < 0 || !api->select_candidate(session_id, best)) {
      // 選不到：恢復原本的整句
      api->clear_composition(session_id);
      api->set_input(session_id, input.c_str());
      return false;
    }
    pos += best_len;
  }
  return true;
}

std::vector<std::wstring> HomophoneFinder::Find(const std::string& schema, const std::string& keys) {
  const std::string cache_key = schema + "\t" + keys;
  auto it = cache_.find(cache_key);
  if (it != cache_.end())
    return it->second;
  // 背景 session 被清掉（例如整理選字記憶）就重建
  if (!session_ || !api_->find_session(session_)) {
    session_ = api_->create_session();
    schema_.clear();
  }
  std::vector<std::wstring> result;
  if (!session_)
    return result;
  if (schema_ != schema) {
    api_->select_schema(session_, schema.c_str());
    schema_ = schema;
  }
  api_->set_input(session_, keys.c_str());
  RimeCandidateListIterator iter = {0};
  if (api_->candidate_list_begin(session_, &iter)) {
    for (int i = 0; i < 60 && result.size() < 6 && api_->candidate_list_next(&iter); ++i) {
      if (!iter.candidate.text)
        continue;
      const std::wstring text = utf8::ToWide(iter.candidate.text);
      if (zhuyin_preview::SplitChars(text).size() == 1 &&
          std::find(result.begin(), result.end(), text) == result.end())
        result.push_back(text);
    }
    api_->candidate_list_end(&iter);
  }
  api_->clear_composition(session_);
  if (cache_.size() > 5000)
    cache_.clear();
  cache_[cache_key] = result;
  return result;
}

}  // namespace ime
