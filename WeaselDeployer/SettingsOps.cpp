#include "stdafx.h"
#include "SettingsOps.h"
#include <PersonalCrypto.h>
#include <WeaselIPC.h>
#include <WeaselUtility.h>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <winhttp.h>
#include <dwrite.h>
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "dwrite.lib")

namespace settings_ops {

const wchar_t kDefaultApiUrl[] = L"https://api.openai.com/v1/chat/completions";

// ---------------------------------------------------------------------------
// 文字與路徑

std::wstring ToLower(std::wstring s) {
  std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
  return s;
}

std::wstring FileNameOf(const std::wstring& path) {
  return fs::path(path).filename().wstring();
}

std::wstring Trim(const std::wstring& s) {
  const wchar_t* ws = L" \t\r\n　";
  const size_t b = s.find_first_not_of(ws);
  return b == std::wstring::npos ? std::wstring() : s.substr(b, s.find_last_not_of(ws) - b + 1);
}

std::wstring FormatSize(unsigned long long bytes) {
  wchar_t buf[32];
  if (bytes >= (1ULL << 30))
    swprintf_s(buf, L"%.2f GB", bytes / 1073741824.0);
  else
    swprintf_s(buf, L"%.0f MB", bytes / 1048576.0);
  return buf;
}

std::wstring FormatTime(int64_t t) {
  if (t <= 0)
    return L"—";
  FILETIME ft;
  const ULONGLONG ticks = (ULONGLONG)t * 10000000ULL + 116444736000000000ULL;
  ft.dwLowDateTime = (DWORD)ticks;
  ft.dwHighDateTime = (DWORD)(ticks >> 32);
  FILETIME local;
  SYSTEMTIME st;
  if (!FileTimeToLocalFileTime(&ft, &local) || !FileTimeToSystemTime(&local, &st))
    return L"—";
  wchar_t buf[32];
  swprintf_s(buf, L"%04d/%02d/%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour,
             st.wMinute);
  return buf;
}

std::wstring LoadedModelDisplay(const std::wstring& model) {
  if (model.empty())
    return model;
  if (ToLower(fs::path(model).extension().wstring()) == L".gguf")
    return FileNameOf(model);
  return model;
}

const wchar_t* GuessModelType(const std::wstring& path) {
  const std::wstring name = ToLower(FileNameOf(path));
  if (name.find(L"base") != std::wstring::npos)
    return L"Base";
  if (name.find(L"instruct") != std::wstring::npos || name.find(L"chat") != std::wstring::npos ||
      name.find(L"-it") != std::wstring::npos)
    return L"Instruct";
  return nullptr;
}

// ---------------------------------------------------------------------------
// 檔案總管與檔案對話框

void OpenFolderAndSelectItem(std::wstring filepath) {
  filepath = fs::path(filepath).make_preferred().wstring();
  std::wstring directory = fs::path(filepath).parent_path();
  CoInitializeEx(0, COINIT_MULTITHREADED);
  ITEMIDLIST* folder = ILCreateFromPath(directory.c_str());
  std::vector<LPITEMIDLIST> v;
  v.push_back(ILCreateFromPath(filepath.c_str()));
  SHOpenFolderAndSelectItems(folder, (UINT)v.size(), (LPCITEMIDLIST*)v.data(), 0);
  for (auto idl : v)
    ILFree(idl);
  ILFree(folder);
  CoUninitialize();
}

namespace {

template <typename T, typename U>
std::wstring DoFileDialog(HWND owner, const std::wstring& title,
                          const std::vector<FileFilter>& filters, const wchar_t* filename,
                          const wchar_t* def_ext) {
  std::vector<COMDLG_FILTERSPEC> specs;
  for (const auto& f : filters)
    specs.push_back({f.name.c_str(), f.spec.c_str()});
  std::wstring path;
  CoInitialize(NULL);
  {
    CComPtr<T> dialog;
    if (SUCCEEDED(dialog.CoCreateInstance(__uuidof(U)))) {
      dialog->SetFileTypes((UINT)specs.size(), specs.data());
      dialog->SetTitle(title.c_str());
      if (filename)
        dialog->SetFileName(filename);
      dialog->SetDefaultExtension(def_ext);
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

std::wstring OpenFileDialog(void* owner, const std::wstring& title,
                            const std::vector<FileFilter>& filters, const wchar_t* file_name,
                            const wchar_t* def_ext) {
  return DoFileDialog<IFileOpenDialog, FileOpenDialog>((HWND)owner, title, filters, file_name,
                                                       def_ext);
}

std::wstring SaveFileDialog(void* owner, const std::wstring& title,
                            const std::vector<FileFilter>& filters, const wchar_t* file_name,
                            const wchar_t* def_ext) {
  return DoFileDialog<IFileSaveDialog, FileSaveDialog>((HWND)owner, title, filters, file_name,
                                                       def_ext);
}

bool MoveToRecycleBin(const std::wstring& path) {
  std::wstring from = path;
  from.push_back(L'\0');  // SHFileOperation 需要雙 NUL 結尾
  SHFILEOPSTRUCTW op = {0};
  op.wFunc = FO_DELETE;
  op.pFrom = from.c_str();
  op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
  return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

std::vector<std::wstring> ListSystemFonts() {
  std::vector<std::wstring> fonts;
  wchar_t locale[LOCALE_NAME_MAX_LENGTH] = {0};
  GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH);
  CComPtr<IDWriteFactory> factory;
  CComPtr<IDWriteFontCollection> collection;
  if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(&factory))) ||
      FAILED(factory->GetSystemFontCollection(&collection)))
    return fonts;
  for (UINT32 i = 0; i < collection->GetFontFamilyCount(); ++i) {
    CComPtr<IDWriteFontFamily> family;
    CComPtr<IDWriteLocalizedStrings> names;
    if (FAILED(collection->GetFontFamily(i, &family)) || FAILED(family->GetFamilyNames(&names)))
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
    fonts.push_back(name);
  }
  std::sort(fonts.begin(), fonts.end());
  fonts.erase(std::unique(fonts.begin(), fonts.end()), fonts.end());
  return fonts;
}

// ---------------------------------------------------------------------------
// 模型檔

fs::path ModelsDir() {
  wchar_t profile[MAX_PATH] = {0};
  GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
  return fs::path(profile) / L"models";
}

bool IsGguf(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  char magic[4] = {0};
  return in.read(magic, 4) && memcmp(magic, "GGUF", 4) == 0;
}

std::vector<std::wstring> ScanModels(const std::wstring& current) {
  std::vector<fs::path> dirs;
  if (!current.empty())
    dirs.push_back(fs::path(current).parent_path());
  dirs.push_back(ModelsDir());
  std::vector<std::wstring> found;
  std::set<std::wstring> seen;
  for (const auto& dir : dirs) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
      if (entry.is_regular_file(ec) && ToLower(entry.path().extension().wstring()) == L".gguf" &&
          seen.insert(ToLower(entry.path().wstring())).second)
        found.push_back(entry.path().wstring());
    }
  }
  std::sort(found.begin(), found.end(), [](const std::wstring& a, const std::wstring& b) {
    return ToLower(FileNameOf(a)) < ToLower(FileNameOf(b));
  });
  return found;
}

