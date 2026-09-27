// WeaselDeployer.cpp : Defines the entry point for the application.
//
#include "stdafx.h"
#include <WeaselUtility.h>
#include <fstream>
#include "WeaselDeployer.h"
#include "Configurator.h"
#include "WebSettings.h"
#include "SettingsDialog.h"
#include <ShellScalingApi.h>

CAppModule _Module;

static int Run(LPTSTR lpCmdLine);

int APIENTRY _tWinMain(HINSTANCE hInstance,
                       HINSTANCE hPrevInstance,
                       LPTSTR lpCmdLine,
                       int nCmdShow) {
  UNREFERENCED_PARAMETER(hPrevInstance);
  SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
  LANGID langId = get_language_id();
  SetThreadUILanguage(langId);
  SetThreadLocale(langId);

  HRESULT hRes = ::CoInitialize(NULL);
  // If you are running on NT 4.0 or higher you can use the following call
  // instead to make the EXE free threaded. This means that calls come in on a
  // random RPC thread.
  // HRESULT hRes = ::CoInitializeEx(NULL, COINIT_MULTITHREADED);
  ATLASSERT(SUCCEEDED(hRes));

  // this resolves ATL window thunking problem when Microsoft Layer for Unicode
  // (MSLU) is used
  ::DefWindowProc(NULL, 0, 0, 0L);

  AtlInitCommonControls(
      ICC_BAR_CLASSES);  // add flags to support other controls

  hRes = _Module.Init(NULL, hInstance);
  ATLASSERT(SUCCEEDED(hRes));

  CreateDirectory(WeaselUserDataPath().c_str(), NULL);

  int ret = 0;
  HANDLE hMutex = CreateMutex(NULL, TRUE, L"WeaselDeployerExclusiveMutex");
  if (!hMutex) {
    ret = 1;
  } else if (GetLastError() == ERROR_ALREADY_EXISTS) {
    ret = 1;
  } else {
    ret = Run(lpCmdLine);
  }

  if (hMutex) {
    CloseHandle(hMutex);
  }
  _Module.Term();
  ::CoUninitialize();

  return ret;
}

static int Run(LPTSTR lpCmdLine) {
  Configurator configurator;
  configurator.Initialize();

  // 去掉命令列頭尾的空白（有些啟動方式會在參數後面多加空白，例如 Windows PowerShell 5.1）
  std::wstring command_line = lpCmdLine ? lpCmdLine : L"";
  const size_t first = command_line.find_first_not_of(L" \t");
  command_line = first == std::wstring::npos
                     ? std::wstring()
                     : command_line.substr(first, command_line.find_last_not_of(L" \t") - first + 1);
  lpCmdLine = &command_line[0];

  if (!wcscmp(L"/?", lpCmdLine) || !wcscmp(L"/help", lpCmdLine)) {
    WCHAR msg[1024] = {0};
    if (LoadString(GetModuleHandle(NULL), IDS_STR_HELP, msg,
                   sizeof(msg) / sizeof(TCHAR))) {
      MessageBox(NULL, msg, L"Weasel Deployer", MB_ICONINFORMATION | MB_OK);
    } else {
      MessageBox(NULL,
                 L"Usage: WeaselDeployer.exe [options]\n"
                 L"/? or /help		- Show this help message\n"
                 L"/deploy		- Update Workspace\n"
                 L"/dict		- Manage dictionary\n"
                 L"/sync		- Sync user data\n"
                 L"/install		- Install Weasel (Initial deployment)",
                 L"Weasel Deployer", MB_ICONINFORMATION | MB_OK);
    }
    return 0;
  }

  // 設定視窗預設是網頁版；無法使用 WebView2 時改開原本的設定視窗
  auto settings = [&configurator](int page) {
    WebSettingsOptions options;
    options.start_page = page;
    const int result = RunWebSettings(&configurator, options);
    return result == -1 ? configurator.Run(false, page) : result;
  };

  // 原本的設定視窗（過渡期保留）
  if (!wcscmp(L"/legacy", lpCmdLine)) {
    return configurator.Run(false);
  }

  // 網頁版設定：/websettings [--page N] [--screenshot 檔案 --theme 1|2 --size 寬 高]
  if (!wcsncmp(L"/websettings", lpCmdLine, 12)) {
    WebSettingsOptions options;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
      const std::wstring arg = argv[i];
      if (arg == L"--page" && i + 1 < argc)
        options.start_page = _wtoi(argv[++i]);
      else if (arg == L"--screenshot" && i + 1 < argc)
        options.screenshot = argv[++i];
      else if (arg == L"--theme" && i + 1 < argc)
        options.theme = _wtoi(argv[++i]);
      else if (arg == L"--selftest" && i + 2 < argc)
        options.selftest = argv[++i], options.selftest_phase = argv[++i];
      else if (arg == L"--size" && i + 2 < argc)
        options.width = _wtoi(argv[++i]), options.height = _wtoi(argv[++i]);
    }
    LocalFree(argv);
    const int result = RunWebSettings(&configurator, options);
    // 無法使用 WebView2：改開原本的設定視窗
    if (result == -1 && options.screenshot.empty() && options.selftest.empty())
      return configurator.Run(false, options.start_page);
    return result;
  }

  bool deployment_scheduled = !wcscmp(L"/deploy", lpCmdLine);
  if (deployment_scheduled) {
    return configurator.UpdateWorkspace();
  }

  // 原本獨立的用戶詞典管理視窗
  if (!wcscmp(L"/dict_legacy", lpCmdLine)) {
    return configurator.LegacyDictManagement();
  }

  // 用戶詞典管理：網頁版設定的「詞庫管理」頁
  bool dict_management = !wcscmp(L"/dict", lpCmdLine);
  if (dict_management) {
    return settings(SettingsDialog::kPageDict);
  }

  bool sync_user_dict = !wcscmp(L"/sync", lpCmdLine);
  if (sync_user_dict) {
    return configurator.SyncUserData();
  }

  // 安裝時的第一次設定仍用原本的設定視窗（它依是否第一次執行決定要不要顯示）
  bool installing = !wcscmp(L"/install", lpCmdLine);
  if (installing)
    return configurator.Run(true);
  return settings(0);
}
