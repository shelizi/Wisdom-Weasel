#include "choice_stats.h"

#include <cstdio>
#include <fstream>
#include <sstream>

#include "../base/clock.h"

namespace ime {

namespace fs = std::filesystem;

std::string DateString(int days_ago) {
  const std::tm t = base::LocalTime(std::time(nullptr) - (std::time_t)days_ago * 24 * 3600);
  char date[16];
  std::snprintf(date, sizeof(date), "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  return date;
}

std::string ProfileKey(const std::string& version, const std::string& settings) {
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : version + "|" + settings) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  char key[20];
  std::snprintf(key, sizeof(key), "%08llx", (unsigned long long)(h & 0xffffffffULL));
  return key;
}

void ChoiceStatsStore::Load() {
  if (loaded_)
    return;
  loaded_ = true;
  {
    std::ifstream in(dir_ / "weasel_stats_profiles.txt", std::ios::binary);
    for (std::string line; std::getline(in, line);) {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      const size_t tab = line.find('\t');
      if (tab != std::string::npos)
        profiles_[line.substr(0, tab)] = line;
    }
  }
  std::ifstream in(dir_ / "weasel_stats.txt", std::ios::binary);
  for (std::string line; std::getline(in, line);) {
    std::istringstream f(line);
    std::string date, key;
    if (!(f >> date >> key))
      continue;
    // 舊格式沒有組合代碼（第二欄就是數字）：歸到「舊資料」
    std::istringstream numbers;
    if (key.find_first_not_of("0123456789") == std::string::npos) {
      numbers.str(line.substr(line.find(date) + date.size()));
      key = "legacy";
    } else {
      numbers.str(line.substr(line.find(key) + key.size()));
    }
    ChoiceStats s;
    if (numbers >> s.commits >> s.chars >> s.changed >> s.llm_offered >> s.llm_used >>
        s.corrections_used >> s.backspaces) {
      // 較新的欄位，舊檔沒有就是 0
      numbers >> s.deleted_after >> s.focus_uses >> s.recommend_offered >> s.recommend_used;
      stats_[date + "\t" + key] = s;
    }
  }
}

ChoiceStats& ChoiceStatsStore::Today(const std::string& profile) {
  Load();
  return stats_[DateString() + "\t" + profile];
}

void ChoiceStatsStore::AddProfile(const std::string& key,
                                  const std::string& version,
                                  const std::string& build_time,
                                  const std::string& subject,
                                  const std::string& settings) {
  Load();
  if (profiles_.count(key))
    return;
  const std::string line =
      key + '\t' + version + '\t' + build_time + '\t' + subject + '\t' + settings + '\t' + DateString();
  profiles_[key] = line;
  std::ofstream out(dir_ / "weasel_stats_profiles.txt", std::ios::binary | std::ios::app);
  out << line << '\n';
}

void ChoiceStatsStore::Save() {
  Load();
  const std::string cutoff = DateString(90);
  while (!stats_.empty() && stats_.begin()->first < cutoff)
    stats_.erase(stats_.begin());
  std::ofstream out(dir_ / "weasel_stats.txt", std::ios::binary | std::ios::trunc);
  for (const auto& [day_key, s] : stats_)
    out << day_key << '\t' << s.commits << '\t' << s.chars << '\t' << s.changed << '\t'
        << s.llm_offered << '\t' << s.llm_used << '\t' << s.corrections_used << '\t' << s.backspaces
        << '\t' << s.deleted_after << '\t' << s.focus_uses << '\t' << s.recommend_offered << '\t'
        << s.recommend_used << '\n';
}

void ChoiceStatsStore::Reset() {
  stats_.clear();
  profiles_.clear();
  loaded_ = true;
  std::error_code ec;
  fs::remove(dir_ / "weasel_stats.txt", ec);
  fs::remove(dir_ / "weasel_stats_profiles.txt", ec);
}

}  // namespace ime