std::vector<ModelFile> ListModelFiles() {
  std::vector<ModelFile> files;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(ModelsDir(), ec)) {
    if (entry.is_regular_file(ec) && ToLower(entry.path().extension().wstring()) == L".gguf")
      files.push_back({entry.path().wstring(), (unsigned long long)fs::file_size(entry.path(), ec)});
  }
  std::sort(files.begin(), files.end(), [](const ModelFile& a, const ModelFile& b) {
    return ToLower(FileNameOf(a.path)) < ToLower(FileNameOf(b.path));
  });
  return files;
}

std::wstring NormalizeModelUrl(std::wstring url, std::wstring* file_name) {
  url = Trim(url);
  const size_t blob = url.find(L"/blob/");
  if (url.find(L"huggingface.co/") != std::wstring::npos && blob != std::wstring::npos)
    url.replace(blob, 6, L"/resolve/");
  std::wstring path = url.substr(0, url.find_first_of(L"?#"));
  *file_name = path.substr(path.find_last_of(L'/') + 1);
  // 百分比編碼的檔名（例如 %2B）
  wchar_t decoded[MAX_PATH] = {0};
  DWORD len = MAX_PATH;
  if (SUCCEEDED(UrlUnescapeW((LPWSTR)file_name->c_str(), decoded, &len, 0)))
    *file_name = decoded;
  return url;
}

bool HttpDownload(const std::wstring& url, const fs::path& dest, const Progress& progress,
                  std::wstring* error) {
  URL_COMPONENTS uc = {0};
  uc.dwStructSize = sizeof(uc);
  wchar_t host[256] = {0};
  wchar_t path[4096] = {0};
  uc.lpszHostName = host;
  uc.dwHostNameLength = _countof(host);
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = _countof(path);
  wchar_t extra[4096] = {0};
  uc.lpszExtraInfo = extra;
  uc.dwExtraInfoLength = _countof(extra);
  if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc)) {
    *error = L"網址格式不正確";
    return false;
  }
  HINTERNET session = WinHttpOpen(L"Weasel Deployer", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session)
    session = WinHttpOpen(L"Weasel Deployer", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    *error = L"無法建立連線";
    return false;
  }
  WinHttpSetTimeouts(session, 30000, 30000, 30000, 60000);
  const std::wstring object = std::wstring(path) + extra;
  HINTERNET connect = WinHttpConnect(session, std::wstring(host, uc.dwHostNameLength).c_str(),
                                     uc.nPort, 0);
  HINTERNET request =
      connect ? WinHttpOpenRequest(connect, L"GET", object.c_str(), NULL, WINHTTP_NO_REFERER,
                                   WINHTTP_DEFAULT_ACCEPT_TYPES,
                                   uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
              : NULL;
  bool ok = false;
  if (!request) {
    *error = L"無法連線到伺服器";
  } else if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA,
                                 0, 0, 0) ||
             !WinHttpReceiveResponse(request, NULL)) {
    *error = L"連線失敗（錯誤 " + std::to_wstring(GetLastError()) + L"）";
  } else {
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    ULONGLONG total = 0;
    size = sizeof(total);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER64,
                        WINHTTP_HEADER_NAME_BY_INDEX, &total, &size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
      *error = L"伺服器回應 HTTP " + std::to_wstring(status) +
               (status == 401 || status == 403 ? L"（可能需要登入或同意授權條款）" : L"");
    } else {
      std::ofstream out(dest, std::ios::binary | std::ios::trunc);
      std::vector<char> buf(1 << 20);
      ULONGLONG done = 0;
      ok = !!out;
      if (!ok)
        *error = L"無法寫入檔案";
      while (ok) {
        DWORD read = 0;
        if (!WinHttpReadData(request, buf.data(), (DWORD)buf.size(), &read)) {
          *error = L"下載中斷（錯誤 " + std::to_wstring(GetLastError()) + L"）";
          ok = false;
          break;
        }
        if (read == 0)
          break;
        out.write(buf.data(), read);
        done += read;
        if (!progress(done, total)) {
          *error = L"已取消";
          ok = false;
        }
      }
      if (ok && total && done != total) {
        *error = L"下載不完整";
        ok = false;
      }
    }
  }
  if (request)
    WinHttpCloseHandle(request);
  if (connect)
    WinHttpCloseHandle(connect);
  WinHttpCloseHandle(session);
  return ok;
}

