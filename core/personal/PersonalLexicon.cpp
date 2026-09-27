#include "PersonalLexicon.h"
#include <PersonalCrypto.h>
#include "../base/clock.h"
#include "../base/utf8.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iterator>
#include <sstream>
#include <thread>


namespace fs = std::filesystem;

namespace {

const char kMagic[] = "WWPL1";
const size_t kMaxWords = 20000;     // 存檔時保留的單詞數
const size_t kMaxPairs = 60000;     // 存檔時保留的接續數
const size_t kMaxUnitLength = 40;   // 太長的片段（多半是貼上的整段文字）不學
const double kMinScore = 0.05;      // 衰減到這以下的直接淘汰
const wchar_t kBackoff = L'\x1f';   // 以「前一個詞的最後兩個字」為鍵的退化接續

int64_t Now() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

bool IsSentenceEnd(wchar_t c) {
  return c == L'。' || c == L'！' || c == L'？' || c == L'!' || c == L'?' ||
         c == L'\n' || c == L'\r';
}

bool IsSeparator(wchar_t c) {
  if (IsSentenceEnd(c) || iswspace(c))
    return true;
  static const wchar_t kPunct[] =
      L"，、；：…—–（）【】《》「」"
      L"『』“”‘’,.;:()[]{}<>\"'`~/\\|";
  return wcschr(kPunct, c) != nullptr;
}

std::wstring BackoffKey(const std::wstring& word) {
  return std::wstring(1, kBackoff) + (word.size() > 2 ? word.substr(word.size() - 2) : word);
}

using personal_crypto::Protect;
using personal_crypto::Unprotect;
using personal_crypto::WriteFileAtomic;

// 解不開的檔案（金鑰遺失、換了帳號、檔案損毀）改名保留，免得之後存檔把它蓋掉；
// 找回金鑰後改回原名就能再讀
void SetAsideUnreadable(const fs::path& path) {
  fs::path aside = path;
  aside += L".unreadable-" + std::to_wstring((long long)time(nullptr));
  std::error_code ec;
  fs::rename(path, aside, ec);
}

}  // namespace

PersonalLexicon::PersonalLexicon(fs::path dir) : dir_(std::move(dir)) {
  std::error_code ec;
  fs::create_directories(dir_, ec);
}

PersonalLexicon::~PersonalLexicon() {
  if (!persist_)
    return;
  // 等背景存檔結束，再做最後一次存檔
  for (int i = 0; i < 500; ++i) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!saving_)
        break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  Save();
}

double PersonalLexicon::Decayed(const Score& s, int64_t now) const {
  const double age_days = (double)(now - s.last) / 86400.0;
  return age_days <= 0 ? s.value : s.value * std::pow(0.5, age_days / half_life_days_);
}

void PersonalLexicon::Bump(Score& s, int64_t now) const {
  s.value = Decayed(s, now) + 1.0;
  s.last = now;
}

std::vector<std::wstring> PersonalLexicon::SplitUnits(const std::wstring& text,
                                                      bool* sentence_end) {
  std::vector<std::wstring> units;
  std::wstring cur;
  *sentence_end = false;
  for (wchar_t c : text) {
    if (IsSeparator(c)) {
      if (!cur.empty())
        units.push_back(cur);
      cur.clear();
      if (IsSentenceEnd(c))
        *sentence_end = true;
    } else {
      cur += c;
    }
  }
  if (!cur.empty())
    units.push_back(cur);
  units.erase(std::remove_if(units.begin(), units.end(),
                             [](const std::wstring& u) { return u.size() > kMaxUnitLength; }),
              units.end());
  return units;
}

void PersonalLexicon::RecordLocked(const std::wstring& window, const std::wstring& text,
                                   int64_t now) {
  bool sentence_end = false;
  const std::vector<std::wstring> units = SplitUnits(text, &sentence_end);
  std::wstring prev = last_word_[window];
  for (const auto& raw : units) {
    // 精煉結果：合併的寫法算到正確寫法；刪除的詞不學，也不拿來接續
    auto m = merged_.find(raw);
    const std::wstring& unit = m != merged_.end() ? m->second : raw;
    if (removed_.count(unit)) {
      prev.clear();
      continue;
    }
    Bump(words_[unit], now);
    if (!prev.empty()) {
      Bump(next_[prev][unit], now);
      Bump(next_[BackoffKey(prev)][unit], now);
    }
    prev = unit;
  }
  // 句尾標點之後重新開始，不把上一句的最後一個詞接到下一句
  last_word_[window] = sentence_end ? std::wstring() : prev;
  if (!units.empty())
    ++dirty_;
}

