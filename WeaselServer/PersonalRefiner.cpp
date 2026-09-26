#include "stdafx.h"
#include "PersonalRefiner.h"
#include "DevConsole.h"
#include "LLMProvider.h"
#include <WeaselUtility.h>
#include <rime_api.h>
#include <rime_levers_api.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <shellapi.h>
#include <unordered_map>
#include <sstream>
#include <unordered_set>

namespace fs = std::filesystem;
extern DevConsole* g_dev_console;

namespace {

// 分批審查：每批送的待審詞、參考用的常用詞、例句數（本機模型上下文較小，送少一點）
const size_t kBatchRemote = 300, kBatchLocal = 120;
const size_t kReferenceRemote = 50, kReferenceLocal = 30;
const size_t kExamplesRemote = 40, kExamplesLocal = 20;
const size_t kExampleRecords = 20000;  // 一般精煉時，最多讀最近幾筆原始紀錄來挑例句
const size_t kExampleLength = 100;   // 每個例句最多幾個字
const int64_t kIdleSeconds = 300;    // 自動精煉：停止打字多久後才開始
const unsigned long kTimeoutMs = 90000;
// 匯出給 Rime 的詞：2～7 字（terra_pinyin 的 max_phrase_length）、全為漢字，最多幾個
const size_t kRimeMinLength = 2, kRimeMaxLength = 7, kRimeMaxWords = 5000;

bool IsHan(wchar_t c) {
  return (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) ||
         (c >= 0xF900 && c <= 0xFAFF);
}

int64_t Now() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

void Log(const std::wstring& text) {
  if (g_dev_console && g_dev_console->IsEnabled())
    g_dev_console->WriteLine(L"[個人詞庫] " + text);
}

std::wstring Trim(const std::wstring& s) {
  const wchar_t* ws = L" \t\r\n　：:";
  const size_t b = s.find_first_not_of(ws);
  if (b == std::wstring::npos)
    return std::wstring();
  return s.substr(b, s.find_last_not_of(ws) - b + 1);
}

// 依 tab、空白、箭頭切開
std::vector<std::wstring> Tokens(const std::wstring& s) {
  std::wstring t = s;
  for (const wchar_t* arrow : {L"->", L"→", L"=>", L"⇒"}) {
    size_t p;
    while ((p = t.find(arrow)) != std::wstring::npos)
      t.replace(p, wcslen(arrow), L" ");
  }
  std::vector<std::wstring> out;
  std::wstring cur;
  for (wchar_t c : t) {
    if (iswspace(c) || c == L'　' || c == L'：' || c == L':' || c == L'，' || c == L',') {
      if (!cur.empty())
        out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty())
    out.push_back(cur);
  // 去掉 LLM 常加的引號
  for (auto& w : out) {
    while (!w.empty() && wcschr(L"「」『』\"'“”‘’`", w.front()))
      w.erase(0, 1);
    while (!w.empty() && wcschr(L"「」『』\"'“”‘’`", w.back()))
      w.pop_back();
  }
  out.erase(std::remove(out.begin(), out.end(), std::wstring()), out.end());
  return out;
}

const wchar_t kSystemPrompt[] =
    L"你是中文輸入法「個人詞庫」的整理助手。使用者平常打字時，輸入法記下了常用詞（依使用頻率排序）與一些原始例句。"
    L"請找出需要整理的項目：\n"
    L"1. 刪除：錯字、亂碼、打錯的注音組合、沒有意義或不完整的片段、看起來像密碼、帳號、電話或金鑰等隱私內容。\n"
    L"2. 合併：同一個詞的錯誤或不一致寫法（例如錯別字、簡體字、全半形不同），合併到正確的繁體中文寫法。\n"
    L"只能處理「請審查的詞」清單中的詞；「常用詞」只是讓你了解使用者平常的用字，不要處理。"
    L"正常的詞不要動，不確定時保留。\n"
    L"每行輸出一項，格式如下（欄位以 Tab 分隔）：\n"
    L"刪除\t詞\n"
    L"合併\t原寫法\t正確寫法\n"
    L"沒有需要整理的就只輸出「無」。不要輸出任何其他說明。";

// Rime 選字記憶（使用者詞典）的審查：每批送的詞數
const size_t kMemoryBatchRemote = 300, kMemoryBatchLocal = 120;

const wchar_t kRimeMemoryPrompt[] =
    L"你是中文注音輸入法「選字記憶」的整理助手。使用者打字時，輸入法記住了使用者送出過的詞與它的拼音，"
    L"之後會優先推薦這些詞。其中可能混進錯字錯詞：選錯的同音字、不成詞的組合、打錯注音產生的怪詞。\n"
    L"請找出這些錯字錯詞，讓輸入法忘掉它們。正常的詞、專有名詞、人名、口語或網路用語都不要動，不確定時保留。\n"
    L"每行輸出一個要刪除的詞，格式：刪除\t詞\n"
    L"沒有要刪除的就只輸出「無」。不要輸出任何其他說明。";

}  // namespace

PersonalRefiner::PersonalRefiner(PersonalLexicon* lexicon) : lexicon_(lexicon) {
  // 沿用上次的結果說明（status.txt）
  std::ifstream in(lexicon_->Dir() / L"status.txt", std::ios::binary);
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("last_result=", 0) == 0)
      last_result_ = u8tow(line.substr(12));
  }
}

