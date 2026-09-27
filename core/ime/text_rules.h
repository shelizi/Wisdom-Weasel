#pragma once

// 輸入法的文字規則：哪些送出的文字值得預測下一個詞、清洗模型給的候選與校正、合併候選
#include <string>
#include <vector>

namespace ime {

// 分隔符（空白與中英文標點）
bool IsSeparatorOrPunctuation(wchar_t ch);

// 至少有一個不是分隔符的字：只打標點或空白時不進入預測
bool HasMeaningfulContent(const std::wstring& text);

// 清洗模型給的候選：只留第一行，去掉控制字元與首尾的引號、括號、標點，去重、丟掉空的
// （小模型常模仿 prompt 裡的「…」格式，多路取樣也常給出相同結果）。
// prefix 非空時是打字中的補全：候選 = Rime 目前的轉換結果 + 續寫
std::vector<std::wstring> CleanCandidates(const std::vector<std::wstring>& raw,
                                          const std::wstring& prefix);

// 整句校正的結果：去掉首尾空白、引號與句末標點；和初稿相同（不必校正）
// 或字數差太多（模型沒照做）時回傳空字串
std::wstring CleanCorrection(const std::wstring& raw, const std::wstring& draft);

// 依序合併、去重，最多 max 個（對應 Tab、Shift+2~5）
std::vector<std::wstring> MergeCandidates(std::vector<std::wstring> merged,
                                          const std::vector<std::wstring>& more,
                                          size_t max = 5);

}  // namespace ime