void PersonalLexicon::Record(const std::wstring& window, const std::wstring& text) {
  if (text.empty())
    return;
  const int64_t now = Now();
  last_activity_ = now;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    RecordLocked(window, text, now);
    if (rebuilding_)
      pending_.push_back({now, window, text});
  }
  if (keep_raw_log_)
    AppendRawLog(window, text, now);
  MaybeSaveAsync();
}

std::vector<std::wstring> PersonalLexicon::NextAfter(const std::wstring& window,
                                                     size_t max_count) const {
  const int64_t now = Now();
  std::lock_guard<std::mutex> lock(mutex_);
  auto w = last_word_.find(window);
  if (w == last_word_.end() || w->second.empty())
    return {};
  std::unordered_map<std::wstring, double> scores;
  auto add = [&](const std::wstring& key, double weight) {
    auto it = next_.find(key);
    if (it == next_.end())
      return;
    for (const auto& [word, s] : it->second)
      scores[word] += weight * Decayed(s, now);
  };
  add(w->second, 1.0);
  add(BackoffKey(w->second), 0.5);  // 完全相同的前詞沒資料時，看最後兩個字相同的
  std::vector<std::pair<double, std::wstring>> ranked;
  for (auto& [word, score] : scores) {
    if (score >= kMinScore)
      ranked.emplace_back(score, word);
  }
  std::sort(ranked.begin(), ranked.end(), [](auto& a, auto& b) { return a.first > b.first; });
  std::vector<std::wstring> out;
  for (size_t i = 0; i < ranked.size() && out.size() < max_count; ++i)
    out.push_back(ranked[i].second);
  return out;
}

std::vector<std::wstring> PersonalLexicon::CompleteFrom(const std::wstring& prefix,
                                                        size_t max_count) const {
  if (prefix.empty())
    return {};
  const int64_t now = Now();
  std::lock_guard<std::mutex> lock(mutex_);
  std::unordered_map<std::wstring, double> scores;
  // 1) 以 prefix 開頭、比 prefix 長的常用詞
  for (const auto& [word, s] : words_) {
    if (word.size() > prefix.size() && word.compare(0, prefix.size(), prefix) == 0)
      scores[word] += Decayed(s, now);
  }
  // 2) prefix 後面最常接的詞
  auto it = next_.find(prefix);
  if (it != next_.end()) {
    for (const auto& [word, s] : it->second)
      scores[prefix + word] += Decayed(s, now);
  }
  std::vector<std::pair<double, std::wstring>> ranked;
  for (auto& [word, score] : scores) {
    if (score >= kMinScore)
      ranked.emplace_back(score, word);
  }
  std::sort(ranked.begin(), ranked.end(), [](auto& a, auto& b) { return a.first > b.first; });
  std::vector<std::wstring> out;
  for (size_t i = 0; i < ranked.size() && out.size() < max_count; ++i)
    out.push_back(ranked[i].second);
  return out;
}

size_t PersonalLexicon::WordCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return words_.size();
}

// 序列化並在記憶體中淘汰：只留分數最高的一批，其餘丟掉
bool PersonalLexicon::SaveLocked(std::string* blob) const {
  const int64_t now = Now();
  std::vector<std::pair<double, const std::wstring*>> words;
  for (const auto& [word, s] : words_) {
    const double v = Decayed(s, now);
    if (v >= kMinScore)
      words.emplace_back(v, &word);
  }
  std::sort(words.begin(), words.end(), [](auto& a, auto& b) { return a.first > b.first; });
  if (words.size() > kMaxWords)
    words.resize(kMaxWords);

  struct Pair {
    double v;
    const std::wstring* prev;
    const std::wstring* next;
  };
  std::vector<Pair> pairs;
  for (const auto& [prev, nexts] : next_) {
    for (const auto& [next, s] : nexts) {
      const double v = Decayed(s, now);
      if (v >= kMinScore)
        pairs.push_back({v, &prev, &next});
    }
  }
  std::sort(pairs.begin(), pairs.end(), [](auto& a, auto& b) { return a.v > b.v; });
  if (pairs.size() > kMaxPairs)
    pairs.resize(kMaxPairs);

  std::ostringstream out;
  out << kMagic << "\n";
  out.precision(6);
  for (const auto& [v, word] : words)
    out << "W\t" << utf8::FromWide(*word) << "\t" << v << "\t" << now << "\n";
  for (const auto& p : pairs)
    out << "N\t" << utf8::FromWide(*p.prev) << "\t" << utf8::FromWide(*p.next) << "\t" << p.v << "\t" << now << "\n";
  *blob = out.str();
  return true;
}