PersonalRefiner::~PersonalRefiner() {
  Stop();
}

void PersonalRefiner::Configure(const Config& config) {
  std::lock_guard<std::mutex> lock(mutex_);
  config_ = config;
  cv_.notify_all();
}

void PersonalRefiner::Start() {
  if (scheduler_.joinable())
    return;
  // 第一次使用：從現在開始算，一個週期後才自動精煉
  if (lexicon_->LastRefine() == 0)
    lexicon_->SetLastRefine(Now());
  // 重新部署後輸入法會重建精煉器：從現有的 Rime 詞典檔讀回詞數與更新時間
  {
    std::lock_guard<std::mutex> lock(mutex_);
    std::error_code ec;
    const fs::path dict(config_.rime_dict_path);
    if (!config_.rime_dict_path.empty() && fs::exists(dict, ec)) {
      std::ifstream in(dict, std::ios::binary);
      bool body = false;
      size_t words = 0;
      for (std::string line; std::getline(in, line);) {
        if (body && line.find('	') != std::string::npos)
          ++words;
        else if (line.rfind("...", 0) == 0)
          body = true;
      }
      rime_words_ = words;
      WIN32_FILE_ATTRIBUTE_DATA attr;
      if (GetFileAttributesExW(dict.c_str(), GetFileExInfoStandard, &attr)) {
        const ULONGLONG t = ((ULONGLONG)attr.ftLastWriteTime.dwHighDateTime << 32) |
                            attr.ftLastWriteTime.dwLowDateTime;
        rime_updated_ = (int64_t)((t - 116444736000000000ULL) / 10000000ULL);
      }
    }
  }
  stop_ = false;
  scheduler_ = std::thread([this]() { SchedulerLoop(); });
  WriteStatus();
}

void PersonalRefiner::RequestStop() {
  std::lock_guard<std::mutex> lock(mutex_);
  stop_ = true;
  cv_.notify_all();
}

void PersonalRefiner::Stop() {
  RequestStop();
  if (scheduler_.joinable())
    scheduler_.join();
  std::lock_guard<std::mutex> lock(run_mutex_);
  if (worker_.joinable())
    worker_.join();
}

void PersonalRefiner::SchedulerLoop() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (!stop_) {
    cv_.wait_for(lock, std::chrono::seconds(60), [this]() { return stop_.load(); });
    if (stop_)
      break;
    const double interval = config_.interval_days;
    const int64_t now = Now();
    if (interval <= 0 || running_ || now < retry_after_)
      continue;
    if (now - lexicon_->LastRefine() < (int64_t)(interval * 86400))
      continue;
    if (now - lexicon_->LastActivity() < kIdleSeconds)
      continue;
    lock.unlock();
    Log(L"已到精煉週期，開始自動精煉");
    RunAsync(false);
    lock.lock();
  }
}

bool PersonalRefiner::RunAsync(bool full) {
  std::lock_guard<std::mutex> lock(run_mutex_);
  if (running_ || stop_)
    return false;
  if (worker_.joinable())
    worker_.join();  // 上一次已結束
  running_ = true;
  WriteStatus();
  worker_ = std::thread([this, full]() {
    // 要求停止時，串流請求與本機模型的生成都會立刻中斷，不讓等待的人（例如重新部署）卡住
    LLMCancelScope cancel([this]() { return stop_.load(); });
    Run(full);
  });
  return true;
}

bool PersonalRefiner::Clear() {
  std::lock_guard<std::mutex> lock(run_mutex_);
  if (running_)
    return false;
  lexicon_->Clear();
  lexicon_->SetLastRefine(Now());
  {
    std::lock_guard<std::mutex> lock2(mutex_);
    last_result_ = L"已清除所有資料";
  }
  WriteStatus();
  return true;
}

