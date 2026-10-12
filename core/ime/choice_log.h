#pragma once

// 選字紀錄（llm/choice/log 開啟時）：每次送出一筆，加密附加到 personal/choice_log.dat
// （每筆前面是 4 位元組的長度）。
// 內容（13 欄，後面的欄位沒有資料是空字串）：
//   時間(毫秒) \t 應用程式 \t 方式 \t 前文 \t 注音 \t 預設轉換 \t 送出的文字
//   \t Rime 當時的整句候選（U+001E 分隔，最多 20 句，第一句是預設轉換）
//   \t 它們的模型分數（逗號分隔；只評過前面幾句，沒評的沒有分數）
//   \t 每句的個人詞庫分數（逗號分隔） \t 顯示中的建議（kind:gain 逗號分隔）
//   \t 套用的建議（kind；沒有是空） \t 前文視窗 key
// 整句候選重排沒開時一樣會記（只查不評分）；送出後很快在應用程式裡刪字會再記一筆
// 方式 = reverted 的紀錄（欄位同原本那筆）——那是「剛才送出的是錯的」的訊號。
// 這是使用者自己的 Rime（有選字記憶）的 Top-K，離線評估重排與日後訓練排序器用。
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ime {

struct ChoiceRecord {
  int64_t time = 0;             // 毫秒（epoch）
  std::string app;
  std::string method;  // ChoiceMethod()；送出後馬上刪除的補記是 "reverted"
  std::wstring context;
  std::wstring zhuyin;
  std::wstring default_text;  // 還沒換字前 Rime 的預設轉換
  std::wstring text;          // 送出的文字
  std::vector<std::wstring> shown;  // Rime 的整句候選（第一句是預設轉換）；查不到是空的
  std::vector<double> shown_scores;  // 對應的模型分數（整句 log 機率；只有前面幾句評過）
  std::vector<double> shown_personal;  // 對應的個人詞庫分數（PersonalLexicon::SentenceScore）
  std::string rec;            // 顯示中沒採用的建議："rerank:2.34" 等，逗號分隔 kind:gain
  std::string took;           // 套用的建議 "kind:gain"（SessionState::applied_suggestion；沒有是空）
  std::wstring context_key;   // 前文視窗 key（同一份前文把多次送出串起來）
};

constexpr wchar_t kChoiceListSeparator = L'\x1E';

// 這次是怎麼選出來的
const char* ChoiceMethod(bool mixed, bool correction, bool llm, bool focus, bool changed);

// 前文：送出之前的最近文字。有些路徑已先把這次送出的文字加進前文，去掉；最多 max 字
std::wstring ChoiceContext(std::wstring recent, const std::wstring& text, size_t max = 40);

// 一筆紀錄的明文（欄位裡的 tab 與換行換成空白）
std::string FormatChoiceRecord(const ChoiceRecord& record);

// 加密後附加到 personal_dir/choice_log.dat
bool AppendChoiceRecord(const std::filesystem::path& personal_dir, const ChoiceRecord& record);

}  // namespace ime