bool PersonalLexicon::Save() {
  std::string plain;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    SaveLocked(&plain);
    dirty_ = 0;
    last_save_ = Now();
  }
  std::string cipher;
  return Protect(plain, &cipher) && WriteFileAtomic(dir_ / L"lexicon.dat", cipher);
}

void PersonalLexicon::MaybeSaveAsync() {
  std::string plain;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const int64_t now = Now();
    if (saving_ || dirty_ == 0 || (dirty_ < 50 && now - last_save_ < 300))
      return;
    SaveLocked(&plain);
    dirty_ = 0;
    last_save_ = now;
    saving_ = true;
  }
  // 加密與寫檔在背景執行，不佔用打字的執行緒
  std::thread([this, plain = std::move(plain)]() {
    std::string cipher;
    if (Protect(plain, &cipher))
      WriteFileAtomic(dir_ / L"lexicon.dat", cipher);
    std::lock_guard<std::mutex> lock(mutex_);
    saving_ = false;
  }).detach();
}

bool PersonalLexicon::Load() {
  std::string cipher;
  {
    std::ifstream in(dir_ / L"lexicon.dat", std::ios::binary);
    if (!in) {
      LoadRefinement();
      return true;  // 尚未建立：空詞庫
    }
    cipher.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  std::string plain;
  if (!Unprotect(cipher, &plain)) {
    SetAsideUnreadable(dir_ / L"lexicon.dat");
    return false;
  }
  LoadRefinement();
  std::istringstream lines(plain);
  std::string line;
  if (!std::getline(lines, line) || line != kMagic) {
    SetAsideUnreadable(dir_ / L"lexicon.dat");
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  words_.clear();
  next_.clear();
  while (std::getline(lines, line)) {
    std::vector<std::string> f;
    size_t start = 0, tab;
    while ((tab = line.find('\t', start)) != std::string::npos) {
      f.push_back(line.substr(start, tab - start));
      start = tab + 1;
    }
    f.push_back(line.substr(start));
    if (f.size() == 4 && f[0] == "W") {
      words_[utf8::ToWide(f[1])] = Score{atof(f[2].c_str()), _atoi64(f[3].c_str())};
    } else if (f.size() == 5 && f[0] == "N") {
      next_[utf8::ToWide(f[1])][utf8::ToWide(f[2])] = Score{atof(f[3].c_str()), _atoi64(f[4].c_str())};
    }
  }
  last_save_ = Now();
  return true;
}

// ---------------------------------------------------------------------------
// 精煉結果（refine.dat，加密）：上次精煉時間、刪除的詞、合併的寫法

bool PersonalLexicon::SaveRefinementLocked(std::string* blob) const {
  std::ostringstream out;
  out << "WWPR1\nT\t" << last_refine_ << "\n";
  for (const auto& word : removed_)
    out << "R\t" << utf8::FromWide(word) << "\n";
  for (const auto& [from, to] : merged_)
    out << "M\t" << utf8::FromWide(from) << "\t" << utf8::FromWide(to) << "\n";
  for (const auto& [word, time] : added_)
    out << "A\t" << utf8::FromWide(word) << "\t" << time << "\n";
  // 已審查的詞：只存還在詞庫裡的，避免無限增長
  for (const auto& word : reviewed_) {
    if (words_.count(word))
      out << "V\t" << utf8::FromWide(word) << "\n";
  }
  *blob = out.str();
  return true;
}

void PersonalLexicon::LoadRefinement() {
  std::string cipher;
  {
    std::ifstream in(dir_ / L"refine.dat", std::ios::binary);
    if (!in)
      return;
    cipher.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  std::string plain;
  if (!Unprotect(cipher, &plain)) {
    SetAsideUnreadable(dir_ / L"refine.dat");
    return;
  }
  std::istringstream lines(plain);
  std::string line;
  if (!std::getline(lines, line) || line != "WWPR1") {
    SetAsideUnreadable(dir_ / L"refine.dat");
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  removed_.clear();
  merged_.clear();
  reviewed_.clear();
  added_.clear();
  while (std::getline(lines, line)) {
    const size_t t1 = line.find('\t');
    if (t1 == std::string::npos)
      continue;
    const std::string kind = line.substr(0, t1), rest = line.substr(t1 + 1);
    if (kind == "T") {
      last_refine_ = _atoi64(rest.c_str());
    } else if (kind == "R") {
      removed_.insert(utf8::ToWide(rest));
    } else if (kind == "V") {
      reviewed_.insert(utf8::ToWide(rest));
    } else if (kind == "A") {
      const size_t t2 = rest.find('\t');
      added_[utf8::ToWide(rest.substr(0, t2))] =
          t2 == std::string::npos ? 0 : _atoi64(rest.substr(t2 + 1).c_str());
    } else if (kind == "M") {
      const size_t t2 = rest.find('\t');
      if (t2 != std::string::npos)
        merged_[utf8::ToWide(rest.substr(0, t2))] = utf8::ToWide(rest.substr(t2 + 1));
    }
  }
}

namespace {
void WriteRefinementFile(const fs::path& path, const std::string& plain) {
  std::string cipher;
  if (Protect(plain, &cipher))
    WriteFileAtomic(path, cipher);
}
}  // namespace

int64_t PersonalLexicon::LastRefine() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_refine_;
}

void PersonalLexicon::SetLastRefine(int64_t time) {
  std::string plain;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    last_refine_ = time;
    SaveRefinementLocked(&plain);
  }
  WriteRefinementFile(dir_ / L"refine.dat", plain);
}

void PersonalLexicon::ApplyRefinement(
    const std::vector<std::wstring>& removals,
    const std::vector<std::pair<std::wstring, std::wstring>>& merges) {
  const int64_t now = Now();
  std::string plain;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& word : removals) {
      if (word.empty())
        continue;
      removed_.insert(word);
      merged_.erase(word);
      added_.erase(word);
      words_.erase(word);
      next_.erase(word);
      for (auto& [prev, nexts] : next_)
        nexts.erase(word);
    }
    for (const auto& [from, to] : merges) {
      if (from.empty() || to.empty() || from == to || removed_.count(to))
        continue;
      merged_[from] = to;
      auto added = added_.find(from);
      if (added != added_.end()) {
        added_[to] = added->second;
        added_.erase(from);
      }
      // 分數以現在的衰減值相加，併到正確寫法
      auto move_score = [&](Score& dst, const Score& src) {
        dst.value = Decayed(dst, now) + Decayed(src, now);
        dst.last = now;
      };
      auto w = words_.find(from);
      if (w != words_.end()) {
        move_score(words_[to], w->second);
        words_.erase(from);
      }
      auto n = next_.find(from);
      if (n != next_.end()) {
        auto moved = std::move(n->second);
        next_.erase(n);
        for (auto& [next, s] : moved)
          move_score(next_[to][next], s);
      }
      for (auto& [prev, nexts] : next_) {
        auto it = nexts.find(from);
        if (it != nexts.end()) {
          const Score s = it->second;
          nexts.erase(it);
          move_score(nexts[to], s);
        }
      }
    }
    ++dirty_;
    SaveRefinementLocked(&plain);
  }
  WriteRefinementFile(dir_ / L"refine.dat", plain);
}