namespace {

DWORD CALLBACK CopyProgress(LARGE_INTEGER total, LARGE_INTEGER done, LARGE_INTEGER, LARGE_INTEGER,
                            DWORD, DWORD, HANDLE, HANDLE, LPVOID data) {
  auto* progress = (const Progress*)data;
  return (*progress)(done.QuadPart, total.QuadPart) ? PROGRESS_CONTINUE : PROGRESS_CANCEL;
}

}  // namespace

bool CopyFileWithProgress(const fs::path& src, const fs::path& dest, const Progress& progress,
                          std::wstring* error) {
  BOOL cancel = FALSE;
  if (CopyFileExW(src.c_str(), dest.c_str(), CopyProgress, (LPVOID)&progress, &cancel, 0))
    return true;
  const DWORD err = GetLastError();
  *error = err == ERROR_REQUEST_ABORTED ? L"已取消"
                                        : L"複製失敗（錯誤 " + std::to_wstring(err) + L"）";
  return false;
}

bool FileInUse(const std::wstring& path) {
  HANDLE h = CreateFileW(path.c_str(), DELETE, 0, NULL, OPEN_EXISTING, 0, NULL);
  if (h == INVALID_HANDLE_VALUE)
    return true;
  CloseHandle(h);
  return false;
}

// ---------------------------------------------------------------------------
// API 連線測試

namespace {

std::string JsonEscape(const std::string& s) {
  std::string out;
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += (char)c;
    } else if (c < 0x20) {
      char buf[8];
      sprintf_s(buf, "\\u%04x", c);
      out += buf;
    } else {
      out += (char)c;
    }
  }
  return out;
}

bool HttpRequest(const wchar_t* method, const std::wstring& url, const std::wstring& key,
                 const std::string& body, DWORD* status, std::string* response,
                 std::wstring* error) {
  URL_COMPONENTS uc = {0};
  uc.dwStructSize = sizeof(uc);
  wchar_t host[256] = {0}, path[2048] = {0}, extra[2048] = {0};
  uc.lpszHostName = host;
  uc.dwHostNameLength = _countof(host);
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = _countof(path);
  uc.lpszExtraInfo = extra;
  uc.dwExtraInfoLength = _countof(extra);
  if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc)) {
    *error = L"網址格式不正確";
    return false;
  }
  const std::wstring host_s(host, uc.dwHostNameLength);
  const bool local = host_s == L"localhost" || host_s == L"127.0.0.1";
  HINTERNET session = WinHttpOpen(L"Weasel Deployer",
                                  local ? WINHTTP_ACCESS_TYPE_NO_PROXY
                                        : WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session)
    session = WinHttpOpen(L"Weasel Deployer", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    *error = L"無法建立連線";
    return false;
  }
  WinHttpSetTimeouts(session, 10000, 10000, 30000, 60000);
  const std::wstring object = std::wstring(path) + extra;
  HINTERNET connect = WinHttpConnect(session, host_s.c_str(), uc.nPort, 0);
  HINTERNET request =
      connect ? WinHttpOpenRequest(connect, method, object.c_str(), NULL, WINHTTP_NO_REFERER,
                                   WINHTTP_DEFAULT_ACCEPT_TYPES,
                                   uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
              : NULL;
  bool ok = false;
  if (request) {
    std::wstring headers = L"Content-Type: application/json\r\n";
    if (!key.empty())
      headers += L"Authorization: Bearer " + key + L"\r\n";
    if (WinHttpSendRequest(request, headers.c_str(), (DWORD)-1,
                           body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
                           (DWORD)body.size(), (DWORD)body.size(), 0) &&
        WinHttpReceiveResponse(request, NULL)) {
      DWORD size = sizeof(*status);
      WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, status, &size, WINHTTP_NO_HEADER_INDEX);
      DWORD avail = 0;
      while (WinHttpQueryDataAvailable(request, &avail) && avail > 0 && response->size() < (1 << 20)) {
        std::vector<char> buf(avail);
        DWORD read = 0;
        if (!WinHttpReadData(request, buf.data(), avail, &read))
          break;
        response->append(buf.data(), read);
      }
      ok = true;
    } else {
      const DWORD err = GetLastError();
      *error = err == ERROR_WINHTTP_TIMEOUT             ? L"連線逾時"
               : err == ERROR_WINHTTP_CANNOT_CONNECT    ? L"無法連線到伺服器（服務沒開或網址錯誤）"
               : err == ERROR_WINHTTP_NAME_NOT_RESOLVED ? L"找不到主機名稱"
               : err == ERROR_WINHTTP_SECURE_FAILURE    ? L"HTTPS 憑證驗證失敗"
                                                        : L"連線失敗（錯誤 " + std::to_wstring(err) + L"）";
    }
  } else {
    *error = L"無法連線到伺服器";
  }
  if (request)
    WinHttpCloseHandle(request);
  if (connect)
    WinHttpCloseHandle(connect);
  WinHttpCloseHandle(session);
  return ok;
}

