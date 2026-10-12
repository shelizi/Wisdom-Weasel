#include "auto_tune.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>

#include "../third_party/nlohmann/json.hpp"

namespace settings_rerank {

namespace {

using nlohmann::json;

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::filesystem::path HistoryPath(const std::filesystem::path& dir) {
  return dir / "tune_history.log";
}
std::filesystem::path StatePath(const std::filesystem::path& dir) {
  return dir / "auto_tune.json";
}

// 「使用者還要動手修」的送出方式：送錯反悔（reverted）或自己換過字（changed/focus）
bool NeedsFix(const std::string& method) {
  return method == "reverted" || method == "changed" || method == "focus";
}

double FixRate(const std::vector<LoggedMeta>& meta, size_t begin, size_t end, size_t* n) {
  size_t fix = 0, total = 0;
  for (size_t i = begin; i < end && i < meta.size(); ++i) {
    fix += NeedsFix(meta[i].method);
    ++total;
  }
  if (n)
    *n = total;
  return total ? (double)fix / total : 0;
}

// 排序網格上朝 target 走一格；target 不在格上時朝它的方向走一格
template <typename T>
T StepToward(const std::vector<T>& grid, T cur, T target) {
  if (cur == target)
    return cur;
  // cur 不在格上：先進格（取最接近的一格），再往 target 走一格
  auto it = std::lower_bound(grid.begin(), grid.end(), cur);
  size_t i = it == grid.end() ? grid.size() - 1 : (size_t)(it - grid.begin());
  if (it != grid.end() && i > 0 && *it > cur &&
      (double)(*it - cur) > (double)(cur - grid[i - 1]))
    --i;  // 前一格比較近
  if (target > cur && i + 1 < grid.size())
    return grid[i + 1];
  if (target < cur && i > 0)
    return grid[i - 1];
  return grid[i];
}

}  // namespace

std::vector<TuneEntry> LoadTuneHistory(const std::filesystem::path& personal_dir) {
  std::vector<TuneEntry> out;
  std::ifstream in(HistoryPath(personal_dir));
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty())
      continue;
    try {
      const json j = json::parse(line);
      TuneEntry e;
      e.ts = j.value("ts", (int64_t)0);
      e.action = j.value("action", "");
      e.top = j.value("top", (size_t)0);
      e.margin = j.value("margin", 0.0);
      e.prev_top = j.value("prev_top", (size_t)0);
      e.prev_margin = j.value("prev_margin", 0.0);
      e.cases = j.value("cases", (size_t)0);
      e.acc_prev = j.value("acc_prev", 0.0);
      e.acc_best = j.value("acc_best", 0.0);
      e.wrong_next = j.value("wrong_next", 0.0);
      e.apply_rec_ms = j.value("apply_rec_ms", (int64_t)0);
      e.note = j.value("note", "");
      out.push_back(std::move(e));
    } catch (...) {
    }
  }
  return out;
}

bool AppendTuneHistory(const std::filesystem::path& personal_dir, const TuneEntry& e) {
  std::ofstream out(HistoryPath(personal_dir), std::ios::app);
  if (!out)
    return false;
  out << json{{"ts", e.ts},
              {"action", e.action},
              {"top", e.top},
              {"margin", e.margin},
              {"prev_top", e.prev_top},
              {"prev_margin", e.prev_margin},
              {"cases", e.cases},
              {"acc_prev", e.acc_prev},
              {"acc_best", e.acc_best},
              {"wrong_next", e.wrong_next},
              {"apply_rec_ms", e.apply_rec_ms},
              {"note", e.note}}
            .dump()
      << "\n";
  return true;
}

AutoState LoadAutoState(const std::filesystem::path& personal_dir) {
  AutoState s;
  std::ifstream in(StatePath(personal_dir));
  if (!in)
    return s;
  try {
    const json j = json::parse(std::string((std::istreambuf_iterator<char>(in)),
                                           std::istreambuf_iterator<char>()));
    s.enabled = j.value("enabled", false);
    s.last_run_ms = j.value("last_run_ms", (int64_t)0);
    s.last_records = j.value("last_records", (size_t)0);
    s.cooldown_until_ms = j.value("cooldown_until_ms", (int64_t)0);
  } catch (...) {
  }
  return s;
}

