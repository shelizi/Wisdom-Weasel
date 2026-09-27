#pragma once

// 網頁版設定的後端：網頁送來 {方法, 參數}，這裡回傳 JSON 結果。
// 介面邏輯（分頁、表單、模型設定的增刪與驗證）都在網頁裡；這裡只讀寫 Rime 設定、
// 改寫方案檔、與輸入法溝通、處理檔案與下載。
#include "../core/third_party/nlohmann/json.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

class Configurator;

class SettingsBackend {
 public:
  using json = nlohmann::json;
  // 推送事件給網頁（下載進度等）；任何執行緒都可以呼叫
  using Emit = std::function<void(const std::string& event, const json& data)>;

  // 方法在哪裡執行：
  // kUi   視窗的執行緒（需要擁有者視窗的對話框：選檔、選字型）
  // kRime Rime 的工作執行緒，依序執行（levers API 與部署不能同時呼叫）
  // kTask 各自的背景執行緒（等待輸入法回覆、連線測試、讀檔）
  enum class Thread { kUi, kRime, kTask };

  SettingsBackend(Configurator* configurator, Emit emit);
  ~SettingsBackend();

  static Thread ThreadOf(const std::string& method);
  // 失敗時丟出 Error，訊息直接顯示給使用者
  json Call(const std::string& method, const json& params, void* owner);

  struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
  };

 private:
  struct State;
  std::unique_ptr<State> s_;
};