// 取出 JSON 裡第一個 "key": "..." 的字串值（處理跳脫與 \uXXXX）
bool JsonString(const std::string& json, const char* key, std::wstring* out, size_t from = 0) {
  const std::string pattern = std::string("\"") + key + "\"";
  size_t pos = from;
  while ((pos = json.find(pattern, pos)) != std::string::npos) {
    size_t p = json.find_first_not_of(" \t\r\n", pos + pattern.size());
    if (p == std::string::npos || json[p] != ':') {
      pos += pattern.size();
      continue;
    }
    p = json.find_first_not_of(" \t\r\n", p + 1);
    if (p == std::string::npos || json[p] != '"') {
      if (p == std::string::npos)
        return false;
      pos = p;  // 不是字串（例如物件），繼續找下一個
      continue;
    }
    std::wstring w;
    std::string run;
    auto flush = [&]() {
      w += u8tow(run);
      run.clear();
    };
    for (size_t i = p + 1; i < json.size(); ++i) {
      const char c = json[i];
      if (c == '"') {
        flush();
        *out = w;
        return true;
      }
      if (c != '\\' || i + 1 >= json.size()) {
        run += c;
        continue;
      }
      const char e = json[++i];
      if (e == 'u' && i + 4 < json.size()) {
        flush();
        w += (wchar_t)strtoul(json.substr(i + 1, 4).c_str(), nullptr, 16);
        i += 4;
      } else {
        run += e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e;
      }
    }
    return false;
  }
  return false;
}

// 由 chat/completions 網址推出 models 網址
std::wstring ModelsUrl(const std::wstring& url) {
  const std::wstring tail = L"/chat/completions";
  const size_t p = url.rfind(tail);
  if (p != std::wstring::npos)
    return url.substr(0, p) + L"/models";
  return L"";
}

std::wstring ListModels(const std::wstring& url, const std::wstring& key) {
  const std::wstring models_url = ModelsUrl(url);
  if (models_url.empty())
    return L"";
  DWORD status = 0;
  std::string response;
  std::wstring error;
  if (!HttpRequest(L"GET", models_url, key, "", &status, &response, &error) || status != 200)
    return L"";
  std::wstring names;
  int count = 0;
  size_t pos = 0;
  std::wstring id;
  while ((pos = response.find("\"id\"", pos)) != std::string::npos) {
    if (JsonString(response, "id", &id, pos)) {
      if (count < 8)
        names += (count ? L"、" : L"") + id;
      ++count;
    }
    pos += 4;
  }
  if (!count)
    return L"";
  return L"可用的模型：" + names + (count > 8 ? L"…（共 " + std::to_wstring(count) + L" 個）" : L"");
}

}  // namespace

std::wstring TestApi(const std::wstring& api_url, const std::wstring& api_key,
                     const std::wstring& model) {
  std::string body = "{\"model\":\"" + JsonEscape(wtou8(model)) +
                     "\",\"messages\":[{\"role\":\"user\",\"content\":\"" +
                     JsonEscape(u8"請只回覆「OK」兩個字。") +
                     "\"}],\"max_tokens\":64,\"temperature\":0,\"stream\":false}";
  const ULONGLONG t0 = GetTickCount64();
  DWORD status = 0;
  std::string response;
  std::wstring error;
  if (!HttpRequest(L"POST", api_url, api_key, body, &status, &response, &error))
    return L"✗ " + error;
  const ULONGLONG ms = GetTickCount64() - t0;
  std::wstring server_message;
  JsonString(response, "message", &server_message);
  if (server_message.size() > 120)
    server_message = server_message.substr(0, 120) + L"…";
  if (status == 200) {
    std::wstring content;
    const size_t choices = response.find("\"choices\"");
    if (choices == std::string::npos || !JsonString(response, "content", &content, choices))
      return L"✗ 連線成功，但回應不是 OpenAI 相容格式（網址是否指向 /v1/chat/completions？）";
    content = Trim(content);
    if (content.size() > 40)
      content = content.substr(0, 40) + L"…";
    return L"✓ 連線成功（" + std::to_wstring(ms) + L" ms）：" +
           (content.empty() ? L"回覆是空的（推理模型可能需要較多 token）" : L"模型回覆「" + content + L"」");
  }
  std::wstring reason;
  switch (status) {
    case 401:
    case 403: reason = L"金鑰錯誤或沒有權限"; break;
    case 404: reason = L"網址或模型名稱不正確"; break;
    case 400: reason = L"請求被拒絕（常見原因：模型名稱不正確）"; break;
    case 429: reason = L"超過使用頻率或額度"; break;
    default: reason = status >= 500 ? L"伺服器錯誤" : L"失敗";
  }
  std::wstring text = L"✗ HTTP " + std::to_wstring(status) + L" " + reason;
  if (!server_message.empty())
    text += L"：" + server_message;
  if (status == 400 || status == 404 || model.empty()) {
    const std::wstring models = ListModels(api_url, api_key);
    if (!models.empty())
      text += L"\r\n" + models;
  }
  return text;
}

// ---------------------------------------------------------------------------
// 注音方案的 custom.yaml

