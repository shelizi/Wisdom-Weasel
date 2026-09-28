#include "stdafx.h"
#include "WinSettingsPlatform.h"
#include "Configurator.h"
#include <WeaselIPC.h>
#include <WeaselUtility.h>
#include <algorithm>
#include <dwrite.h>
#include <shlobj.h>
#include <shobjidl.h>
#pragma comment(lib, "dwrite.lib")

namespace fs = std::filesystem;

namespace {

const wchar_t kRegKey[] = L"Software\\Rime\\Weasel";

template <typename T, typename U>
fs::path DoFileDialog(HWND owner,
                      const std::string& title,
                      const std::vector<settings::FileFilter>& filters,
                      const std::string& file_name,
                      const std::string& def_ext) {
  std::vector<std::wstring> names, specs;
  for (const auto& f : filters) {
    names.push_back(u8tow(f.name));
    specs.push_back(u8tow(f.spec));
  }
  std::vector<COMDLG_FILTERSPEC> items;
  for (size_t i = 0; i < filters.size(); ++i)
    items.push_back({names[i].c_str(), specs[i].c_str()});
  fs::path path;
  CoInitialize(NULL);
  {
    CComPtr<T> dialog;
    if (SUCCEEDED(dialog.CoCreateInstance(__uuidof(U)))) {
      dialog->SetFileTypes((UINT)items.size(), items.data());
      dialog->SetTitle(u8tow(title).c_str());
      if (!file_name.empty())
        dialog->SetFileName(u8tow(file_name).c_str());
      dialog->SetDefaultExtension(u8tow(def_ext).c_str());
      if (SUCCEEDED(dialog->Show(owner))) {
        CComPtr<IShellItem> result;
        if (SUCCEEDED(dialog->GetResult(&result))) {
          wchar_t* name;
          if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &name))) {
            path = name;
            CoTaskMemFree(name);
          }
        }
      }
    }
  }
  CoUninitialize();
  return path;
}

}  // namespace

WinSettingsPlatform& WinSettingsPlatform::Shared() {
  static WinSettingsPlatform platform(nullptr);
  return platform;
}

fs::path WinSettingsPlatform::UserDataDir() {
  return WeaselUserDataPath();
}

fs::path WinSettingsPlatform::ModelsDir() {
  wchar_t profile[MAX_PATH] = {0};
  GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
  return fs::path(profile) / L"models";
}

fs::path WinSettingsPlatform::FromRimePath(const char* path) {
  // librime 在 Windows 上以系統字碼頁回傳路徑
  wchar_t wide[MAX_PATH * 4] = {0};
  MultiByteToWideChar(CP_ACP, 0, path ? path : "", -1, wide, _countof(wide));
  return fs::path(wide);
}

bool WinSettingsPlatform::SendPersonalCommand(unsigned command) {
  weasel::Client client;
  if (!client.Connect())
    return false;
  client.PersonalCommand(command);
  return true;
}

bool WinSettingsPlatform::NotifyLLMTest() {
  weasel::Client client;
  if (!client.Connect())
    return false;
  client.LLMTestRequest();
  return true;
}

void WinSettingsPlatform::StartMaintenance() {
  weasel::Client client;
  if (client.Connect())
    client.StartMaintenance();
}

void WinSettingsPlatform::EndMaintenance() {
  weasel::Client client;
  if (client.Connect())
    client.EndMaintenance();
}

void WinSettingsPlatform::Deploy() {
  if (configurator_)
    configurator_->UpdateWorkspace(true);
}

bool WinSettingsPlatform::InstallSchemas(std::string* error) {
  // 執行安裝資料夾裡的 rime-install.bat（東風破），等它結束
  HKEY key;
  const std::wstring reg = is_wow64() ? L"Software\\WOW6432Node\\Rime\\Weasel"
                                      : L"Software\\Rime\\Weasel";
  wchar_t root[MAX_PATH] = {0};
  DWORD len = sizeof(root), type = 0;
  bool found = false;
  if (RegOpenKeyW(HKEY_LOCAL_MACHINE, reg.c_str(), &key) == ERROR_SUCCESS) {
    found = RegQueryValueExW(key, L"WeaselRoot", NULL, &type, (LPBYTE)root,
                             &len) == ERROR_SUCCESS &&
            type == REG_SZ;
    RegCloseKey(key);
  }
  if (!found) {
    *error = "找不到小狼毫的安裝資料夾。";
    return false;
  }
  const std::wstring parameters =
      std::wstring(L"/k \"") + root + L"\\rime-install.bat\"";
  SHELLEXECUTEINFOW cmd = {sizeof(cmd)};
  cmd.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
  cmd.lpVerb = L"open";
  cmd.lpFile = L"cmd";
  cmd.lpParameters = parameters.c_str();
  cmd.nShow = SW_SHOW;
  if (ShellExecuteExW(&cmd) && cmd.hProcess) {
    WaitForSingleObject(cmd.hProcess, INFINITE);
    CloseHandle(cmd.hProcess);
  }
  return true;
}

