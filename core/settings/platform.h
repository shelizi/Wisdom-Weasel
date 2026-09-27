#pragma once

// 設定後端需要的平台功能。每個前端各一份實作：Windows（小狼毫，WeaselDeployer）、macOS（鼠鬚管）。
// 字串一律 UTF-8；路徑用 std::filesystem::path。
#include <filesystem>
#include <string>
#include <vector>

namespace settings {

namespace fs = std::filesystem;

struct FileFilter {
  std::string name;  // 例如「文字文件 (*.txt)」
  std::string spec;  // 例如「*.txt」
};

class Platform {
 public:
  virtual ~Platform() = default;

  // --- 資料夾 ---
  virtual fs::path UserDataDir() = 0;   // Rime 使用者資料夾
  virtual fs::path ModelsDir() = 0;     // 模型檔（GGUF）資料夾
  // librime 回傳的路徑字串（Windows 是系統字碼頁，macOS 是 UTF-8）
  virtual fs::path FromRimePath(const char* path) = 0;

  // --- 與輸入法溝通 ---
  // 個人詞庫指令：1 更新狀態、2 精煉、3 全部重新精煉、4 清除、5 匯出詞彙、6 套用修改、
  // 7 產生注音排序詞典、8 清除統計。連不上輸入法時回傳 false
  virtual bool SendPersonalCommand(unsigned command) = 0;
  // 預測測試：請求檔已寫好，通知輸入法去讀
  virtual bool NotifyLLMTest() = 0;
  // 暫停／恢復輸入法（操作使用者詞典時）
  virtual void StartMaintenance() = 0;
  virtual void EndMaintenance() = 0;
  // 重新部署並讓輸入法載入新設定（會等部署完成）
  virtual void Deploy() = 0;
  // 取得更多輸入方案（東風破）；會等安裝程式結束
  virtual bool InstallSchemas(std::string* error) = 0;

  // --- 檔案與桌面 ---
  // 對話框：owner 是視窗代碼（沒有時為 nullptr）；取消時回傳空路徑
  virtual fs::path OpenFileDialog(void* owner, const std::string& title,
                                  const std::vector<FileFilter>& filters, const std::string& file_name,
                                  const std::string& default_ext) = 0;
  virtual fs::path SaveFileDialog(void* owner, const std::string& title,
                                  const std::vector<FileFilter>& filters, const std::string& file_name,
                                  const std::string& default_ext) = 0;
  virtual void Reveal(const fs::path& file) = 0;       // 在檔案管理員顯示並選取
  virtual void OpenFolder(const fs::path& dir) = 0;
  virtual bool MoveToTrash(const fs::path& file) = 0;  // 資源回收筒／垃圾桶
  virtual bool FileInUse(const fs::path& file) = 0;    // 被輸入法載入中（無法刪除）
  virtual std::vector<std::string> SystemFonts() = 0;  // 字型家族名稱

  // --- 設定視窗自己的偏好（例如主題），不是輸入法設定 ---
  virtual int GetPreference(const std::string& key, int fallback) = 0;
  virtual void SetPreference(const std::string& key, int value) = 0;
};

}  // namespace settings
