// WeaselDeployer.cpp : Defines the entry point for the application.
//
#include "stdafx.h"
#include <WeaselUtility.h>
#include <fstream>
#include "WeaselDeployer.h"
#include "Configurator.h"
#include "WebSettings.h"
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
      else if (arg == L"--size" && i + 2 < argc)
        options.width = _wtoi(argv[++i]), options.height = _wtoi(argv[++i]);
    }
    LocalFree(argv);
    const int result = RunWebSettings(&configurator, options);
    // 無法使用 WebView2：改開原本的設定視窗
    if (result == -1 && options.screenshot.empty())
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

  bool dict_management = !wcscmp(L"/dict", lpCmdLine);
  if (dict_management) {
    return configurator.DictManagement();
  }

  bool sync_user_dict = !wcscmp(L"/sync", lpCmdLine);
  if (sync_user_dict) {
    return configurator.SyncUserData();
  }

  bool installing = !wcscmp(L"/install", lpCmdLine);
  return configurator.Run(installing);
}