void PersonalRefiner::Run(bool full) {
  Config config;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    config = config_;
  }
  const ULONGLONG started = GetTickCount64();
  std::wstring result;
  bool ok = true;
  try {
    // 原始紀錄：重新精煉全部要全部（重建用）；一般精煉只需要最近的一部分（挑例句用）
    std::vector<PersonalLexicon::RawRecord> records;
    const auto active = PersonalLexicon::ReadRawLog(lexicon_->ActiveLogPath());
    const auto archives = lexicon_->ArchivedLogs();
    size_t archived_files = 0;
    for (auto it = archives.rbegin(); it != archives.rend(); ++it) {
      if (!full && records.size() + active.size() >= kExampleRecords)
        break;
      auto part = PersonalLexicon::ReadRawLog(*it);
      records.insert(records.begin(), part.begin(), part.end());
      ++archived_files;
    }
    records.insert(records.end(), active.begin(), active.end());
    if (full) {
      Log(L"重新精煉全部：重建 " + std::to_wstring(records.size()) + L" 筆紀錄");
      lexicon_->Rebuild(records);
    }

    std::wstring detail;
    if (!config.UsesLLM())
      detail = L"未選擇精煉模型，只做統計整理";
    else
      detail = RefineWithLLM(config, full, records, &ok);
    // 順便整理 Rime 的選字記憶；失敗不影響個人詞庫的精煉，下次精煉會接著審查
    if (ok && config.UsesLLM() && config.clean_rime_memory && !stop_) {
      bool memory_ok = true;
      const std::wstring memory = RefineRimeMemory(config, full, &memory_ok);
      if (!memory.empty())
        detail += L"；" + memory;
    }

    if (ok) {
      // 精煉完成：累積中的紀錄封存，重新累積
      if (!active.empty())
        lexicon_->ArchiveActiveLog();
      lexicon_->Save();
      lexicon_->SetLastRefine(Now());
      retry_after_ = 0;
    } else {
      lexicon_->Save();  // 已完成的批次仍然保留
      retry_after_ = Now() + 3600;
    }
    std::wostringstream text;
    text << (full ? L"重新精煉全部" : L"精煉") << (ok ? L"完成" : L"失敗") << L"（";
    if (full)
      text << records.size() << L" 筆紀錄、" << archived_files << L" 個封存檔，";
    else
      text << L"新紀錄 " << active.size() << L" 筆，";
    text << (GetTickCount64() - started + 500) / 1000 << L" 秒）：" << detail;
    result = text.str();
  } catch (const std::exception& e) {
    result = L"精煉失敗：" + u8tow(e.what());
    retry_after_ = Now() + 3600;
  }
  Log(result);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    last_result_ = result;
    progress_.clear();
  }
  // 先寫好結果再標記結束，讓等 Running() 的人讀到的是最新的狀態
  WriteStatusAs(false);
  running_ = false;
  // 精煉後詞庫有變，更新給 Rime 的詞典（有變動才重新部署）
  if (ok)
    ExportRimeDictAndDeploy();
}

int PersonalRefiner::ExportRimeDict() {
  Config config;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    config = config_;
  }
  if (config.rime_dict_path.empty())
    return -1;
  // 挑詞：常用（分數 ≥ 2）或 LLM 審查過而保留（分數 ≥ 1）；單字交給 Rime 自己的排序
  std::vector<std::pair<std::wstring, double>> picked;
  for (const auto& info : lexicon_->WordInfos()) {
    const size_t n = info.word.size();
    if (n < kRimeMinLength || n > kRimeMaxLength)
      continue;
    if (!std::all_of(info.word.begin(), info.word.end(), IsHan))
      continue;
    if (info.score >= 2 || (info.reviewed && info.score >= 1))
      picked.emplace_back(info.word, info.score);
    if (picked.size() >= kRimeMaxWords)
      break;
  }
  // 原本的詞頻（essay.txt）：在原有權重上加分，讓同音詞裡你常打的排前面
  std::unordered_map<std::wstring, long long> essay;
  if (!picked.empty() && !config.essay_path.empty()) {
    std::unordered_set<std::wstring> wanted;
    for (const auto& [w, s] : picked)
      wanted.insert(w);
    std::ifstream in(fs::path(config.essay_path), std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
      const size_t tab = line.find('\t');
      if (tab == std::string::npos)
        continue;
      const std::wstring w = u8tow(line.substr(0, tab));
      if (wanted.count(w))
        essay[w] = _atoi64(line.c_str() + tab + 1);
    }
  }
  std::ostringstream out;
  out << u8"# 小狼毫個人詞庫：你常打的詞（由輸入法自動產生，請勿手動修改）\n"
      << u8"# 在「小狼毫設定 → 個人詞庫」取消「讓常打的詞在注音選字時排前面」即可停用\n"
      << "---\n"
      << "name: terra_pinyin.personal\n"
      << "version: \"1\"\n"
      << "sort: by_weight\n"
      << "use_preset_vocabulary: true\n"
      << "max_phrase_length: 7\n"
      << "min_phrase_weight: 100\n"
      << "import_tables:\n"
      << "  - terra_pinyin\n"
      << "columns:\n"
      << "  - text\n"
      << "  - weight\n"
      << "...\n\n";
  for (const auto& [w, score] : picked) {
    // 分數取整數再換算，小幅變動不會讓內容改變（避免每天都重新部署）
    const long long bonus = 100000 + (long long)(std::min)(score, 50.0) * 20000;
    out << wtou8(w) << "\t" << essay[w] + bonus << "\n";
  }
  const std::string content = out.str();
  std::string old;
  {
    std::ifstream in(fs::path(config.rime_dict_path), std::ios::binary);
    old.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    rime_words_ = picked.size();
  }
  if (old == content)
    return 0;
  const fs::path path(config.rime_dict_path);
  const fs::path tmp = path.wstring() + L".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f)
      return -1;
    f.write(content.data(), (std::streamsize)content.size());
  }
  if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
    return -1;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    rime_updated_ = Now();
  }
  Log(L"已更新給 Rime 的個人詞表：" + std::to_wstring(picked.size()) + L" 個詞");
  return 1;
}