std::vector<std::pair<std::wstring, double>> PersonalLexicon::TopWords(size_t max_count) const {
  const int64_t now = Now();
  std::vector<std::pair<std::wstring, double>> out;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    out.reserve(words_.size());
    for (const auto& [word, s] : words_)
      out.emplace_back(word, Decayed(s, now));
  }
  std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.second > b.second; });
  if (out.size() > max_count)
    out.resize(max_count);
  return out;
}

std::vector<PersonalLexicon::WordInfo> PersonalLexicon::WordInfos() const {
  const int64_t now = Now();
  std::vector<WordInfo> out;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    out.reserve(words_.size());
    for (const auto& [word, s] : words_)
      out.push_back({word, Decayed(s, now), s.last, reviewed_.count(word) > 0});
  }
  std::sort(out.begin(), out.end(), [](const WordInfo& a, const WordInfo& b) { return a.score > b.score; });
  return out;
}

void PersonalLexicon::MarkReviewed(const std::vector<std::wstring>& words) {
  std::string plain;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& w : words)
      reviewed_.insert(w);
    SaveRefinementLocked(&plain);
  }
  personal_crypto::WriteProtected(dir_ / L"refine.dat", plain);
}

size_t PersonalLexicon::PairCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t n = 0;
  for (const auto& [prev, nexts] : next_) {
    if (prev.empty() || prev[0] != kBackoff)
      n += nexts.size();
  }
  return n;
}

