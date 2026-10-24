#pragma once

// 每個輸入 session 在 Rime 之外的狀態（混打、注音預覽、逐字選字、這次組字的統計）
#include <rime_api.h>

#include <limits>
#include <string>
#include <vector>

namespace ime {

struct SessionState {
  struct ManualChoice {
    size_t start = 0;
    std::string keys;
    std::wstring text;
  };
  RimeSessionId session_id = 0;
  std::string manual_input;
  std::string manual_schema;
  std::vector<ManualChoice> manual_choices;
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
  // 這次組字套用過的建議（"predict"／"recommend"／"correction"／"rerank"，
  // 有差距時是 "kind:gain"；選字紀錄用）
  std::string applied_suggestion;
  // 信心校準：目前顯示中的推薦／校正比原句好多少（沒有是 NaN）；送出或選了候選時記成一筆樣本
  double recommend_gain = std::numeric_limits<double>::quiet_NaN();
  double correction_gain = std::numeric_limits<double>::quiet_NaN();
  double rerank_gain = std::numeric_limits<double>::quiet_NaN();
  // 整句重排的 shadow 模式：最後一次的結果（不顯示），送出時和使用者送出的句子對照
  bool shadow_pending = false;
  // 這段組字最後一次整句重排評過的 Rime 整句與分數（選字紀錄用；組字結束清掉）
  std::vector<std::wstring> shown_sentences;
  std::vector<double> shown_scores;
  // 顯示中的整句重排推薦的特徵（校準樣本附記，給之後的多特徵校準）
  std::string rerank_features;
  bool shadow_would_change = false;  // 過了門檻，顯示模式會推薦
  std::wstring shadow_first;         // Rime 第一句
  std::wstring shadow_best;          // 分數最高的另一句（沒有是空字串）
  double shadow_gain = std::numeric_limits<double>::quiet_NaN();
  // 選字紀錄：還沒換字前 Rime 的預設轉換（整句）與對應的注音
  std::wstring default_text;
  std::wstring default_zhuyin;
  std::string default_input;  // default_text 是哪一串按鍵的轉換（之後又改了輸入就不能用）
  // 注音逐字選字（像新注音）：←/→ 框住的字（音節序號），-1 表示沒有框選
  int focus_hl = 0;  // 框選時反白停的位置（目前顯示的字），換到別的才算換字
  int focus = -1;
  std::string focus_input;  // 框選時的輸入與游標，改變了就取消框選
  size_t focus_caret = 0;
  // 沒有框選、游標停在句首。Rime 的游標放在句首會整句重新轉換，所以仍留在句尾，
  // 真的在句首打字時才移過去；輸入或游標變了（focus_input／focus_caret）就取消
  bool at_start = false;
};

}  // namespace ime
