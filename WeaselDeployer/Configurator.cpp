#include "stdafx.h"
#include "WeaselDeployer.h"
#include "Configurator.h"
#include "SwitcherSettingsDialog.h"
#include "UIStyleSettings.h"
#include "UIStyleSettingsDialog.h"
#include "SettingsDialog.h"
#include "DictManagementDialog.h"
#include <WeaselConstants.h>
#include <WeaselIPC.h>
#include <WeaselIPCData.h>
#include <WeaselUtility.h>
#pragma warning(disable : 4005)
#include <rime_api.h>
#include <rime_levers_api.h>
#pragma warning(default : 4005)
#include <filesystem>
#include <fstream>
#include "WeaselDeployer.h"

static void CreateFileIfNotExist(std::string filename) {
  std::filesystem::path file_path = WeaselUserDataPath() / u8tow(filename);
  DWORD dwAttrib = GetFileAttributes(file_path.c_str());
  if (!(INVALID_FILE_ATTRIBUTES != dwAttrib &&
        0 == (dwAttrib & FILE_ATTRIBUTE_DIRECTORY))) {
    std::wofstream o(file_path.c_str(), std::ios::app);
    o.close();
  }
}
Configurator::Configurator() {
  CreateFileIfNotExist("default.custom.yaml");
  CreateFileIfNotExist("weasel.custom.yaml");
}

void Configurator::Initialize() {
  RIME_STRUCT(RimeTraits, weasel_traits);
  std::string shared_dir = wtou8(WeaselSharedDataPath().wstring());
  std::string user_dir = wtou8(WeaselUserDataPath().wstring());
  weasel_traits.shared_data_dir = shared_dir.c_str();
  weasel_traits.user_data_dir = user_dir.c_str();
  weasel_traits.prebuilt_data_dir = weasel_traits.shared_data_dir;
  std::string distribution_name = wtou8(get_weasel_ime_name());
  weasel_traits.distribution_name = distribution_name.c_str();
  weasel_traits.distribution_code_name = WEASEL_CODE_NAME;
  weasel_traits.distribution_version = WEASEL_VERSION;
  weasel_traits.app_name = "rime.weasel";
  std::string log_dir = WeaselLogPath().u8string();
  weasel_traits.log_dir = log_dir.c_str();
  RimeApi* rime_api = rime_get_api();
  assert(rime_api);
  rime_api->setup(&weasel_traits);
  LOG(INFO) << "WeaselDeployer reporting.";
  rime_api->deployer_initialize(NULL);
}

int Configurator::Run(bool installing, int start_page) {
  RimeModule* levers = rime_get_api()->find_module("levers");
  if (!levers)
    return 1;
  RimeLeversApi* api = (RimeLeversApi*)levers->get_api();
  if (!api)
    return 1;

  RimeSwitcherSettings* switcher_settings = api->switcher_settings_init();
  UIStyleSettings ui_style_settings;

  // 安裝時只有首次執行才顯示設定視窗（與原本逐一詢問的行為一致）
  const bool skip_settings =
      installing &&
      !api->is_first_run((RimeCustomSettings*)switcher_settings) &&
      !api->is_first_run(ui_style_settings.settings());

  // 輸入方案、外觀、LLM 智慧預測合併在同一個視窗；套用時在視窗內直接重新部署
  bool deployed = false;
  if (!skip_settings &&
      api->load_settings((RimeCustomSettings*)switcher_settings) &&
      api->load_settings(ui_style_settings.settings())) {
    SettingsDialog dialog(switcher_settings, &ui_style_settings,
                          [this] { UpdateWorkspace(true); });
    dialog.SetStartPage(start_page);
    dialog.DoModal();
    deployed = dialog.deployed();
  }

  api->custom_settings_destroy((RimeCustomSettings*)switcher_settings);

  if (installing && !deployed) {
    return UpdateWorkspace(false);
  }
  return 0;
}