// ---------------------------------------------------------------------------
// 原始語料：封存與讀取

std::vector<fs::path> PersonalLexicon::ArchivedLogs() const {
  std::vector<fs::path> files;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(ArchiveDir(), ec)) {
    if (entry.is_regular_file(ec) && entry.path().extension() == L".dat")
      files.push_back(entry.path());
  }
  std::sort(files.begin(), files.end());
  return files;
}

bool PersonalLexicon::ArchiveActiveLog(fs::path* archived) {
  // 持鎖：避免和 AppendRawLog 同時寫入
  std::lock_guard<std::mutex> lock(mutex_);
  std::error_code ec;
  const fs::path active = ActiveLogPath();
  if (!fs::exists(active, ec) || fs::file_size(active, ec) == 0)
    return false;
  fs::create_directories(ArchiveDir(), ec);
  const std::tm t = base::LocalTime();
  wchar_t name[64];
  std::swprintf(name, 64, L"input_log-%04d%02d%02d-%02d%02d%02d.dat", t.tm_year + 1900, t.tm_mon + 1,
                t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
  fs::path target = ArchiveDir() / name;
  for (int i = 2; fs::exists(target, ec); ++i)
    target = ArchiveDir() / (std::wstring(name, wcslen(name) - 4) + L"-" + std::to_wstring(i) + L".dat");
  fs::rename(active, target, ec);
  if (ec) {
    // 不同磁碟區不能直接改名：複製後刪除
    ec.clear();
    if (!fs::copy_file(active, target, ec) || !fs::remove(active, ec))
      return false;
  }
  if (archived)
    *archived = target;
  return true;
}

size_t PersonalLexicon::CountRawRecords(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  size_t count = 0;
  uint32_t len = 0;
  while (in.read((char*)&len, sizeof(len))) {
    in.seekg(len, std::ios::cur);
    if (!in)
      break;
    ++count;
  }
  return count;
}

std::vector<PersonalLexicon::RawRecord> PersonalLexicon::ReadRawLog(const fs::path& path) {
  std::vector<RawRecord> records;
  std::ifstream in(path, std::ios::binary);
  uint32_t len = 0;
  std::string cipher, plain;
  while (in.read((char*)&len, sizeof(len))) {
    cipher.resize(len);
    if (!in.read(cipher.data(), len))
      break;
    if (!Unprotect(cipher, &plain))
      continue;
    // 格式：時間 \t 視窗 \t 文字（文字本身可能含 tab，只切前兩個）
    const size_t t1 = plain.find('\t');
    const size_t t2 = t1 == std::string::npos ? std::string::npos : plain.find('\t', t1 + 1);
    if (t2 == std::string::npos)
      continue;
    RawRecord r;
    r.time = _atoi64(plain.substr(0, t1).c_str());
    r.window = utf8::ToWide(plain.substr(t1 + 1, t2 - t1 - 1));
    r.text = utf8::ToWide(plain.substr(t2 + 1));
    records.push_back(std::move(r));
  }
  return records;
}

void PersonalLexicon::Rebuild(std::vector<RawRecord> records) {
  std::stable_sort(records.begin(), records.end(),
                   [](const RawRecord& a, const RawRecord& b) { return a.time < b.time; });
  {
    std::lock_guard<std::mutex> lock(mutex_);
    rebuilding_ = true;
    pending_.clear();
  }
  // 在暫存的統計上重播，不佔住鎖（重播大量紀錄時打字仍可正常學習與預測）
  PersonalLexicon scratch(dir_);
  scratch.persist_ = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    scratch.removed_ = removed_;
    scratch.merged_ = merged_;
    scratch.half_life_days_ = half_life_days_;
  }
  for (const auto& r : records)
    scratch.RecordLocked(r.window, r.text, r.time);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    words_ = std::move(scratch.words_);
    next_ = std::move(scratch.next_);
    last_word_.clear();
    // 重建期間送出的文字補上
    for (const auto& r : pending_)
      RecordLocked(r.window, r.text, r.time);
    pending_.clear();
    // 手動加入的詞沒有原始紀錄，依加入時間補回（一樣隨時間衰減）
    const int64_t now = Now();
    for (const auto& [word, time] : added_) {
      if (removed_.count(word))
        continue;
      Score& s = words_[word];
      Score manual{5.0, time > 0 ? time : now};
      s.value = Decayed(s, now) + Decayed(manual, now);
      s.last = now;
    }
    rebuilding_ = false;
    ++dirty_;
  }
}

