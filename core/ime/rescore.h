#pragma once

// 推薦（llm/choice/rescore）：用本機模型比較同音字整句的通順度
#include <string>
#include <vector>

class LLMProvider;

namespace ime {

// 搜尋的範圍與門檻。預設值由 test/TuneChoice 調出來：選字紀錄上 Rime 的初稿已經很準，
// 門檻越高、亂改越少；公開題目上門檻到 8 開始漏掉真的錯字，4 在兩個模型上都是最好或並列最好
struct RescoreOptions {
  double margin = 4.0;      // 單一位置換字要比原句好超過這麼多（log 機率）才算
  size_t positions = 4;     // 最多檢查幾個最不通順的位置
  size_t alternatives = 3;  // 每個位置最多試幾個同音字（常用的前幾個就夠）
};

// 找出更通順的同音字整句：先算目前整句每個字的機率，只在最不通順的幾個位置試同音字；
// 比原句好超過門檻（log 機率）才推薦。units 每個音節一個字，homophones 是各位置的同音字。
// 沒有更好的就回傳空字串；還在拼的注音與之後的內容照原樣接在推薦後面。
// gain：推薦比原句好多少（整句 log 機率差，給信心校準用）
std::wstring RescoreSentence(LLMProvider* scorer,
                             const std::wstring& context,
                             const std::vector<std::wstring>& units,
                             const std::vector<std::vector<std::wstring>>& homophones,
                             double* gain = nullptr,
                             const RescoreOptions& options = RescoreOptions());

}  // namespace ime
