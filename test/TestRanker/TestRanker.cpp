// 學習式排序器的煙霧測試：模型檔 Load/Score 來回、ApplyRanker 的決策覆寫
#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>

#include "../../core/ime/ranker.h"

namespace {

int failures = 0;
void Check(bool ok, const char* what) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what);
  } else {
    std::printf("ok %s\n", what);
  }
}

const char* kLinearJson = R"({
  "version": 1, "type": "linear", "margin": 0.5,
  "features": ["lm","lm_pc","lm_gap","pos","chars","personal","pers_gap","formula","form_gap","ctx","zy"],
  "linear": {"mean": [0,0,0,0,0,0,0,0,0,0,0], "std": [1,1,1,1,1,1,1,1,1,1,1],
             "w": [0,0,1,0,0,0,0,0,0,0,0], "b": 0}
})";

const char* kGbdtJson = R"({
  "version": 1, "type": "gbdt", "margin": 0.3,
  "features": ["lm","lm_pc","lm_gap","pos","chars","personal","pers_gap","formula","form_gap","ctx","zy"],
  "gbdt": {"init": 0.1, "lr": 1.0,
    "trees": [[{"f":2,"t":0.5,"l":1,"r":2,"v":0},
               {"f":-1,"t":0,"l":-1,"r":-1,"v":-0.2},
               {"f":-1,"t":0,"l":-1,"r":-1,"v":0.8}]]}
})";

}  // namespace

int main() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path();
  // 線性：分數只看 lm_gap（第 3 個特徵）
  const auto lin_path = dir / "ranker_lin.json";
  { std::ofstream o(lin_path); o << kLinearJson; }
  ime::RankerModel lin;
  Check(lin.Load(lin_path), "linear load");
  Check(lin.type() == "linear" && lin.margin() == 0.5, "linear fields");

  const auto gbdt_path = dir / "ranker_gbdt.json";
  { std::ofstream o(gbdt_path); o << kGbdtJson; }
  ime::RankerModel gbdt;
  Check(gbdt.Load(gbdt_path), "gbdt load");

  // 特徵：lm_gap=0.8 → 線性分數 0.8；GBDT：0.1 + 1.0*0.8（走右子樹）= 0.9
  double f[ime::kRankerFeatures] = {0};
  f[2] = 0.8;
  Check(std::abs(lin.Score(f) - 0.8) < 1e-9, "linear score");
  Check(std::abs(gbdt.Score(f) - 0.9) < 1e-9, "gbdt score");
  f[2] = 0.1;
  Check(std::abs(gbdt.Score(f) - (0.1 - 0.2)) < 1e-9, "gbdt left leaf");
  f[0] = std::numeric_limits<double>::quiet_NaN();
  Check(std::isnan(lin.Score(f)), "NaN propagates");

  // 壞檔：特徵名不符要拒絕
  const auto bad_path = dir / "ranker_bad.json";
  {
    std::ofstream o(bad_path);
    o << R"({"version":1,"type":"linear","margin":0,"features":["x"],"linear":{"mean":[],"std":[],"w":[],"b":0}})";
  }
  ime::RankerModel bad;
  Check(!bad.Load(bad_path), "reject bad features");

  // ApplyRanker：第二句分數較高且過門檻 → 推薦第二句
  ime::RerankResult r;
  r.texts = {L"今天天氣", L"今天天汽"};
  r.lm_scores = {-10.0, -9.0};
  r.scores = {-10.0, -9.0};
  ime::RerankOptions opt;
  opt.personal = [](const std::wstring&) { return 0.0; };
  ime::ApplyRanker(&lin, 5, L"ㄐㄧㄣ ㄊㄧㄢ", opt, &r);
  // 線性看 lm_gap：第二句 1.0 > margin 0.5 → 推薦
  Check(r.index == 1 && r.text == L"今天天汽", "ranker recommends");
  Check(std::abs(r.scores[1] - 1.0) < 1e-9, "scores replaced");

  // 門檻沒過：gbdt margin 0.3，分數差 0.1-(-0.2)... 第二句 lm_gap=1 → 0.9 vs 0.1 → 0.8 > 0.3 也推薦
  // 換個不過門檻的：lm 分數一樣 → gap 0 → 不推薦
  r = ime::RerankResult();
  r.texts = {L"今天天氣", L"今天天汽"};
  r.lm_scores = {-10.0, -10.0};
  r.scores = {-10.0, -10.0};
  ime::ApplyRanker(&lin, 5, L"ㄐㄧㄣ ㄊㄧㄢ", opt, &r);
  Check(r.text.empty() && r.index == 0, "no change below margin");

  // 沒模型：fail-open 不動結果
  r = ime::RerankResult();
  r.texts = {L"a", L"b"};
  r.lm_scores = {0, 1};
  r.scores = {0, 1};
  r.text = L"b";  // 公式已推薦
  ime::ApplyRanker(nullptr, 0, L"", opt, &r);
  Check(r.text == L"b", "null model keeps result");

  std::printf(failures ? "%d failed\n" : "all passed\n", failures);
  return failures ? 1 : 0;
}