namespace {

const wchar_t* const kZhuyinSchemas[] = {L"bopomofo", L"bopomofo_express", L"bopomofo_tw"};
const char kBoostBegin[] = "  # >>> weasel-personal-dict";
const char kBoostEnd[] = "  # <<< weasel-personal-dict";
const char kTypoBegin[] = "  # >>> weasel-typo-correction";
const char kTypoEnd[] = "  # <<< weasel-typo-correction";
const char kGrammarBegin[] = "  # >>> weasel-grammar";
const char kGrammarEnd[] = "  # <<< weasel-grammar";
const wchar_t kGrammarFile[] = L"zh-hant-t-essay-bgw.gram";
const ULONGLONG kGrammarMinBytes = 30ull * 1024 * 1024;  // 完整的檔案約 41 MB

// 改寫一個方案的 custom.yaml：拿掉 begin ~ end 標記的區塊，block 非空時再加在 patch: 下面。
// 使用者自己設定過 conflict_keys 其中一項時不修改，回傳 false
bool PatchSchemaBlock(const fs::path& file, const char* begin, const char* end,
                      const std::vector<std::string>& block,
                      const std::vector<std::string>& conflict_keys, std::wstring* error) {
  const bool enable = !block.empty();
  std::string text;
  {
    std::ifstream in(file, std::ios::binary);
    if (in)
      text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  if (text.empty() && !enable)
    return true;
  if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0)
    text.erase(0, 3);
  // 拆行並拿掉我們之前加的區塊
  std::vector<std::string> lines;
  {
    std::istringstream in(text);
    bool inside = false;
    for (std::string line; std::getline(in, line);) {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      if (line.rfind(begin, 0) == 0) {
        inside = true;
        continue;
      }
      if (inside) {
        if (line.rfind(end, 0) == 0)
          inside = false;
        continue;
      }
      lines.push_back(line);
    }
  }
  if (enable) {
    for (const auto& line : lines) {
      for (const auto& key : conflict_keys) {
        if (line.find(key) != std::string::npos) {
          *error = file.filename().wstring() + L" 已自行設定 " + u8tow(key) + L"，沒有修改";
          return false;
        }
      }
    }
    auto patch = std::find_if(lines.begin(), lines.end(), [](const std::string& l) {
      return l.rfind("patch:", 0) == 0;
    });
    if (patch != lines.end() && patch->find_first_not_of(" \t", 6) != std::string::npos &&
        (*patch)[patch->find_first_not_of(" \t", 6)] != '#') {
      *error = file.filename().wstring() + L" 的 patch 格式無法自動修改";
      return false;
    }
    if (patch == lines.end()) {
      lines.push_back("patch:");
      patch = lines.end() - 1;
    }
    lines.insert(patch + 1, block.begin(), block.end());
  }
  std::string result;
  for (const auto& line : lines)
    result += line + "\n";
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  if (!out) {
    *error = L"無法寫入 " + file.filename().wstring();
    return false;
  }
  out << result;
  return true;
}

// 注音排序：改用 terra_pinyin.personal 詞典
bool PatchSchemaDictionary(const fs::path& file, bool enable, std::wstring* error) {
  std::vector<std::string> block;
  if (enable)
    block = {
        std::string(kBoostBegin) + u8"：個人詞庫的常用詞影響選字排序（小狼毫設定自動管理）",
        "  translator/dictionary: terra_pinyin.personal",
        "  translator/user_dict: terra_pinyin",
        kBoostEnd,
    };
  return PatchSchemaBlock(file, kBoostBegin, kBoostEnd, block,
                          {"translator/dictionary", "translator/user_dict"}, error);
}

// 注音容錯：打開 Rime 的拼寫糾錯（依鍵盤鄰鍵與編輯距離找相近的音節）
bool PatchSchemaCorrection(const fs::path& file, bool enable, std::wstring* error) {
  std::vector<std::string> block;
  if (enable)
    block = {
        std::string(kTypoBegin) + u8"：打錯注音時找相近的音節（小狼毫設定自動管理）",
        "  translator/enable_correction: true",
        kTypoEnd,
    };
  return PatchSchemaBlock(file, kTypoBegin, kTypoEnd, block, {"translator/enable_correction"},
                          error);
}

// 在方案加上 octagram 語言模型（設定同 rime-octagram-data 的 grammar:/hant）
bool PatchSchemaGrammar(const fs::path& file, bool enable, std::wstring* error) {
  std::vector<std::string> block;
  if (enable)
    block = {
        std::string(kGrammarBegin) + u8"：語言模型改善整句選字（小狼毫設定自動管理）",
        "  grammar:",
        "    language: zh-hant-t-essay-bgw",
        "  translator/contextual_suggestions: true",
        "  translator/max_homophones: 7",
        "  translator/max_homographs: 7",
        kGrammarEnd,
    };
  return PatchSchemaBlock(file, kGrammarBegin, kGrammarEnd, block,
                          {"grammar:", "translator/contextual_suggestions"}, error);
}

// 三個注音方案都改；關閉時只還原已有的檔案
bool PatchAllZhuyin(bool enable, std::wstring* error,
                    bool (*patch)(const fs::path&, bool, std::wstring*)) {
  const fs::path user_dir = WeaselUserDataPath();
  bool ok = true;
  for (const wchar_t* schema : kZhuyinSchemas) {
    const fs::path file = user_dir / (std::wstring(schema) + L".custom.yaml");
    std::error_code ec;
    if (!enable && !fs::exists(file, ec))
      continue;
    if (!patch(file, enable, error))
      ok = false;
  }
  return ok;
}

}  // namespace

const wchar_t kGrammarUrl[] =
    L"https://raw.githubusercontent.com/lotem/rime-octagram-data/hant/zh-hant-t-essay-bgw.gram";