// 原始輸入紀錄：每筆各自加密後附加到檔尾（4 位元組長度 + 密文），供之後的定時精煉使用
void PersonalLexicon::AppendRawLog(const std::wstring& window, const std::wstring& text,
                                   int64_t now) {
  std::ostringstream record;
  record << now << "\t" << utf8::FromWide(window) << "\t" << utf8::FromWide(text);
  std::string cipher;
  if (!Protect(record.str(), &cipher))
    return;
  std::lock_guard<std::mutex> lock(mutex_);
  std::ofstream out(dir_ / L"input_log.dat", std::ios::binary | std::ios::app);
  const uint32_t len = (uint32_t)cipher.size();
  out.write((const char*)&len, sizeof(len));
  out.write(cipher.data(), (std::streamsize)cipher.size());
}

void PersonalLexicon::Clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  words_.clear();
  next_.clear();
  last_word_.clear();
  removed_.clear();
  merged_.clear();
  reviewed_.clear();
  added_.clear();
  pending_.clear();
  last_refine_ = 0;
  dirty_ = 0;
  std::error_code ec;
  fs::remove(dir_ / L"lexicon.dat", ec);
  fs::remove(dir_ / L"input_log.dat", ec);
  fs::remove(dir_ / L"refine.dat", ec);
  fs::remove(dir_ / L"status.txt", ec);
  fs::remove(dir_ / L"export.dat", ec);
  fs::remove(dir_ / L"edit.dat", ec);
  fs::remove_all(ArchiveDir(), ec);
}

// ---------------------------------------------------------------------------
// 手動維護（設定程式的「詞庫管理」）

void PersonalLexicon::AddWord(const std::wstring& word) {
  if (word.empty() || word.size() > kMaxUnitLength)
    return;
  const int64_t now = Now();
  std::string plain;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    removed_.erase(word);
    merged_.erase(word);
    reviewed_.insert(word);  // 使用者親自加入的詞不必再給 LLM 審查
    added_[word] = now;      // 記住，「重新精煉全部」重建後補回
    // 手動加入的詞給較高的起始分數，一樣會隨時間衰減
    Score& s = words_[word];
    s.value = Decayed(s, now) + 5.0;
    s.last = now;
    ++dirty_;
    SaveRefinementLocked(&plain);
  }
  personal_crypto::WriteProtected(dir_ / L"refine.dat", plain);
}

void PersonalLexicon::DeleteWords(const std::vector<std::wstring>& words) {
  std::string plain;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& word : words) {
      words_.erase(word);
      next_.erase(word);
      for (auto& [prev, nexts] : next_)
        nexts.erase(word);
      for (auto& [window, last] : last_word_) {
        if (last == word)
          last.clear();
      }
      added_.erase(word);  // 手動加入的詞：刪除後重建也不補回
    }
    ++dirty_;
    SaveRefinementLocked(&plain);
  }
  personal_crypto::WriteProtected(dir_ / L"refine.dat", plain);
}