void PersonalRefiner::ExportRimeDictAndDeploy() {
  Config config;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    config = config_;
  }
  if (!config.rime_boost || ExportRimeDict() != 1) {
    WriteStatus();
    return;
  }
  WriteStatus();
  // 重新部署讓 Rime 編譯新的詞典（和托盤的「重新部署」相同）
  if (!config.deployer_path.empty()) {
    Log(L"個人詞表有變動，重新部署");
    ShellExecuteW(NULL, NULL, config.deployer_path.c_str(), L"/deploy", NULL, SW_SHOWNORMAL);
  }
}

std::wstring PersonalRefiner::RefineWithLLM(
    const Config& config, bool full, const std::vector<PersonalLexicon::RawRecord>& records,
    bool* ok) {
  *ok = true;
  const bool local = config.type == "llamacpp";
  const size_t batch_size = local ? kBatchLocal : kBatchRemote;
  const size_t infos_count = lexicon_->WordCount();
  // 詞庫還小的時候，參考詞最多佔四分之一，其餘都要審查
  const size_t reference_count = (std::min)(local ? kReferenceLocal : kReferenceRemote, infos_count / 4);
  const size_t max_examples = local ? kExamplesLocal : kExamplesRemote;

  const auto infos = lexicon_->WordInfos();  // 依分數排序
  if (infos.empty())
    return L"詞庫是空的";

  // 最常用的詞當參考、不審查（常用詞很少是錯字，也避免模型誤刪）
  std::vector<std::wstring> reference;
  std::vector<const PersonalLexicon::WordInfo*> pool;
  for (size_t i = 0; i < infos.size(); ++i) {
    if (i < reference_count)
      reference.push_back(infos[i].word);
    else if (full || !infos[i].reviewed)
      pool.push_back(&infos[i]);
  }
  if (!full) {
    // 一般精煉：還沒審查過的詞，最近出現的優先（新冒出來的最可能是打錯的），一批
    std::stable_sort(pool.begin(), pool.end(),
                     [](auto* a, auto* b) { return a->last > b->last; });
    if (pool.size() > batch_size)
      pool.resize(batch_size);
  }
  if (pool.empty())
    return L"沒有需要審查的新詞";
  const size_t batches = (pool.size() + batch_size - 1) / batch_size;

  // 例句：同一視窗 60 秒內連續送出的合成一句
  std::vector<std::wstring> groups;
  {
    std::wstring cur, cur_window;
    int64_t cur_time = 0;
    for (const auto& r : records) {
      if (!cur.empty() && (r.window != cur_window || r.time - cur_time > 60)) {
        groups.push_back(cur);
        cur.clear();
      }
      cur += r.text;
      cur_window = r.window;
      cur_time = r.time;
    }
    if (!cur.empty())
      groups.push_back(cur);
  }

  // 本機模型只載入一次，所有批次共用
  LLMLocalChatSession session;
  if (local) {
    LLMLocalModelSpec spec;
    spec.model_path = config.model_path;
    spec.instruct = config.instruct;
    spec.n_gpu_layers = config.n_gpu_layers;
    spec.n_threads = config.n_threads;
    spec.disable_thinking = config.disable_thinking;
    spec.think_tokens = config.think_tokens;
    std::wstring error;
    Log(L"載入本機精煉模型：" + u8tow(config.model_path));
    if (!session.Open(spec, &error)) {
      *ok = false;
      return error;
    }
  }

  std::wstring reference_text;
  for (size_t i = 0; i < reference.size(); ++i)
    reference_text += (i ? L"、" : L"") + reference[i];

  size_t reviewed = 0, merged = 0;
  std::vector<std::wstring> removed;
  auto summary = [&]() {
    std::wostringstream out;
    out << L"審查 " << reviewed << L" 個詞";
    if (batches > 1)
      out << L"（" << batches << L" 批）";
    out << L"，刪除 " << removed.size() << L" 個、合併 " << merged << L" 個";
    std::wstring sample;
    for (size_t i = 0; i < removed.size() && i < 5; ++i)
      sample += (sample.empty() ? L"" : L"、") + removed[i];
    if (!sample.empty())
      out << L"（刪除：" << sample << (removed.size() > 5 ? L"…" : L"") << L"）";
    return out.str();
  };

  for (size_t b = 0; b < batches; ++b) {
    if (stop_) {
      *ok = false;
      return L"已中止（完成 " + std::to_wstring(b) + L"／" + std::to_wstring(batches) + L" 批）；" +
             summary();
    }
    if (batches > 1) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        progress_ = L"第 " + std::to_wstring(b + 1) + L"／" + std::to_wstring(batches) + L" 批";
      }
      WriteStatus();
    }
    std::vector<std::wstring> targets;
    std::unordered_set<std::wstring> known;
    std::wostringstream user;
    user << L"請審查的詞（詞\t使用分數）：\n";
    for (size_t i = b * batch_size; i < pool.size() && i < (b + 1) * batch_size; ++i) {
      targets.push_back(pool[i]->word);
      known.insert(pool[i]->word);
      user << pool[i]->word << L"\t" << (int)(pool[i]->score + 0.5) << L"\n";
    }
    user << L"\n常用詞（參考用，不必處理）：" << reference_text << L"\n";

    // 例句：優先挑含有這批待審詞的句子（從最近的開始找），讓模型看到實際用法
    std::vector<std::wstring> examples;
    std::unordered_set<std::wstring> covered;
    for (auto g = groups.rbegin(); g != groups.rend() && examples.size() < max_examples; ++g) {
      for (const auto& t : targets) {
        if (!covered.count(t) && g->find(t) != std::wstring::npos) {
          covered.insert(t);
          examples.push_back(*g);
          break;
        }
      }
    }
    user << L"\n例句：\n";
    for (const auto& e : examples) {
      std::wstring line = e.size() > kExampleLength ? e.substr(0, kExampleLength) : e;
      std::replace(line.begin(), line.end(), L'\n', L' ');
      std::replace(line.begin(), line.end(), L'\r', L' ');
      user << L"- " << line << L"\n";
    }

    std::wstring content, error;
    bool batch_ok = true;
    if (local) {
      Log(L"本機精煉 第 " + std::to_wstring(b + 1) + L"／" + std::to_wstring(batches) + L" 批：" +
          std::to_wstring(targets.size()) + L" 個詞、" + std::to_wstring(examples.size()) + L" 句例句");
      std::string output;
      batch_ok = session.Chat(wtou8(kSystemPrompt), wtou8(user.str()), 1024, &output, &error);
      content = u8tow(output);
    } else {
      content = CallRemote(config, user.str(), targets.size(), examples.size(), &batch_ok);
      if (!batch_ok)
        error = content;
    }
    if (!batch_ok) {
      *ok = false;
      return (batches > 1 ? L"第 " + std::to_wstring(b + 1) + L" 批失敗：" : L"") + error +
             (reviewed ? L"；先前完成的批次已套用（" + summary() + L"）" : L"");
    }
    // 推理模型（如 Qwen3）可能先輸出 <think>…</think>
    for (size_t p; (p = content.find(L"<think>")) != std::wstring::npos;) {
      const size_t e = content.find(L"</think>", p);
      content.erase(p, e == std::wstring::npos ? std::wstring::npos : e + 8 - p);
    }
    Log(L"LLM 回覆：\n" + content);
    const auto batch_removed = ApplyLLMResult(content, targets.size(), known, &merged);
    removed.insert(removed.end(), batch_removed.begin(), batch_removed.end());
    lexicon_->MarkReviewed(targets);
    reviewed += targets.size();
  }
  return summary();
}

