#include "stdafx.h"
#include "PersonalRefiner.h"
#include "DevConsole.h"
#include "LLMProvider.h"
#include <WeaselUtility.h>
#include <algorithm>
#include <chrono>
#include <fstream>
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
  stop_ = false;
  scheduler_ = std::thread([this]() { SchedulerLoop(); });
  WriteStatus();
}

void PersonalRefiner::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
    cv_.notify_all();
  }
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
  worker_ = std::thread([this, full]() { Run(full); });
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

std::wstring PersonalRefiner::CallRemote(const Config& config, const std::wstring& user,
                                         size_t words, size_t examples, bool* ok) {
  std::ostringstream body;
  body << "{\"model\":\"" << LLMJsonEscape(config.model) << "\","
       << "\"messages\":["
       << "{\"role\":\"system\",\"content\":\"" << LLMJsonEscape(wtou8(kSystemPrompt)) << "\"},"
       << "{\"role\":\"user\",\"content\":\"" << LLMJsonEscape(wtou8(user)) << "\"}"
       << "],\"temperature\":0.2,\"max_tokens\":2048,\"stream\":false}";

  Log(L"送出精煉請求：" + std::to_wstring(words) + L" 個詞、" + std::to_wstring(examples) +
      L" 句例句 → " + u8tow(config.api_url));
  std::string response;
  unsigned long status = 0;
  if (!LLMHttpPostJson(config.api_url, config.api_key, body.str(), &response, kTimeoutMs,
                       &status)) {
    *ok = false;
    if (status)
      return L"API 回應 HTTP " + std::to_wstring(status) + L" " +
             u8tow(response.substr(0, 200));
    return L"無法連線到 API";
  }
  bool found = false;
  const std::wstring content = LLMExtractChatContent(response, &found);
  if (!found) {
    *ok = false;
    return L"API 回應格式無法解析";
  }
  return content;
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