void PersonalLexicon::RemoveRules(const std::vector<std::wstring>& unblock,
                                  const std::vector<std::wstring>& unmerge,
                                  const std::vector<std::wstring>& unadd) {
  std::string plain;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& w : unblock)
      removed_.erase(w);
    for (const auto& w : unmerge)
      merged_.erase(w);
    for (const auto& w : unadd)
      added_.erase(w);
    SaveRefinementLocked(&plain);
  }
  personal_crypto::WriteProtected(dir_ / L"refine.dat", plain);
}

void PersonalLexicon::Rules(std::vector<std::wstring>* removed,
                            std::vector<std::pair<std::wstring, std::wstring>>* merged,
                            std::vector<std::wstring>* added) const {
  std::lock_guard<std::mutex> lock(mutex_);
  removed->assign(removed_.begin(), removed_.end());
  std::sort(removed->begin(), removed->end());
  merged->assign(merged_.begin(), merged_.end());
  std::sort(merged->begin(), merged->end());
  if (added) {
    added->clear();
    for (const auto& [word, time] : added_)
      added->push_back(word);
    std::sort(added->begin(), added->end());
  }
}

// 匯出（加密）：W 詞 分數 / R 封鎖的詞 / M 原寫法 正確寫法
bool PersonalLexicon::ExportTo(const fs::path& path, size_t max_words) const {
  std::ostringstream out;
  out << "WWPX1\n";
  out.precision(4);
  for (const auto& [word, score] : TopWords(max_words))
    out << "W\t" << utf8::FromWide(word) << "\t" << score << "\n";
  std::vector<std::wstring> removed;
  std::vector<std::pair<std::wstring, std::wstring>> merged;
  std::vector<std::wstring> added;
  Rules(&removed, &merged, &added);
  for (const auto& w : added)
    out << "A\t" << utf8::FromWide(w) << "\n";
  for (const auto& w : removed)
    out << "R\t" << utf8::FromWide(w) << "\n";
  for (const auto& [from, to] : merged)
    out << "M\t" << utf8::FromWide(from) << "\t" << utf8::FromWide(to) << "\n";
  return personal_crypto::WriteProtected(path, out.str());
}

// 套用設定程式寫的修改（加密）：
// A 詞（加入）、R 詞（刪除並封鎖）、U 詞（解除封鎖）、M 原寫法 正確寫法（合併）、X 原寫法（解除合併）、
// Y 詞（取消手動加入的規則）、D 詞（只刪除，不留規則）
int PersonalLexicon::ApplyEdits(const fs::path& path) {
  std::string plain;
  if (!personal_crypto::ReadProtected(path, &plain))
    return -1;
  std::istringstream lines(plain);
  std::string line;
  if (!std::getline(lines, line) || line != "WWPE1")
    return -1;
  std::vector<std::wstring> add, block, unblock, unmerge, unadd, erase;
  std::vector<std::pair<std::wstring, std::wstring>> merges;
  int count = 0;
  while (std::getline(lines, line)) {
    std::vector<std::wstring> f;
    size_t start = 0, tab;
    while ((tab = line.find('\t', start)) != std::string::npos) {
      f.push_back(utf8::ToWide(line.substr(start, tab - start)));
      start = tab + 1;
    }
    f.push_back(utf8::ToWide(line.substr(start)));
    if (f.size() < 2 || f[1].empty())
      continue;
    ++count;
    if (f[0] == L"A") add.push_back(f[1]);
    else if (f[0] == L"R") block.push_back(f[1]);
    else if (f[0] == L"U") unblock.push_back(f[1]);
    else if (f[0] == L"X") unmerge.push_back(f[1]);
    else if (f[0] == L"Y") unadd.push_back(f[1]);
    else if (f[0] == L"D") erase.push_back(f[1]);
    else if (f[0] == L"M" && f.size() >= 3 && !f[2].empty()) merges.emplace_back(f[1], f[2]);
    else --count;
  }
  if (!unblock.empty() || !unmerge.empty() || !unadd.empty())
    RemoveRules(unblock, unmerge, unadd);
  if (!block.empty() || !merges.empty())
    ApplyRefinement(block, merges);
  if (!erase.empty())
    DeleteWords(erase);
  for (const auto& w : add)
    AddWord(w);
  Save();
  return count;
}