std::wstring PersonalRefiner::RefineRimeMemory(const Config& config, bool full, bool* ok) {
  *ok = true;
  if (!user_dict_access_)
    return L"";
  RimeLeversApi* levers = nullptr;
  if (RimeModule* module = rime_get_api()->find_module("levers"))
    levers = (RimeLeversApi*)module->get_api();
  if (!levers)
    return L"選字記憶：無法使用 Rime 詞典管理";

  // 1. 匯出使用者詞典（要在輸入法放開詞典時做）
  // 匯出檔保留下來當備份（每次覆蓋）：刪錯時可以用詞典管理匯入還原
  const fs::path export_path = lexicon_->Dir() / L"rime_memory_backup.txt";
  int exported = -1;
  if (!user_dict_access_([&]() {
        exported = levers->export_user_dict(config.rime_user_dict.c_str(),
                                            wtou8(export_path.wstring()).c_str());
      }) ||
      exported < 0) {
    *ok = false;
    return L"選字記憶：匯出失敗";
  }

  // 每行：詞 \t 拼音 \t 次數；只看次數為正（沒被刪掉）、兩字以上的詞
  struct Entry {
    std::wstring text;
    std::string code;
  };
  std::vector<Entry> entries;
  {
    std::ifstream in(export_path, std::ios::binary);
    for (std::string line; std::getline(in, line);) {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      if (line.empty() || line[0] == '#')
        continue;
      const size_t t1 = line.find('\t');
      const size_t t2 = t1 == std::string::npos ? t1 : line.find('\t', t1 + 1);
      if (t2 == std::string::npos)
        continue;
      Entry e{u8tow(line.substr(0, t1)), line.substr(t1 + 1, t2 - t1 - 1)};
      if (atoi(line.c_str() + t2 + 1) <= 0)
        continue;
      size_t han = 0;
      for (wchar_t c : e.text)
        han += IsHan(c) ? 1 : 0;
      if (han >= 2 && han == e.text.size())
        entries.push_back(std::move(e));
    }
  }
  std::error_code ec;

  // 2. 還沒審查過的（重新精煉全部時全部重審）
  const fs::path reviewed_path = lexicon_->Dir() / L"rime_memory_reviewed.txt";
  std::unordered_set<std::string> reviewed;
  if (!full) {
    std::ifstream in(reviewed_path, std::ios::binary);
    for (std::string line; std::getline(in, line);)
      reviewed.insert(line);
  }
  std::vector<const Entry*> pool;
  for (const auto& e : entries) {
    if (!reviewed.count(wtou8(e.text) + "\t" + e.code))
      pool.push_back(&e);
  }
  if (pool.empty())
    return L"選字記憶：沒有需要審查的新詞";

  const bool local = config.type == "llamacpp";
  const size_t batch_size = local ? kMemoryBatchLocal : kMemoryBatchRemote;
  const size_t batches = (pool.size() + batch_size - 1) / batch_size;
  LLMLocalChatSession session;
  if (local) {
    LLMLocalModelSpec spec;
    spec.model_path = config.model_path;
    spec.instruct = config.instruct;
    spec.n_gpu_layers = config.n_gpu_layers;
    spec.n_threads = config.n_threads;
    spec.disable_thinking = config.disable_thinking;
    spec.think_tokens = config.think_tokens;
    std::wstring error;
    if (!session.Open(spec, &error)) {
      *ok = false;
      return L"選字記憶：" + error;
    }
  }

  // 3. 分批請 LLM 挑錯
  std::vector<const Entry*> to_delete;
  size_t reviewed_count = 0;
  std::ofstream reviewed_out(reviewed_path, full ? std::ios::binary | std::ios::trunc
                                                 : std::ios::binary | std::ios::app);
  for (size_t b = 0; b < batches && !stop_; ++b) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      progress_ = L"選字記憶 第 " + std::to_wstring(b + 1) + L"／" + std::to_wstring(batches) + L" 批";
    }
    WriteStatus();
    std::unordered_map<std::wstring, std::vector<const Entry*>> by_text;
    std::wostringstream user;
    user << L"輸入法記住的詞（詞\t拼音）：\n";
    size_t words = 0;
    for (size_t i = b * batch_size; i < pool.size() && i < (b + 1) * batch_size; ++i) {
      by_text[pool[i]->text].push_back(pool[i]);
      user << pool[i]->text << L"\t" << u8tow(pool[i]->code) << L"\n";
      ++words;
    }
    std::wstring content, error;
    bool batch_ok = true;
    if (local) {
      std::string output;
      batch_ok = session.Chat(wtou8(kRimeMemoryPrompt), wtou8(user.str()), 1024, &output, &error);
      content = u8tow(output);
    } else {
      content = CallRemote(config, user.str(), words, 0, &batch_ok, kRimeMemoryPrompt);
      if (!batch_ok)
        error = content;
    }
    if (!batch_ok) {
      *ok = false;
      break;
    }
    content = LLMStripThinking(content);
    Log(L"選字記憶 LLM 回覆：\n" + content);
    std::vector<std::wstring> flagged;
    std::wistringstream lines(content);
    for (std::wstring line; std::getline(lines, line);) {
      line = Trim(line);
      while (!line.empty() && (line[0] == L'-' || line[0] == L'*' || line[0] == L'•'))
        line = Trim(line.substr(1));
      if (line.rfind(L"刪除", 0) != 0)
        continue;
      const auto t = Tokens(line.substr(2));
      if (!t.empty() && by_text.count(t[0]) &&
          std::find(flagged.begin(), flagged.end(), t[0]) == flagged.end())
        flagged.push_back(t[0]);
    }
    // 保險：一批最多刪掉五分之一，避免模型失控清掉整份記憶
    const size_t max_removals = (std::max<size_t>)(5, words / 5);
    if (flagged.size() > max_removals)
      flagged.resize(max_removals);
    for (const auto& w : flagged)
      to_delete.insert(to_delete.end(), by_text[w].begin(), by_text[w].end());
    for (const auto& kv : by_text)
      for (const Entry* e : kv.second)
        reviewed_out << wtou8(e->text) << "\t" << e->code << "\n";
    reviewed_out.flush();
    reviewed_count += words;
  }
  reviewed_out.close();

  // 4. 刪除：匯入次數為負的項目，Rime 會把它們標記為已刪除
  if (!to_delete.empty()) {
    const fs::path import_path = lexicon_->Dir() / L"rime_memory_delete.txt";
    {
      std::ofstream out(import_path, std::ios::binary | std::ios::trunc);
      for (const Entry* e : to_delete)
        out << wtou8(e->text) << "\t" << e->code << "\t-1\n";
    }
    int imported = -1;
    user_dict_access_([&]() {
      imported = levers->import_user_dict(config.rime_user_dict.c_str(),
                                          wtou8(import_path.wstring()).c_str());
    });
    fs::remove(import_path, ec);
    if (imported < 0) {
      *ok = false;
      return L"選字記憶：刪除失敗";
    }
  }
  std::wstring sample;
  for (size_t i = 0; i < to_delete.size() && i < 8; ++i)
    sample += (sample.empty() ? L"" : L"、") + to_delete[i]->text;
  std::wstring result = L"選字記憶：審查 " + std::to_wstring(reviewed_count) + L" 個詞，刪除 " +
                        std::to_wstring(to_delete.size()) + L" 個";
  if (!sample.empty())
    result += L"（" + sample + (to_delete.size() > 8 ? L"…" : L"") + L"）";
  if (!*ok)
    result += L"；中途失敗，下次繼續";
  Log(result);
  return result;
}