fs::path WinSettingsPlatform::OpenFileDialog(
    void* owner,
    const std::string& title,
    const std::vector<settings::FileFilter>& filters,
    const std::string& file_name,
    const std::string& default_ext) {
  return DoFileDialog<IFileOpenDialog, FileOpenDialog>(
      (HWND)owner, title, filters, file_name, default_ext);
}

fs::path WinSettingsPlatform::SaveFileDialog(
    void* owner,
    const std::string& title,
    const std::vector<settings::FileFilter>& filters,
    const std::string& file_name,
    const std::string& default_ext) {
  return DoFileDialog<IFileSaveDialog, FileSaveDialog>(
      (HWND)owner, title, filters, file_name, default_ext);
}

void WinSettingsPlatform::Reveal(const fs::path& file) {
  const std::wstring filepath = fs::path(file).make_preferred().wstring();
  const std::wstring directory = fs::path(filepath).parent_path().wstring();
  CoInitializeEx(0, COINIT_MULTITHREADED);
  ITEMIDLIST* folder = ILCreateFromPath(directory.c_str());
  LPITEMIDLIST item = ILCreateFromPath(filepath.c_str());
  LPCITEMIDLIST items[] = {item};
  SHOpenFolderAndSelectItems(folder, 1, items, 0);
  ILFree(item);
  ILFree(folder);
  CoUninitialize();
}

void WinSettingsPlatform::OpenFolder(const fs::path& dir) {
  ShellExecuteW(NULL, L"open", dir.c_str(), NULL, NULL, SW_SHOWNORMAL);
}

bool WinSettingsPlatform::MoveToTrash(const fs::path& file) {
  std::wstring from = file.wstring();
  from.push_back(L'\0');  // SHFileOperation 需要雙 NUL 結尾
  SHFILEOPSTRUCTW op = {0};
  op.wFunc = FO_DELETE;
  op.pFrom = from.c_str();
  op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
  return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

bool WinSettingsPlatform::FileInUse(const fs::path& file) {
  HANDLE h = CreateFileW(file.c_str(), DELETE, 0, NULL, OPEN_EXISTING, 0, NULL);
  if (h == INVALID_HANDLE_VALUE)
    return true;
  CloseHandle(h);
  return false;
}

std::vector<std::string> WinSettingsPlatform::SystemFonts() {
  std::vector<std::string> fonts;
  wchar_t locale[LOCALE_NAME_MAX_LENGTH] = {0};
  GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH);
  CComPtr<IDWriteFactory> factory;
  CComPtr<IDWriteFontCollection> collection;
  if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                 __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(&factory))) ||
      FAILED(factory->GetSystemFontCollection(&collection)))
    return fonts;
  for (UINT32 i = 0; i < collection->GetFontFamilyCount(); ++i) {
    CComPtr<IDWriteFontFamily> family;
    CComPtr<IDWriteLocalizedStrings> names;
    if (FAILED(collection->GetFontFamily(i, &family)) ||
        FAILED(family->GetFamilyNames(&names)))
      continue;
    UINT32 index = 0;
    BOOL exists = FALSE;
    if (FAILED(names->FindLocaleName(locale, &index, &exists)) || !exists)
      index = 0;  // 沒有這個語系的名稱：用第一個
    UINT32 length = 0;
    if (FAILED(names->GetStringLength(index, &length)))
      continue;
    std::wstring name(length + 1, L'\0');
    if (FAILED(names->GetString(index, &name[0], length + 1)))
      continue;
    name.resize(length);
    fonts.push_back(wtou8(name));
  }
  std::sort(fonts.begin(), fonts.end());
  fonts.erase(std::unique(fonts.begin(), fonts.end()), fonts.end());
  return fonts;
}

int WinSettingsPlatform::GetPreference(const std::string& key, int fallback) {
  DWORD value = 0, size = sizeof(value);
  if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, u8tow(key).c_str(),
                   RRF_RT_REG_DWORD, NULL, &value, &size) != ERROR_SUCCESS)
    return fallback;
  return (int)value;
}

void WinSettingsPlatform::SetPreference(const std::string& key, int value) {
  DWORD v = (DWORD)value;
  RegSetKeyValueW(HKEY_CURRENT_USER, kRegKey, u8tow(key).c_str(), REG_DWORD, &v,
                  sizeof(v));
}