bool SaveAutoState(const std::filesystem::path& personal_dir, const AutoState& s) {
  std::ofstream out(StatePath(personal_dir), std::ios::trunc);
  if (!out)
    return false;
  out << json{{"enabled", s.enabled},
              {"last_run_ms", s.last_run_ms},
              {"last_records", s.last_records},
              {"cooldown_until_ms", s.cooldown_until_ms}}
            .dump(2);
  return true;
}

DegradeCheck CheckDegradation(const std::vector<LoggedMeta>& meta, int64_t apply_rec_ms,
                              size_t min_after) {
  DegradeCheck c;
  if (apply_rec_ms <= 0 || meta.empty())
    return c;
  // 套用點以後的紀錄（時間 > apply_rec_ms）
  size_t after_begin = meta.size();
  for (size_t i = 0; i < meta.size(); ++i) {
    if (meta[i].time > apply_rec_ms) {
      after_begin = i;
      break;
    }
  }
  size_t after_n = 0;
  c.after_rate = FixRate(meta, after_begin, meta.size(), &after_n);
  c.after_n = after_n;
  if (after_n < min_after)
    return c;  // 套用後的資料還不夠下判斷
  c.judgeable = true;
  // 套用點之前取同樣大小的窗口當基準
  const size_t before_n = std::min(after_n, after_begin);
  c.before_rate = FixRate(meta, after_begin - before_n, after_begin, nullptr);
  // 修正率顯著上升才算劣化：至少 +5 個百分點且是原本 1.5 倍以上
  c.degraded = c.after_rate > c.before_rate + 0.05 && c.after_rate > c.before_rate * 1.5;
  return c;
}

GuardedDecision DecideGuarded(const std::vector<ScoredCase>& items,
                              size_t cur_top, double cur_margin,
                              double alpha, double personal_weight) {
  GuardedDecision d;
  d.current = EvalParams(items, cur_top, cur_margin, alpha, personal_weight);
  if (items.empty()) {
    d.reason = "no cases";
    return d;
  }
  // 錯改率上限：不準比現在更常把對的改掉（至少留 0.5pp 或現在值的 25% 緩衝）
  const double wrong_cap =
      d.current.wrong_change + (std::max)(0.005, d.current.wrong_change * 0.25);
  // 網格最佳：要真的比現在準（至少多答對一筆），且錯改在預算內
  const double min_gain = 1.0 / items.size() + 1e-9;
  size_t max_n = 0;
  for (const auto& it : items)
    max_n = (std::max)(max_n, it.lm.size());
  bool found = false;
  for (size_t top : TopGrid()) {
    if (top < 2 || top > max_n)
      continue;
    for (double margin : MarginGrid()) {
      TuneOutcome o = EvalParams(items, top, margin, alpha, personal_weight);
      if (o.wrong_change > wrong_cap)
        continue;
      const bool better = !found || o.accuracy > d.best.accuracy + 1e-9 ||
                          (o.accuracy > d.best.accuracy - 1e-9 &&
                           (margin > d.best.margin ||
                            (margin == d.best.margin && top < d.best.top)));
      if (better) {
        d.best = o;
        found = true;
      }
    }
  }
  if (!found) {
    d.reason = "no grid cell within wrong-change budget";
    return d;
  }
  d.best_top = d.best.top;
  d.best_margin = d.best.margin;
  if (d.best.accuracy < d.current.accuracy + min_gain) {
    d.reason = "current params already near optimum";
    return d;
  }
  // 漸進：每維朝網格最佳走一格（不在格上的值先就近進格）
  d.top = StepToward(TopGrid(), cur_top, d.best.top);
  d.margin = StepToward(MarginGrid(), cur_margin, d.best.margin);
  d.applied = EvalParams(items, d.top, d.margin, alpha, personal_weight);
  if (d.top == cur_top && d.margin == cur_margin) {
    d.reason = "already at grid neighbor of best";
    return d;
  }
  // 走一步後也不能比現在差（單步可能跨過頭）
  if (d.applied.accuracy < d.current.accuracy - 1e-9 ||
      d.applied.wrong_change > wrong_cap) {
    d.reason = "one step would regress";
    return d;
  }
  d.apply = true;
  d.reason = "step toward best";
  return d;
}