std::wstring PersonalRefiner::CallRemote(const Config& config, const std::wstring& user,
                                         size_t words, size_t examples, bool* ok,
                                         const std::wstring& system) {
  // 答案：一批幾百個詞，要刪除或合併的清單可能很長，留 4096 token
  auto build = [&](bool no_think) {
    std::ostringstream body;
    body << "{\"model\":\"" << LLMJsonEscape(config.model) << "\","
         << "\"messages\":["
         << "{\"role\":\"system\",\"content\":\""
         << LLMJsonEscape(wtou8(system.empty() ? kSystemPrompt : system)) << "\"},"
         << "{\"role\":\"user\",\"content\":\"" << LLMJsonEscape(wtou8(user)) << "\"}"
         << "],\"temperature\":0.2,\"stream\":true";
    const int max_tokens = LLMTokenBudget(4096, !no_think, config.think_tokens);
    if (max_tokens >= 0)
      body << ",\"max_tokens\":" << max_tokens;
    if (no_think) {
      const std::string off = LLMDisableThinkingJson(config.api_url, config.model);
      if (!off.empty())
        body << "," << off;
    }
    body << "}";
    return body.str();
  };
  // 呼叫端設定的進度（例如「選字記憶 第 1／3 批」），串流狀況接在後面
  std::wstring base;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    base = progress_;
  }
  auto set_progress = [&](const std::wstring& state) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      progress_ = base.empty() ? state : base + L"：" + state;
    }
    WriteStatus();
  };

  bool no_think = config.disable_thinking;
  for (int attempt = 0;; ++attempt) {
    Log(L"送出精煉請求（串流）：" + std::to_wstring(words) + L" 個詞、" +
        std::to_wstring(examples) + L" 句例句" + (no_think ? L"（關閉思考）" : L"") + L" → " +
        u8tow(config.api_url));
    const ULONGLONG started = GetTickCount64();
    ULONGLONG last_report = 0;
    std::wstring content, reasoning;
    std::string finish, last_events;
    set_progress(L"等待模型回應");
    auto on_event = [&](const std::string& data) {
      bool found = false;
      const std::wstring c = LLMExtractJsonString(data, "content", &found);
      if (found)
        content += c;
      std::wstring r = LLMExtractJsonString(data, "reasoning_content", &found);
      if (!found)
        r = LLMExtractJsonString(data, "reasoning", &found);
      if (found)
        reasoning += r;
      const size_t f = data.find("\"finish_reason\":\"");
      if (f != std::string::npos) {
        const size_t e = data.find('"', f + 17);
        if (e != std::string::npos)
          finish = data.substr(f + 17, e - f - 17);
      }
      // 留最後幾個事件，失敗時存檔除錯
      last_events += data + "\n";
      if (last_events.size() > 16384)
        last_events.erase(0, last_events.size() - 16384);
      // 每秒更新一次進度（狀態檔與除錯主控台）
      const ULONGLONG now = GetTickCount64();
      if (now - last_report >= 1000) {
        last_report = now;
        const std::wstring secs = std::to_wstring((now - started) / 1000) + L" 秒";
        const size_t lines = std::count(content.begin(), content.end(), L'\n');
        set_progress(content.empty()
                         ? L"思考中 " + std::to_wstring(reasoning.size()) + L" 字（" + secs + L"）"
                         : L"回答中 " + std::to_wstring(lines) + L" 行（" + secs + L"）");
      }
      return !stop_.load();
    };
    unsigned long status = 0;
    bool timed_out = false;
    std::string error_body;
    const bool sent = LLMHttpPostStream(config.api_url, config.api_key, build(no_think), on_event,
                                        kTimeoutMs, &status, &timed_out, &error_body);
    const std::wstring elapsed = std::to_wstring((GetTickCount64() - started + 500) / 1000);
    Log(L"精煉回應結束（" + elapsed + L" 秒）：思考 " + std::to_wstring(reasoning.size()) +
        L" 字、回答 " + std::to_wstring(content.size()) + L" 字" +
        (finish.empty() ? L"" : L"，finish_reason=" + u8tow(finish)));
    if (stop_) {
      *ok = false;
      return L"已中止";
    }
    if (!sent && !timed_out) {
      *ok = false;
      if (status && !(status >= 200 && status < 300))
        return L"API 回應 HTTP " + std::to_wstring(status) + L" " +
               u8tow(error_body.substr(0, 200));
      return L"無法連線到 API";
    }
    const std::wstring answer = LLMStripThinking(content);
    if (!timed_out && !answer.empty())
      return answer;
    // 沒拿到答案：最後的事件留給除錯（回應裡沒有金鑰）
    {
      std::ofstream out(lexicon_->Dir() / L"refine_last_response.txt",
                        std::ios::binary | std::ios::trunc);
      out << last_events;
    }
    // 開著思考時多半是想太久：關閉思考再試一次（精煉只是挑錯字，不需要思考）
    if (!no_think && attempt == 0) {
      Log(timed_out ? L"模型 " + std::to_wstring(kTimeoutMs / 1000) + L" 秒沒有新內容，關閉思考重試"
                    : L"模型沒有給出答案，關閉思考重試");
      no_think = true;
      continue;
    }
    *ok = false;
    if (timed_out)
      return L"模型超過 " + std::to_wstring(kTimeoutMs / 1000) + L" 秒沒有新內容";
    if (finish == "length")
      return L"模型輸出被截斷（思考 " + std::to_wstring(reasoning.size()) +
             L" 字就用完輸出額度），沒有給出結果";
    return L"模型沒有給出結果，最後的回應見 refine_last_response.txt";
  }
}

