#pragma once

// 設定後端的 Windows 平台功能：登錄檔、Shell、檔案對話框、DirectWrite、與小狼毫服務的 IPC
#include "../core/settings/platform.h"

class Configurator;

class WinSettingsPlatform : public settings::Platform {
 public:
  // configurator 用來重新部署；舊的設定視窗不重新部署，可以是 nullptr
  explicit WinSettingsPlatform(Configurator* configurator) : configurator_(configurator) {}

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

  // 舊的設定視窗（不重新部署）共用的實例
  static WinSettingsPlatform& Shared();

 private:
  Configurator* configurator_;
};
