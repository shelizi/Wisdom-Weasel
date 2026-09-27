#pragma once

// 選字紀錄（llm/choice/log 開啟時）：每次送出一筆，加密附加到 personal/choice_log.dat
// （每筆前面是 4 位元組的長度）。
// 內容：時間 \t 應用程式 \t 方式 \t 前文 \t 注音 \t 預設轉換 \t 送出的文字
#include <cstdint>
#include <filesystem>
#include <string>

namespace ime {

struct ChoiceRecord {
  int64_t time = 0;
  std::string app;
  std::string method;  // ChoiceMethod()
  std::wstring context;
  std::wstring zhuyin;
  std::wstring default_text;  // 還沒換字前 Rime 的預設轉換
  std::wstring text;          // 送出的文字
};

// 這次是怎麼選出來的
const char* ChoiceMethod(bool mixed, bool correction, bool llm, bool focus, bool changed);

// 前文：送出之前的最近文字。有些路徑已先把這次送出的文字加進前文，去掉；最多 max 字
std::wstring ChoiceContext(std::wstring recent, const std::wstring& text, size_t max = 40);

// 一筆紀錄的明文（欄位裡的 tab 與換行換成空白）
std::string FormatChoiceRecord(const ChoiceRecord& record);

// 加密後附加到 personal_dir/choice_log.dat
bool AppendChoiceRecord(const std::filesystem::path& personal_dir, const ChoiceRecord& record);

}  // namespace ime