std::vector<std::wstring> PersonalRefiner::ApplyLLMResult(
    const std::wstring& content, size_t batch_size, const std::unordered_set<std::wstring>& known,
    size_t* merged_count) {
  std::vector<std::wstring> removals;
  std::vector<std::pair<std::wstring, std::wstring>> merges;
  std::wistringstream lines(content);
  std::wstring line;
  while (std::getline(lines, line)) {
    line = Trim(line);
    // 去掉清單符號
    while (!line.empty() && (line[0] == L'-' || line[0] == L'*' || line[0] == L'•'))
      line = Trim(line.substr(1));
    if (line.rfind(L"刪除", 0) == 0) {
      const auto t = Tokens(line.substr(2));
      if (!t.empty() && known.count(t[0]))
        removals.push_back(t[0]);
    } else if (line.rfind(L"合併", 0) == 0) {
      const auto t = Tokens(line.substr(2));
      if (t.size() >= 2 && known.count(t[0]) && t[0] != t[1] && t[1].size() <= 40)
        merges.emplace_back(t[0], t[1]);
    }
  }
  // 保險：一批最多刪掉三分之一，避免模型失控把詞庫清空
  const size_t max_removals = std::max<size_t>(10, batch_size / 3);
  if (removals.size() > max_removals)
    removals.resize(max_removals);
  if (!removals.empty() || !merges.empty())
    lexicon_->ApplyRefinement(removals, merges);
  *merged_count += merges.size();
  return removals;
}

