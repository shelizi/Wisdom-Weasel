#pragma once

// 送出時的繁轉簡（OpenCC）：組字、候選、前文與個人詞庫照常是繁體（模型與統計都用繁體），
// 只有送進應用程式的文字轉成簡體。字典用 Rime 共用資料夾的 opencc（小狼毫、鼠鬚管都有附）。
#include <filesystem>
#include <mutex>
#include <string>

namespace ime {

class ScriptConverter {
 public:
  ScriptConverter() = default;
  ~ScriptConverter();
  ScriptConverter(const ScriptConverter&) = delete;
  ScriptConverter& operator=(const ScriptConverter&) = delete;

  // config：OpenCC 設定檔（例如 <共用資料>/opencc/tw2s.json）；同一個檔不重新載入。失敗回傳 false
  bool Load(const std::filesystem::path& config);
  bool Loaded() const;
  // 轉換失敗（或沒載入）時原樣回傳
  std::wstring Convert(const std::wstring& text) const;

 private:
  void Close();

  mutable std::mutex mutex_;
  void* converter_ = nullptr;  // opencc_t
  std::filesystem::path config_;
};

}  // namespace ime
