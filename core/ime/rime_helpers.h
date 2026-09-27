#pragma once

// 只用 librime API 的輸入法小工具（Windows 與 mac 共用）
#include <rime_api.h>

#include <map>
#include <string>
#include <vector>

#include "ZhuyinPreview.h"

namespace ime {

// 方案的拼寫設定（組字預覽用）：speller/finals、alphabet、delimiter 與 preedit_format 的 xlit
ZhuyinSpeller LoadZhuyinSpeller(RimeApi* api, const char* schema_id);

// 送出目前的組字並取回轉換好的文字（不交給應用程式）
std::wstring TakeComposition(RimeApi* api, RimeSessionId session_id);

// 把組字整句改成 units（每個音節一個字）：從頭逐段挑符合、最長的候選來選，
// 送出時 Rime 會照常學習。選不到就恢復原本的整句並回傳 false
bool SelectText(RimeApi* api,
                RimeSessionId session_id,
                const std::string& input,
                const std::vector<std::wstring>& units);

// 查同音字（單字候選）：用一個背景 session，只查候選、不送出，不會學習
class HomophoneFinder {
 public:
  explicit HomophoneFinder(RimeApi* api) : api_(api) {}
  // schema 方案裡 keys（一個音節的按鍵）的同音字，最多 6 個
  std::vector<std::wstring> Find(const std::string& schema, const std::string& keys);
  // 重新部署後詞典可能變了
  void ClearCache() { cache_.clear(); }

 private:
  RimeApi* api_;
  RimeSessionId session_ = 0;
  std::string schema_;
  std::map<std::string, std::vector<std::wstring>> cache_;  // 方案 \t 按鍵 → 同音字
};

}  // namespace ime