bool ApplyRimeBoost(RimeLeversApi* api, RimeSwitcherSettings* switcher, bool enable,
                    std::wstring* error) {
  const fs::path user_dir = WeaselUserDataPath();
  const fs::path dict = user_dir / L"terra_pinyin.personal.dict.yaml";
  // 目前選用的方案
  std::set<std::wstring> selected;
  RimeSchemaList list = {0};
  if (api->get_selected_schema_list(switcher, &list)) {
    for (size_t i = 0; i < list.size; ++i)
      selected.insert(u8tow(list.list[i].schema_id));
    api->schema_list_destroy(&list);
  }
  if (enable) {
    // 先準備詞典檔：請輸入法產生；輸入法沒回應時放一份空的，確保方案編譯得過
    std::error_code ec;
    fs::remove(dict, ec);
    SendPersonalCommand(7);
    if (!fs::exists(dict, ec)) {
      std::ofstream f(dict, std::ios::binary | std::ios::trunc);
      f << u8"# 小狼毫個人詞庫：你常打的詞（由輸入法自動產生，請勿手動修改）\n"
           "---\nname: terra_pinyin.personal\nversion: \"1\"\nsort: by_weight\n"
           "use_preset_vocabulary: true\nmax_phrase_length: 7\nmin_phrase_weight: 100\n"
           "import_tables:\n  - terra_pinyin\ncolumns:\n  - text\n  - weight\n...\n";
    }
  }
  bool ok = true;
  for (const wchar_t* schema : kZhuyinSchemas) {
    const fs::path file = user_dir / (std::wstring(schema) + L".custom.yaml");
    std::error_code ec;
    if (enable && !selected.count(schema)) {
      PatchSchemaDictionary(file, false, error);  // 沒選用的方案不改（之前改過的還原）
      continue;
    }
    if (!enable && !fs::exists(file, ec))
      continue;
    if (!PatchSchemaDictionary(file, enable, error))
      ok = false;
  }
  if (!enable) {
    std::error_code ec;
    fs::remove(dict, ec);
  }
  return ok;
}

bool ApplyTypoCorrection(bool enable, std::wstring* error) {
  // 三個注音方案都改（沒選用的也改，之後改選時不必再套用一次）
  return PatchAllZhuyin(enable, error, PatchSchemaCorrection);
}

bool ApplyGrammar(bool enable, std::wstring* error) {
  return PatchAllZhuyin(enable, error, PatchSchemaGrammar);
}

