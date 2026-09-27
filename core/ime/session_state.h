#pragma once

// 每個輸入 session 在 Rime 之外的狀態（混打、注音預覽、逐字選字、這次組字的統計）
#include <rime_api.h>

#include <string>
#include <vector>

namespace ime {

struct SessionState {
  RimeSessionId session_id = 0;
  // last fully converted preview (preedit_type: preview), one unit per
  // syllable, to keep showing the text after the caret while selecting
  std::string preview_input;
  std::vector<std::wstring> preview_units;
  std::vector<size_t> preview_lens;  // 每個字對應的按鍵數（Rime 的音節切法）
  // 中英混打：組字中按 Shift 切到英文後，已轉好的中文與打的英文暫存在這裡，
  // 顯示在組字區最前面，Enter 一起送出（沒在組字時切英文照舊直接輸出）
  std::wstring mixed_text;
  bool mixed_english = false;  // 正在混打的英文段
  std::wstring mixed_commit;   // 待送出的混打內容
  bool mixed_active() const { return mixed_english || !mixed_text.empty(); }
  // 選字統計：這次組字有沒有換過候選、有沒有出現 LLM 候選（送出時計入）
  bool choice_changed = false;
  bool llm_offered = false;
  bool focus_used = false;         // 這次組字用過逐字選字
  bool recommend_offered = false;  // 這次組字出現過「推薦」
  bool llm_committed = false;      // 這次送出的是 LLM 候選
  bool correction_committed = false;
  // 選字紀錄：還沒換字前 Rime 的預設轉換（整句）與對應的注音
  std::wstring default_text;
  std::wstring default_zhuyin;
  // 注音逐字選字（像新注音）：←/→ 框住的字（音節序號），-1 表示沒有框選
  int focus_hl = 0;  // 框選時反白停的位置（目前顯示的字），換到別的才算換字
  int focus = -1;
  std::string focus_input;  // 框選時的輸入與游標，改變了就取消框選
  size_t focus_caret = 0;
};

}  // namespace ime
