#pragma once

// 推薦（llm/choice/rescore）：用本機模型比較同音字整句的通順度
#include <string>
#include <vector>

class LLMProvider;

namespace ime {

// 找出更通順的同音字整句：先算目前整句每個字的機率，只在最不通順的幾個位置試同音字；
// 比原句好超過門檻（log 機率）才推薦。units 每個音節一個字，homophones 是各位置的同音字。
// 沒有更好的就回傳空字串；還在拼的注音與之後的內容照原樣接在推薦後面
std::wstring RescoreSentence(LLMProvider* scorer,
                             const std::wstring& context,
                             const std::vector<std::wstring>& units,
                             const std::vector<std::vector<std::wstring>>& homophones);

}  // namespace ime