AutoRunResult AutoTuneRun(LLMProvider* scorer,
                          const std::filesystem::path& personal_dir,
                          size_t cur_top, double cur_margin,
                          size_t context_chars, double alpha, double personal_weight,
                          const std::atomic<bool>& cancel,
                          const std::function<void(size_t, size_t)>& progress) {
  AutoRunResult r;
  r.action = "skip";
  std::vector<LoggedMeta> meta;
  LoadLoggedMeta(personal_dir, &meta);
  r.records = meta.size();

  // 1) 上次有套用的話先看有沒有劣化
  const auto history = LoadTuneHistory(personal_dir);
  const TuneEntry* last_apply = nullptr;
  for (auto it = history.rbegin(); it != history.rend() && !last_apply; ++it)
    if (it->action == "apply")
      last_apply = &*it;
  if (last_apply)
    r.check = CheckDegradation(meta, last_apply->apply_rec_ms);
  if (r.check.degraded) {
    // 回滾到上一組；之後冷卻一陣子不再自動調
    r.action = "rollback";
    r.top = last_apply->prev_top;
    r.margin = last_apply->prev_margin;
    r.entry.ts = NowMs();
    r.entry.action = "rollback";
    r.entry.top = r.top;
    r.entry.margin = r.margin;
    r.entry.prev_top = last_apply->top;
    r.entry.prev_margin = last_apply->margin;
    r.entry.apply_rec_ms = meta.empty() ? 0 : meta.back().time;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "修正率 %.1f%% → %.1f%%（%zu 筆後段紀錄）",
                  r.check.before_rate * 100, r.check.after_rate * 100, r.check.after_n);
    r.entry.note = buf;
    r.message = std::string("偵測到套用後修正率上升，回復到評 ") + std::to_string(r.top) +
                    " 句、門檻 " + std::to_string(r.margin);
    AppendTuneHistory(personal_dir, r.entry);
    return r;
  }

  // 2) 評分＋防護決策
  std::vector<LoggedCase> cases;
  LoadLoggedTopK(personal_dir, &cases);
  r.entry.cases = cases.size();
  // 無人看管的套用要比手動（30 筆）保守：資料不夠就跳過這輪
  if (cases.size() < 100) {
    r.action = "skip";
    r.message = "有整句候選的紀錄 " + std::to_string(cases.size()) + " 筆（自動調整至少 100 筆）";
    return r;
  }
  const auto items = ScoreCases(scorer, cases, context_chars, cancel, progress);
  if (cancel) {
    r.action = "skip";
    r.message = "已取消";
    return r;
  }
  r.decision = DecideGuarded(items, cur_top, cur_margin, alpha, personal_weight);
  r.entry.ts = NowMs();
  r.entry.prev_top = cur_top;
  r.entry.prev_margin = cur_margin;
  r.entry.acc_prev = r.decision.current.accuracy;
  r.entry.acc_best = r.decision.best.accuracy;
  r.entry.apply_rec_ms = meta.empty() ? 0 : meta.back().time;
  if (r.decision.apply) {
    r.action = "apply";
    r.top = r.decision.top;
    r.margin = r.decision.margin;
    r.entry.action = "apply";
    r.entry.top = r.top;
    r.entry.margin = r.margin;
    r.entry.wrong_next = r.decision.applied.wrong_change;
    char buf[200];
    std::snprintf(buf, sizeof(buf),
                  "準確率 %.1f%% → 一步後 %.1f%%（網格最佳 %.1f%%：評 %zu 句、門檻 %g）",
                  r.decision.current.accuracy * 100, r.decision.applied.accuracy * 100,
                  r.decision.best.accuracy * 100, r.decision.best.top, r.decision.best.margin);
    r.entry.note = buf;
    r.message = std::string("建議調整為評 ") + std::to_string(r.top) + " 句、門檻 " +
                std::to_string(r.margin) + "（" + buf + "）";
  } else {
    r.action = "hold";
    r.entry.action = "hold";
    r.entry.top = cur_top;
    r.entry.margin = cur_margin;
    r.entry.note = r.decision.reason;
    r.message = "維持目前參數（" + r.decision.reason + "）";
  }
  AppendTuneHistory(personal_dir, r.entry);
  return r;
}

}  // namespace settings_rerank