int Configurator::UpdateWorkspace(bool report_errors) {
  HANDLE hMutex = CreateMutex(NULL, TRUE, L"WeaselDeployerMutex");
  if (!hMutex) {
    LOG(ERROR) << "Error creating WeaselDeployerMutex.";
    return 1;
  }
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    LOG(WARNING) << "another deployer process is running; aborting operation.";
    CloseHandle(hMutex);
    if (report_errors) {
      // MessageBox(NULL,
      // L"正在執行另一項部署任務，方纔所做的修改將在輸入法再次啓動後生效。",
      // L"【小狼毫】", MB_OK | MB_ICONINFORMATION);
      MSG_BY_IDS(IDS_STR_DEPLOYING_RESTARTREQ, IDS_STR_WEASEL,
                 MB_OK | MB_ICONINFORMATION);
    }
    return 1;
  }

  weasel::Client client;
  if (client.Connect()) {
    LOG(INFO) << "Turning WeaselServer into maintenance mode.";
    client.StartMaintenance();
  }

  {
    RimeApi* rime = rime_get_api();
    // initialize default config, preset schemas
    rime->deploy();
    // initialize weasel config
    rime->deploy_config_file("weasel.yaml", "config_version");
  }

  CloseHandle(hMutex);  // should be closed before resuming service.

  if (client.Connect()) {
    LOG(INFO) << "Resuming service.";
    client.EndMaintenance();
  }
  return 0;
}

int Configurator::DictManagement() {
  // 使用者詞典管理已整合到設定視窗的「詞庫管理」頁（操作時才讓輸入法暫停）
  return Run(false, SettingsDialog::kPageDict);
}

// 原本獨立的「用戶詞典管理」視窗（保留備用）
int Configurator::LegacyDictManagement() {
  HANDLE hMutex = CreateMutex(NULL, TRUE, L"WeaselDeployerMutex");
  if (!hMutex) {
    LOG(ERROR) << "Error creating WeaselDeployerMutex.";
    return 1;
  }
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    LOG(WARNING) << "another deployer process is running; aborting operation.";
    CloseHandle(hMutex);
    // MessageBox(NULL, L"正在執行另一項部署任務，請稍候再試。", L"【小狼毫】",
    // MB_OK | MB_ICONINFORMATION);
    MSG_BY_IDS(IDS_STR_DEPLOYING_WAIT, IDS_STR_WEASEL,
               MB_OK | MB_ICONINFORMATION);
    return 1;
  }

  weasel::Client client;
  if (client.Connect()) {
    LOG(INFO) << "Turning WeaselServer into maintenance mode.";
    client.StartMaintenance();
  }

  {
    RimeApi* rime = rime_get_api();
    if (RIME_API_AVAILABLE(rime, run_task)) {
      rime->run_task("installation_update");  // setup user data sync dir
    }
    DictManagementDialog dlg;
    dlg.DoModal();
  }

  CloseHandle(hMutex);  // should be closed before resuming service.

  if (client.Connect()) {
    LOG(INFO) << "Resuming service.";
    client.EndMaintenance();
  }
  return 0;
}

int Configurator::SyncUserData() {
  HANDLE hMutex = CreateMutex(NULL, TRUE, L"WeaselDeployerMutex");
  if (!hMutex) {
    LOG(ERROR) << "Error creating WeaselDeployerMutex.";
    return 1;
  }
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    LOG(WARNING) << "another deployer process is running; aborting operation.";
    CloseHandle(hMutex);
    // MessageBox(NULL, L"正在執行另一項部署任務，請稍候再試。", L"【小狼毫】",
    // MB_OK | MB_ICONINFORMATION);
    MSG_BY_IDS(IDS_STR_DEPLOYING_WAIT, IDS_STR_WEASEL,
               MB_OK | MB_ICONINFORMATION);
    return 1;
  }

  weasel::Client client;
  if (client.Connect()) {
    LOG(INFO) << "Turning WeaselServer into maintenance mode.";
    client.StartMaintenance();
  }

  {
    RimeApi* rime = rime_get_api();
    if (!rime->sync_user_data()) {
      LOG(ERROR) << "Error synching user data.";
      CloseHandle(hMutex);
      return 1;
    }
    rime->join_maintenance_thread();
  }

  CloseHandle(hMutex);  // should be closed before resuming service.

  if (client.Connect()) {
    LOG(INFO) << "Resuming service.";
    client.EndMaintenance();
  }
  return 0;
}