std::string PersonalRefiner::Method() const {
  if (!config_.UsesLLM())
    return "";
  const std::string name = config_.name.empty() ? std::string() : config_.name + " - ";
  if (config_.type == "llamacpp") {
    const std::wstring file = fs::path(u8tow(config_.model_path)).filename().wstring();
    return name + wtou8(L"本機模型 " + file);
  }
  return name + wtou8(L"OpenAI 相容 API ") + config_.model;
}

void PersonalRefiner::WriteStatusAs(bool running) {
  size_t archived_files = 0, archived_records = 0;
  for (const auto& file : lexicon_->ArchivedLogs()) {
    ++archived_files;
    archived_records += PersonalLexicon::CountRawRecords(file);
  }
  std::ostringstream out;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    out << "running=" << (running ? 1 : 0) << "\n"
        << "words=" << lexicon_->WordCount() << "\n"
        << "pairs=" << lexicon_->PairCount() << "\n"
        << "active_records=" << PersonalLexicon::CountRawRecords(lexicon_->ActiveLogPath())
        << "\n"
        << "archived_files=" << archived_files << "\n"
        << "archived_records=" << archived_records << "\n"
        << "last_refine=" << lexicon_->LastRefine() << "\n"
        << "interval_days=" << config_.interval_days << "\n"
        << "method=" << Method() << "\n"
        << "progress=" << wtou8(progress_) << "\n"
        << "rime_boost=" << (config_.rime_boost ? 1 : 0) << "\n"
        << "rime_words=" << rime_words_ << "\n"
        << "rime_updated=" << rime_updated_ << "\n"
        << "updated=" << Now() << "\n"
        << "last_result=" << wtou8(last_result_) << "\n";
  }
  const fs::path path = lexicon_->Dir() / L"status.txt";
  const fs::path tmp = path.wstring() + L".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f)
      return;
    const std::string s = out.str();
    f.write(s.data(), (std::streamsize)s.size());
  }
  MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}
