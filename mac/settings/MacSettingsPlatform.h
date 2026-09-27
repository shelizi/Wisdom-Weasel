#pragma once

// 設定後端的 macOS 平台功能。設定頁和鼠鬚管在同一個行程裡執行：
// 部署、個人詞庫指令、模型測試直接交給鼠鬚管（Hooks），不需要程式間通訊。
// 注意：尚未在 macOS 上編譯驗證。
#include <functional>

#include "../../core/settings/platform.h"

class MacSettingsPlatform : public settings::Platform {
 public:
  struct Hooks {
    std::function<void(unsigned command)> personal_command;  // ime::Controller::PersonalCommand
    std::function<void()> llm_test;                          // ime::Controller::LLMTest
    std::function<void()> start_maintenance;                 // 暫停輸入（操作使用者詞典前）
    std::function<void()> end_maintenance;
    std::function<void()> deploy;                            // 重新部署並等完成
  };

  explicit MacSettingsPlatform(Hooks hooks) : hooks_(std::move(hooks)) {}

  settings::fs::path UserDataDir() override;
  settings::fs::path ModelsDir() override;
  settings::fs::path FromRimePath(const char* path) override;

  bool SendPersonalCommand(unsigned command) override;
  bool NotifyLLMTest() override;
  void StartMaintenance() override;
  void EndMaintenance() override;
  void Deploy() override;
  bool InstallSchemas(std::string* error) override;

  settings::fs::path OpenFileDialog(void* owner, const std::string& title,
                                    const std::vector<settings::FileFilter>& filters,
                                    const std::string& file_name, const std::string& default_ext) override;
  settings::fs::path SaveFileDialog(void* owner, const std::string& title,
                                    const std::vector<settings::FileFilter>& filters,
                                    const std::string& file_name, const std::string& default_ext) override;
  void Reveal(const settings::fs::path& file) override;
  void OpenFolder(const settings::fs::path& dir) override;
  bool MoveToTrash(const settings::fs::path& file) override;
  bool FileInUse(const settings::fs::path& file) override;
  std::vector<std::string> SystemFonts() override;

  int GetPreference(const std::string& key, int fallback) override;
  void SetPreference(const std::string& key, int value) override;

 private:
  Hooks hooks_;
};