bool GrammarEnabled() {
  std::ifstream in(WeaselUserDataPath() / L"bopomofo_express.custom.yaml", std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return text.find("# >>> weasel-grammar") != std::string::npos;
}

fs::path GrammarPath() {
  return WeaselUserDataPath() / kGrammarFile;
}

bool GrammarReady() {
  std::error_code ec;
  return fs::file_size(GrammarPath(), ec) >= kGrammarMinBytes && !ec;
}

// ---------------------------------------------------------------------------
// 選字統計

std::vector<StatsRow> ChoiceStats(int span_days) {
  // weasel_stats.txt（輸入法寫的）：日期 組合代碼 送出 字數 換字 LLM出現 LLM採用 校正採用 Backspace
  //   送出後刪除 逐字選字 推薦出現 推薦套用；舊格式沒有組合代碼
  // weasel_stats_profiles.txt：組合代碼 版本 編譯時間 版本說明 設定 第一次出現
  struct Sum {
    int64_t commits = 0, chars = 0, changed = 0, offered = 0, used = 0, corrections = 0, backs = 0,
            deleted = 0, focus = 0, rec_offered = 0, rec_used = 0;
    void Add(const Sum& s) {
      commits += s.commits;
      chars += s.chars;
      changed += s.changed;
      offered += s.offered;
      used += s.used;
      corrections += s.corrections;
      backs += s.backs;
      deleted += s.deleted;
      focus += s.focus;
      rec_offered += s.rec_offered;
      rec_used += s.rec_used;
    }
  };
  struct Profile {
    std::wstring version, time, subject, settings;
  };
  std::map<std::string, Profile> profiles;
  {
    std::ifstream in(WeaselUserDataPath() / L"weasel_stats_profiles.txt", std::ios::binary);
    for (std::string line; std::getline(in, line);) {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      std::vector<std::wstring> f;
      std::string key;
      size_t start = 0;
      for (int i = 0; i < 6; ++i) {
        const size_t tab = line.find('\t', start);
        const std::string part = line.substr(start, tab == std::string::npos ? tab : tab - start);
        if (i == 0)
          key = part;
        else
          f.push_back(u8tow(part));
        if (tab == std::string::npos)
          break;
        start = tab + 1;
      }
      f.resize(5);
      if (!key.empty())
        profiles[key] = {f[0], f[1], f[2], f[3]};
    }
  }
  // 期間的起始日（本機時間，n 天前）
  auto date_before = [](int n) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    FILETIME ft;
    SystemTimeToFileTime(&st, &ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    u.QuadPart -= (ULONGLONG)n * 24 * 3600 * 10000000ULL;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    FileTimeToSystemTime(&ft, &st);
    char buf[16];
    sprintf_s(buf, "%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
    return std::string(buf);
  };
  const std::string from = span_days > 0 ? date_before(span_days - 1) : std::string();

  std::map<std::string, Sum> sums;          // 組合代碼 → 期間內合計
  std::map<std::string, std::string> last;  // 組合代碼 → 最後使用日
  {
    std::ifstream in(WeaselUserDataPath() / L"weasel_stats.txt", std::ios::binary);
    for (std::string line; std::getline(in, line);) {
      std::istringstream f(line);
      std::string date, key;
      if (!(f >> date >> key) || date < from)
        continue;
      std::istringstream numbers;
      if (key.find_first_not_of("0123456789") == std::string::npos) {
        numbers.str(line.substr(line.find(date) + date.size()));
        key = "legacy";
      } else {
        numbers.str(line.substr(line.find(key) + key.size()));
      }
      Sum s;
      if (numbers >> s.commits >> s.chars >> s.changed >> s.offered >> s.used >> s.corrections >>
          s.backs) {
        numbers >> s.deleted >> s.focus >> s.rec_offered >> s.rec_used;  // 較新的欄位
        sums[key].Add(s);
        last[key] = (std::max)(last[key], date);
      }
    }
  }

  auto percent = [](int64_t n, int64_t d) {
    if (!d)
      return std::wstring(L"—");
    wchar_t buf[32];
    swprintf_s(buf, L"%.1f%%", 100.0 * n / d);
    return std::wstring(buf);
  };
  auto ratio = [](int64_t n, int64_t d) {
    return d ? std::to_wstring(n) + L"／" + std::to_wstring(d) : std::wstring(L"—");
  };
  auto counts = [](const Sum& t) {
    std::wostringstream out;
    out << t.chars << L" 字、逐字選字 " << t.focus << L"、推薦出現 " << t.rec_offered
        << L"、LLM 出現 " << t.offered << L"（校正採用 " << t.corrections << L"）、Backspace "
        << t.backs << L"、送出後刪除 " << t.deleted << L" 次";
    return out.str();
  };

  // 最近用過的組合排前面
  std::vector<std::string> keys;
  for (const auto& [key, s] : sums)
    keys.push_back(key);
  std::sort(keys.begin(), keys.end(), [&](const std::string& a, const std::string& b) {
    return last[a] != last[b] ? last[a] > last[b] : a < b;
  });

  std::vector<StatsRow> rows;
  auto add_row = [&](const std::wstring& version, const std::wstring& settings, const Sum& t,
                     const std::wstring& detail) {
    StatsRow row;
    row.version = version;
    row.settings = settings;
    row.commits = t.commits;
    row.first_ok = percent(t.commits - t.changed, t.commits);
    row.deleted = percent(t.deleted, t.chars);
    row.changed = t.changed;
    row.recommended = ratio(t.rec_used, t.rec_offered);
    row.llm = ratio(t.used, t.offered);
    row.detail = detail;
    rows.push_back(std::move(row));
  };
  if (keys.size() > 1) {
    Sum total;
    for (const auto& [key, s] : sums)
      total.Add(s);
    add_row(L"（合計）", L"所有組合", total, L"所有版本與設定組合的合計\n" + counts(total));
  }
  for (const auto& key : keys) {
    const Sum& t = sums[key];
    std::wstring version, settings, detail;
    if (key == "legacy") {
      version = L"舊資料";
      settings = L"（未記錄）";
      detail = L"分版本統計之前的紀錄，沒有版本與設定資訊\n";
    } else {
      auto p = profiles.find(key);
      const Profile info = p != profiles.end() ? p->second : Profile{L"?", L"", L"", L"?"};
      version = info.version;
      // 列表只放短的：方案名稱留給詳細資訊
      settings = info.settings;
      const size_t bar = settings.find(L'｜');
      if (bar != std::wstring::npos)
        settings = settings.substr(bar + 1);
      detail = L"版本 " + info.version;
      if (!info.version.empty() && info.version.back() == L'*')
        detail += L"（含未提交的修改）";
      if (!info.time.empty())
        detail += L"，" + info.time + L" 編譯";
      if (!info.subject.empty())
        detail += L"：" + info.subject;
      detail += L"\n設定：" + info.settings + L"\n";
    }
    add_row(version, settings, t, detail + counts(t));
  }
  return rows;
}

bool ResetChoiceStats() {
  if (!SendPersonalCommand(8)) {
    std::error_code ec;
    fs::remove(WeaselUserDataPath() / L"weasel_stats.txt", ec);
    fs::remove(WeaselUserDataPath() / L"weasel_stats_profiles.txt", ec);
  }
  Sleep(200);  // 等輸入法刪檔
  return true;
}

ChoiceLogInfo ChoiceLogStatus() {
  const fs::path file = PersonalDir() / L"choice_log.dat";
  ChoiceLogInfo info;
  std::error_code ec;
  info.bytes = fs::file_size(file, ec);
  if (ec) {
    info.bytes = 0;
    return info;
  }
  std::ifstream in(file, std::ios::binary);
  uint32_t len = 0;
  while (in.read((char*)&len, sizeof(len)) && in.seekg(len, std::ios::cur))
    ++info.records;
  return info;
}

bool ClearChoiceLog() {
  std::error_code ec;
  fs::remove(PersonalDir() / L"choice_log.dat", ec);
  return !ec;
}

// ---------------------------------------------------------------------------
// 個人詞庫

fs::path PersonalDir() {
  return WeaselUserDataPath() / L"personal";
}

bool SendPersonalCommand(unsigned long command) {
  weasel::Client client;
  if (!client.Connect())
    return false;
  client.PersonalCommand(command);
  return true;
}

std::wstring PersonalStatusText(bool* running) {
  std::map<std::string, std::string> v;
  {
    std::ifstream in(PersonalDir() / L"status.txt", std::ios::binary);
    for (std::string line; std::getline(in, line);) {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      const size_t eq = line.find('=');
      if (eq != std::string::npos)
        v[line.substr(0, eq)] = line.substr(eq + 1);
    }
  }
  auto num = [&](const char* key) { return _atoi64(v[key].c_str()); };
  std::wostringstream text;
  *running = false;
  if (v.empty()) {
    text << L"無法取得狀態：請確認小狼毫正在執行。";
  } else if (v.count("disabled")) {
    text << L"個人詞庫目前關閉。勾選上方的「啟用個人詞庫」並按「套用」即可開始學習。";
  } else {
    *running = v["running"] == "1";
    text << L"詞庫：" << num("words") << L" 個詞、" << num("pairs") << L" 組接續\n"
         << L"原始紀錄：累積中 " << num("active_records") << L" 筆；已封存 "
         << num("archived_files") << L" 批、共 " << num("archived_records") << L" 筆\n";
    const int64_t last = num("last_refine");
    const double interval = atof(v["interval_days"].c_str());
    text << L"上次精煉：" << FormatTime(last);
    if (interval > 0 && last > 0)
      text << L"　下次：" << FormatTime(last + (int64_t)(interval * 86400)) << L" 之後的閒置時間";
    else
      text << L"　（自動精煉已關閉，只手動）";
    if (v["rime_boost"] == "1")
      text << L"\n注音排序：已加入 " << num("rime_words") << L" 個常打的詞"
           << (num("rime_updated") > 0 ? L"（更新於 " + FormatTime(num("rime_updated")) + L"）" : L"");
    const std::wstring method = u8tow(v["method"]);
    text << L"\n精煉方式：" << (method.empty() ? L"只做統計整理（未選擇精煉模型）" : method) << L"\n";
    if (*running)
      text << L"精煉中…" << (v["progress"].empty() ? L"" : L"（" + u8tow(v["progress"]) + L"）");
    else if (!v["last_result"].empty())
      text << L"上次結果：" << u8tow(v["last_result"]);
  }
  return text.str();
}

PersonalWords LoadPersonalWords() {
  PersonalWords result;
  const fs::path file = PersonalDir() / L"export.dat";
  std::error_code ec;
  fs::remove(file, ec);
  std::string plain;
  if (!SendPersonalCommand(5)) {
    result.error = L"無法連線到輸入法服務。";
  } else if (!personal_crypto::ReadProtected(file, &plain)) {
    // 個人詞庫關閉時輸入法不匯出
  } else {
    result.available = true;
    std::istringstream lines(plain);
    for (std::string line; std::getline(lines, line);) {
      std::vector<std::wstring> f;
      size_t start = 0, tab;
      while ((tab = line.find('\t', start)) != std::string::npos) {
        f.push_back(u8tow(line.substr(start, tab - start)));
        start = tab + 1;
      }
      f.push_back(u8tow(line.substr(start)));
      if (f.size() == 3 && f[0] == L"W")
        result.words.emplace_back(f[1], _wtof(f[2].c_str()));
      else if (f.size() == 2 && f[0] == L"A")
        result.rules.push_back({WordRule::kAdd, f[1], L""});
      else if (f.size() == 2 && f[0] == L"R")
        result.rules.push_back({WordRule::kBlock, f[1], L""});
      else if (f.size() == 3 && f[0] == L"M")
        result.rules.push_back({WordRule::kMerge, f[1], f[2]});
    }
  }
  fs::remove(file, ec);
  return result;
}

bool SendWordEdits(const std::vector<std::wstring>& lines, std::wstring* error) {
  std::string plain = "WWPE1\n";
  for (const auto& line : lines)
    plain += wtou8(line) + "\n";
  if (!personal_crypto::WriteProtected(PersonalDir() / L"edit.dat", plain)) {
    *error = L"無法寫入修改檔。";
    return false;
  }
  if (!SendPersonalCommand(6)) {
    *error = L"無法連線到輸入法服務。";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// 預測測試

namespace {
fs::path RequestFile() {
  return WeaselUserDataPath() / L"llm_test_request.txt";
}
fs::path ResponseFile() {
  return WeaselUserDataPath() / L"llm_test_response.txt";
}
}  // namespace

bool SendLLMTestRequest(unsigned id, const std::wstring& context) {
  std::error_code ec;
  fs::remove(ResponseFile(), ec);
  {
    std::ofstream out(RequestFile(), std::ios::binary | std::ios::trunc);
    out << id << "\n" << wtou8(context);
  }
  weasel::Client client;
  if (!client.Connect())
    return false;
  client.LLMTestRequest();
  return true;
}

std::optional<LLMTestResponse> PollLLMTestResponse(unsigned id) {
  std::string text;
  {
    std::ifstream in(ResponseFile(), std::ios::binary);
    if (!in)
      return std::nullopt;
    text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  std::string response_id;
  LLMTestResponse r;
  std::istringstream lines(text);
  for (std::string line; std::getline(lines, line);) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    const size_t eq = line.find('=');
    if (eq == std::string::npos)
      continue;
    const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
    if (key == "id") response_id = value;
    else if (key == "status") r.status = value;
    else if (key == "model") r.model = u8tow(value);
    else if (key == "ms") r.ms = value;
    else if (key == "cand") r.candidates.push_back(u8tow(value));
  }
  if (response_id != std::to_string(id))
    return std::nullopt;  // 還沒輪到這次的回覆
  return r;
}

// ---------------------------------------------------------------------------
// 使用者詞典

std::vector<std::wstring> ListUserDicts(RimeLeversApi* api) {
  std::vector<std::wstring> dicts;
  RimeUserDictIterator iter = {0};
  api->user_dict_iterator_init(&iter);
  while (const char* dict = api->next_user_dict(&iter))
    dicts.push_back(u8tow(dict));
  api->user_dict_iterator_destroy(&iter);
  return dicts;
}

ServiceMaintenance::ServiceMaintenance() {
  weasel::Client client;
  if (client.Connect())
    client.StartMaintenance();
}

ServiceMaintenance::~ServiceMaintenance() {
  weasel::Client client;
  if (client.Connect())
    client.EndMaintenance();
}

void PrepareUserDictTask() {
  static bool ready = false;
  RimeApi* rime = rime_get_api();
  if (!ready && RIME_API_AVAILABLE(rime, run_task)) {
    rime->run_task("installation_update");
    ready = true;
  }
}

}  // namespace settings_ops
