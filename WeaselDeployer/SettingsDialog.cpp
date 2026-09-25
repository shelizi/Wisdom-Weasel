#include "stdafx.h"
#include "SettingsDialog.h"
#include "FontSettingDialog.h"
#include "WeaselDeployer.h"
#include <PersonalCrypto.h>
#include <WeaselIPC.h>
#include <WeaselUtility.h>
#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <dwmapi.h>
#include <uxtheme.h>
#include <shlobj.h>
#include <shobjidl.h>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#include <shlwapi.h>
#include <winhttp.h>
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "winhttp.lib")

namespace fs = std::filesystem;

namespace {

// 顏色參考 Windows 11 設定 App 的淺色／深色外觀
//                                   bg                  nav                 nav_selected        accent              text                subtle              input               separator
const SettingsDialog::Palette kLight{RGB(255, 255, 255), RGB(243, 243, 243), RGB(224, 234, 246), RGB(0, 95, 184),    RGB(27, 27, 27),    RGB(96, 96, 96),    RGB(255, 255, 255), RGB(229, 229, 229)};
const SettingsDialog::Palette kDark{RGB(39, 39, 39),     RGB(32, 32, 32),    RGB(45, 55, 66),    RGB(76, 194, 255),  RGB(255, 255, 255), RGB(200, 200, 200), RGB(29, 29, 29),    RGB(58, 58, 58)};

const wchar_t kThemeRegKey[] = L"Software\\Rime\\Weasel";
const wchar_t kThemeRegValue[] = L"SettingsTheme";

struct PageInfo {
  int id_base;          // 這頁控制項的 ID 範圍（id_base ~ id_base + 99）
  const wchar_t* nav;
  const wchar_t* icon;  // Segoe Fluent Icons / Segoe MDL2 Assets
  const wchar_t* title;
  const wchar_t* desc;
};
// 順序要與 SettingsDialog::Page 一致
const PageInfo kPages[] = {
    {3100, L"輸入方案", L"\uE765", L"輸入方案", L"選擇要使用的輸入方案，並查看各方案的說明。"},
    {3200, L"外觀", L"\uE790", L"外觀", L"候選視窗的配色與字體。"},
    {3300, L"智慧預測", L"\uE82F", L"LLM 智慧預測",
     L"用語言模型預測你接下來要打的詞。"},
    {3500, L"語言模型", L"\uE950", L"語言模型",
     L"本機（llama.cpp）或 OpenAI 相容 API 的模型設定，可以設定多組。"},
    {3400, L"個人詞庫", L"\uE8F1", L"個人詞庫",
     L"從你打過的字學習常用詞，優先出現在預測候選中；可定時用 LLM 精煉。"},
    {3600, L"詞庫管理", L"\uE82D", L"詞庫管理",
     L"維護個人詞庫的詞彙與精煉規則，以及備份、匯出輸入法的使用者詞典。"},
};
const int kPageCount = sizeof(kPages) / sizeof(kPages[0]);

// 控制項 ID 依範圍分頁（見 kPages 的 id_base）；其餘為共用
int PageOfControl(int id) {
  if (id < 3100 || id >= 3700)
    return -1;
  for (int i = 0; i < kPageCount; ++i) {
    if (kPages[i].id_base == id / 100 * 100)
      return i;
  }
  return -1;
}

bool IsSubtleText(int id) {
  switch (id) {
    case IDC_PAGE_DESC:
    case IDC_P1_HINT:
    case IDC_P1_HOTKEY_LABEL:
    case IDC_P3_LOADED:
    case IDC_P3_TEST_HINT:
    case IDC_P3_TEST_STATUS:
    case IDC_P3_PREFIX_HINT:
    case IDC_P4_INTERVAL_HINT:
    case IDC_P4_API_HINT:
    case IDC_P4_HINT:
    case IDC_P5_HINT:
    case IDC_P5_LOCAL_HINT:
    case IDC_P5_REMOTE_HINT:
    case IDC_P5_USAGE:
    case IDC_P5_FILE_STATUS:
    case IDC_P6_COUNT:
    case IDC_P6_RULES_LABEL:
    case IDC_P6_HINT:
    case IDC_P6_DICT_HINT:
    case IDC_STATUS:
    case IDC_THEME_LABEL:
      return true;
  }
  return false;
}

// 語言模型頁的「本機模型」與「OpenAI 相容 API」兩組控制項疊在同一塊位置，依選擇顯示其一
bool IsLocalOnly(int id) {
  return id == IDC_P5_MODEL_LABEL || id == IDC_P5_MODEL || id == IDC_P5_BROWSE ||
         id == IDC_P5_TYPE_LABEL || id == IDC_P5_TYPE || id == IDC_P5_LOCAL_HINT;
}
bool IsRemoteOnly(int id) {
  return id == IDC_P5_API_URL_LABEL || id == IDC_P5_API_URL || id == IDC_P5_API_KEY_LABEL ||
         id == IDC_P5_API_KEY || id == IDC_P5_API_MODEL_LABEL || id == IDC_P5_API_MODEL ||
         id == IDC_P5_REMOTE_HINT || id == IDC_P5_API_TEST;
}

const wchar_t kDefaultApiUrl[] = L"https://api.openai.com/v1/chat/completions";

std::string LLMJsonEscapeLite(const std::string& s) {
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

// 詞庫管理：開啟檔案所在資料夾並選取檔案（沿用原本「用戶詞典管理」的行為）
void OpenFolderAndSelectItem(std::wstring filepath) {
  filepath = std::filesystem::path(filepath).make_preferred().wstring();
  std::wstring directory = std::filesystem::path(filepath).parent_path();
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

template <typename T, typename U>
std::wstring DoFileDialog(HWND owner, LPCWSTR title, UINT filter_size, COMDLG_FILTERSPEC filter[],
                          LPCWSTR filename, LPCWSTR def_ext) {
  std::wstring path;
  CoInitialize(NULL);
  {
    CComPtr<T> dialog;
    if (SUCCEEDED(dialog.CoCreateInstance(__uuidof(U)))) {
      dialog->SetFileTypes(filter_size, filter);
      dialog->SetTitle(title);
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

bool FontExists(const wchar_t* face) {
  LOGFONTW lf = {0};
  lf.lfCharSet = DEFAULT_CHARSET;
  wcsncpy_s(lf.lfFaceName, face, _TRUNCATE);
  bool found = false;
  HDC dc = ::GetDC(NULL);
  ::EnumFontFamiliesExW(
      dc, &lf,
      [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM p) -> int {
        *reinterpret_cast<bool*>(p) = true;
        return 0;
      },
      reinterpret_cast<LPARAM>(&found), 0);
  ::ReleaseDC(NULL, dc);
  return found;
}

std::wstring ToLower(std::wstring s) {
  std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
  return s;
}

std::wstring FileNameOf(const std::wstring& path) {
  return fs::path(path).filename().wstring();
}

std::wstring LLMTrim(const std::wstring& s) {
  const wchar_t* ws = L" \t\r\n　";
  const size_t b = s.find_first_not_of(ws);
  return b == std::wstring::npos ? std::wstring() : s.substr(b, s.find_last_not_of(ws) - b + 1);
}

// 輸入法回報的「目前載入」：本機模型是檔案路徑，只顯示檔名；OpenAI 相容 API 原樣顯示
std::wstring LoadedModelDisplay(const std::wstring& model) {
  if (model.empty())
    return model;
  if (ToLower(fs::path(model).extension().wstring()) == L".gguf")
    return FileNameOf(model);
  return model;
}

// 依檔名猜模型類型：含 base → Base；含 instruct / chat / -it → Instruct；否則維持原值
const wchar_t* GuessModelType(const std::wstring& path) {
  const std::wstring name = ToLower(FileNameOf(path));
  if (name.find(L"base") != std::wstring::npos)
    return L"Base";
  if (name.find(L"instruct") != std::wstring::npos || name.find(L"chat") != std::wstring::npos ||
      name.find(L"-it") != std::wstring::npos)
    return L"Instruct";
  return nullptr;
}

fs::path RequestFile() {
  return WeaselUserDataPath() / L"llm_test_request.txt";
}
fs::path ResponseFile() {
  return WeaselUserDataPath() / L"llm_test_response.txt";
}
fs::path PersonalStatusFile() {
  return WeaselUserDataPath() / L"personal" / L"status.txt";
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

}  // namespace

SettingsDialog::SettingsDialog(RimeSwitcherSettings* switcher_settings,
                               UIStyleSettings* ui_style_settings,
                               std::function<void()> deploy)
    : switcher_settings_(switcher_settings),
      ui_settings_(ui_style_settings),
      deploy_(std::move(deploy)) {
  api_ = (RimeLeversApi*)rime_get_api()->find_module("levers")->get_api();
  // 建立視窗期間就會收到 WM_CTLCOLOR* / WM_DRAWITEM，先給預設的淺色
  pal_ = &kLight;
  bg_brush_ = ::CreateSolidBrush(pal_->bg);
  nav_brush_ = ::CreateSolidBrush(pal_->nav);
  input_brush_ = ::CreateSolidBrush(pal_->input);
  separator_brush_ = ::CreateSolidBrush(pal_->separator);
}

void SettingsDialog::ApplyTheme() {
  dark_ = theme_pref_ == 2 || (theme_pref_ == 0 && IsUserDarkMode());
  pal_ = dark_ ? &kDark : &kLight;
  for (HBRUSH* b : {&bg_brush_, &nav_brush_, &input_brush_, &separator_brush_}) {
    if (*b)
      ::DeleteObject(*b);
  }
  bg_brush_ = ::CreateSolidBrush(pal_->bg);
  nav_brush_ = ::CreateSolidBrush(pal_->nav);
  input_brush_ = ::CreateSolidBrush(pal_->input);
  separator_brush_ = ::CreateSolidBrush(pal_->separator);

  // 標題列（Windows 10 20H1 起為 20，更早的版本為 19）
  BOOL dark = dark_;
  if (FAILED(::DwmSetWindowAttribute(m_hWnd, 20, &dark, sizeof(dark))))
    ::DwmSetWindowAttribute(m_hWnd, 19, &dark, sizeof(dark));

  // 控制項的系統主題：按鈕、核取方塊、捲軸、清單用 DarkMode_Explorer；下拉選單與輸入框用 DarkMode_CFD
  for (HWND child = ::GetWindow(m_hWnd, GW_CHILD); child;
       child = ::GetWindow(child, GW_HWNDNEXT)) {
    wchar_t cls[32] = {0};
    ::GetClassNameW(child, cls, _countof(cls));
    // 深色時清單不畫邊框（系統邊框在深色背景上是刺眼的白線），靠較深的底色區隔；淺色恢復原本的邊框。
    // 對話框會把 WS_BORDER 轉成 WS_EX_CLIENTEDGE，所以兩者都要處理，並記住原始樣式以便切回。
    if ((!_wcsicmp(cls, L"ListBox") && child != nav_.m_hWnd) || !_wcsicmp(cls, WC_LISTVIEWW)) {
      auto original = original_frames_.find(child);
      if (original == original_frames_.end())
        original = original_frames_
                       .emplace(child, std::make_pair(::GetWindowLongW(child, GWL_STYLE),
                                                      ::GetWindowLongW(child, GWL_EXSTYLE)))
                       .first;
      // 只改邊框相關的位元，其餘（例如 WS_VISIBLE：別頁的控制項是隱藏的）保持目前狀態
      const LONG frame = WS_BORDER, ex_frame = WS_EX_CLIENTEDGE | WS_EX_STATICEDGE;
      const LONG current = ::GetWindowLongW(child, GWL_STYLE) & ~frame;
      const LONG current_ex = ::GetWindowLongW(child, GWL_EXSTYLE) & ~ex_frame;
      const LONG style = dark_ ? current : current | (original->second.first & frame);
      const LONG ex_style = dark_ ? current_ex : current_ex | (original->second.second & ex_frame);
      ::SetWindowLongW(child, GWL_STYLE, style);
      ::SetWindowLongW(child, GWL_EXSTYLE, ex_style);
      ::SetWindowPos(child, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (!_wcsicmp(cls, L"Button") || !_wcsicmp(cls, L"ListBox")) {
      ::SetWindowTheme(child, dark_ ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    } else if (!_wcsicmp(cls, L"ComboBox") || !_wcsicmp(cls, L"Edit")) {
      ::SetWindowTheme(child, dark_ ? L"DarkMode_CFD" : nullptr, nullptr);
    } else if (!_wcsicmp(cls, WC_LISTVIEWW)) {
      ::SetWindowTheme(child, dark_ ? L"DarkMode_Explorer" : L"Explorer", nullptr);
      ListView_SetBkColor(child, pal_->input);
      ListView_SetTextBkColor(child, pal_->input);
      ListView_SetTextColor(child, pal_->text);
    }
    ::SendMessageW(child, WM_THEMECHANGED, 0, 0);
  }
  RedrawWindow(NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

LRESULT SettingsDialog::OnThemeChange(WORD, WORD, HWND, BOOL&) {
  const int sel = CComboBox(GetDlgItem(IDC_THEME)).GetCurSel();
  if (sel < 0 || sel > 2)
    return 0;
  theme_pref_ = (DWORD)sel;
  // 設定視窗自己的偏好，不是輸入法設定，直接存登錄檔、不用重新部署
  RegSetKeyValueW(HKEY_CURRENT_USER, kThemeRegKey, kThemeRegValue, REG_DWORD, &theme_pref_,
                  sizeof(theme_pref_));
  ApplyTheme();
  return 0;
}

LRESULT SettingsDialog::OnSettingChange(UINT, WPARAM, LPARAM lParam, BOOL& handled) {
  // 系統切換淺色／深色時會廣播 ImmersiveColorSet
  if (theme_pref_ == 0 && lParam && !wcscmp((LPCWSTR)lParam, L"ImmersiveColorSet"))
    ApplyTheme();
  handled = FALSE;
  return 0;
}

SettingsDialog::~SettingsDialog() {
  preview_image_.Destroy();
  DestroyFonts();
  for (HBRUSH b : {bg_brush_, nav_brush_, input_brush_, separator_brush_}) {
    if (b)
      ::DeleteObject(b);
  }
}

int SettingsDialog::Scale(int px) const {
  // 與 CDialogDpiAware 相同：用視窗所在螢幕的有效 DPI
  UINT dpi_x = 96, dpi_y = 96;
  if (m_hWnd) {
    HMONITOR monitor = ::MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST);
    if (FAILED(::GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y)) || !dpi_x)
      dpi_x = 96;
  }
  return MulDiv(px, dpi_x, 96);
}

// ---------------------------------------------------------------------------
// 初始化與外觀

LRESULT SettingsDialog::OnInitDialog(UINT, WPARAM, LPARAM, BOOL&) {
  DWORD pref = 0, size = sizeof(pref);
  if (RegGetValueW(HKEY_CURRENT_USER, kThemeRegKey, kThemeRegValue, RRF_RT_REG_DWORD, NULL,
                   &pref, &size) == ERROR_SUCCESS &&
      pref <= 2)
    theme_pref_ = pref;
  CComboBox theme(GetDlgItem(IDC_THEME));
  theme.AddString(L"跟隨系統");
  theme.AddString(L"淺色");
  theme.AddString(L"深色");
  theme.SetCurSel((int)theme_pref_);

  nav_.Attach(GetDlgItem(IDC_NAV));
  for (const auto& p : kPages)
    nav_.AddString(p.nav);

  schema_list_.SubclassWindow(GetDlgItem(IDC_P1_SCHEMA_LIST));
  schema_list_.SetExtendedListViewStyle(LVS_EX_FULLROWSELECT, LVS_EX_FULLROWSELECT);
  schema_list_.AddColumn(L"輸入方案", 0);

  color_schemes_.Attach(GetDlgItem(IDC_P2_COLOR_SCHEME));
  preview_.Attach(GetDlgItem(IDC_P2_PREVIEW));

  models_.Attach(GetDlgItem(IDC_P5_MODEL));
  model_type_.Attach(GetDlgItem(IDC_P5_TYPE));
  model_type_.AddString(L"Base");
  model_type_.AddString(L"Instruct");
  profile_list_.Attach(GetDlgItem(IDC_P5_LIST));
  model_files_.Attach(GetDlgItem(IDC_P5_FILES));
  predict_profile_.Attach(GetDlgItem(IDC_P3_PROFILE));
  refine_profile_.Attach(GetDlgItem(IDC_P4_PROFILE));
  test_result_.Attach(GetDlgItem(IDC_P3_TEST_RESULT));
  words_list_.Attach(GetDlgItem(IDC_P6_WORDS));
  words_list_.SetExtendedListViewStyle(LVS_EX_FULLROWSELECT, LVS_EX_FULLROWSELECT);
  words_list_.AddColumn(L"詞", 0);
  words_list_.AddColumn(L"分數", 1);
  rules_list_.Attach(GetDlgItem(IDC_P6_RULES));
  rules_list_.SetExtendedListViewStyle(LVS_EX_FULLROWSELECT, LVS_EX_FULLROWSELECT);
  rules_list_.AddColumn(L"規則", 0);
  dicts_.Attach(GetDlgItem(IDC_P6_DICTS));
  {
    CComboBox personal_max(GetDlgItem(IDC_P4_MAX));
    for (int i = 0; i <= 5; ++i)
      personal_max.AddString(std::to_wstring(i).c_str());
  }
  GetDlgItem(IDC_P3_TEST_INPUT).SetWindowTextW(L"今天天氣很好，我們一起去");

  PopulateSchemas();
  PopulateColorSchemes();
  UpdateFontSummary();
  LoadLLMSettings();

  InitCtrlRects();  // 依螢幕 DPI 縮放所有控制項
  ApplyFonts();
  ApplyTheme();

  const int start = start_page_ >= 0 && start_page_ < kPageCount ? start_page_ : 0;
  nav_.SetCurSel(start);
  ShowPage(start);

  // 詢問輸入法目前載入的模型
  request_id_ = (unsigned)GetTickCount();
  SendLLMRequest(L"", false);

  loaded_ = true;
  CenterWindow();
  BringWindowToTop();
  return TRUE;
}

LRESULT SettingsDialog::OnClose(UINT, WPARAM, LPARAM, BOOL&) {
  EndDialog(IDCANCEL);
  return 0;
}

LRESULT SettingsDialog::OnDestroy(UINT, WPARAM, LPARAM, BOOL& handled) {
  KillTimer(kTimerTestPoll);
  KillTimer(kTimerStatusPoll);
  KillTimer(kTimerPersonalPoll);
  // 關閉視窗時取消進行中的下載／複製
  file_cancel_ = true;
  if (file_worker_.joinable())
    file_worker_.join();
  if (api_worker_.joinable())
    api_worker_.join();
  handled = FALSE;
  return 0;
}

LRESULT SettingsDialog::OnDpiChangedPre(UINT, WPARAM, LPARAM, BOOL& handled) {
  // 讓 CDialogDpiAware 先縮放（它會把所有控制項字型換成對話框字型），之後再套自訂字型
  PostMessage(WM_APP_REFONT);
  handled = FALSE;
  return 0;
}

LRESULT SettingsDialog::OnRefont(UINT, WPARAM, LPARAM, BOOL&) {
  ApplyFonts();
  return 0;
}

void SettingsDialog::DestroyFonts() {
  for (HFONT* f : {&title_font_, &section_font_, &nav_font_, &icon_font_}) {
    if (*f) {
      ::DeleteObject(*f);
      *f = nullptr;
    }
  }
}

void SettingsDialog::ApplyFonts() {
  HFONT base = GetFont();
  LOGFONTW lf = {0};
  if (!base || !::GetObjectW(base, sizeof(lf), &lf))
    return;
  DestroyFonts();

  LOGFONTW title = lf;
  title.lfHeight = lf.lfHeight * 17 / 10;
  title.lfWeight = FW_SEMIBOLD;
  title_font_ = ::CreateFontIndirectW(&title);

  LOGFONTW section = lf;
  section.lfHeight = lf.lfHeight * 11 / 10;
  section.lfWeight = FW_BOLD;
  section_font_ = ::CreateFontIndirectW(&section);

  LOGFONTW nav = lf;
  nav.lfHeight = lf.lfHeight * 11 / 10;
  nav_font_ = ::CreateFontIndirectW(&nav);

  LOGFONTW icon = lf;
  icon.lfHeight = lf.lfHeight * 13 / 10;
  icon.lfWeight = FW_NORMAL;
  // 不能沿用對話框字型的中文字元集，否則 GDI 會改挑中文字型、畫不出圖示
  icon.lfCharSet = DEFAULT_CHARSET;
  wcscpy_s(icon.lfFaceName,
           FontExists(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets");
  icon_font_ = ::CreateFontIndirectW(&icon);

  GetDlgItem(IDC_PAGE_TITLE).SetFont(title_font_);
  for (int id : {IDC_P2_SCHEME_LABEL, IDC_P2_FONT_LABEL, IDC_P3_MODEL_LABEL, IDC_P3_TEST_LABEL,
                 IDC_P3_PREFIX_LABEL, IDC_P3_ENABLED, IDC_P4_ENABLED, IDC_P4_REFINE_LABEL,
                 IDC_P4_DATA_LABEL, IDC_P6_WORDS_LABEL, IDC_P6_DICT_LABEL, IDC_P5_FILES_LABEL})
    GetDlgItem(id).SetFont(section_font_);

  nav_.SetItemHeight(0, Scale(40));
  nav_.Invalidate();

  CRect rc;
  schema_list_.GetClientRect(&rc);
  schema_list_.SetColumnWidth(0, rc.Width() - 2);
  words_list_.GetClientRect(&rc);
  const int score_width = Scale(56);
  words_list_.SetColumnWidth(0, rc.Width() - score_width - ::GetSystemMetrics(SM_CXVSCROLL));
  words_list_.SetColumnWidth(1, score_width);
  rules_list_.GetClientRect(&rc);
  rules_list_.SetColumnWidth(0, rc.Width() - ::GetSystemMetrics(SM_CXVSCROLL));
}

void SettingsDialog::ShowPage(int page) {
  if (page < 0 || page >= kPageCount)
    return;
  page_ = page;
  GetDlgItem(IDC_PAGE_TITLE).SetWindowTextW(kPages[page].title);
  GetDlgItem(IDC_PAGE_DESC).SetWindowTextW(kPages[page].desc);
  const bool remote = IsRemoteProvider();
  for (HWND child = ::GetWindow(m_hWnd, GW_CHILD); child;
       child = ::GetWindow(child, GW_HWNDNEXT)) {
    const int id = ::GetDlgCtrlID(child);
    const int owner = PageOfControl(id);
    if (owner < 0)
      continue;
    bool visible = owner == page;
    if (IsLocalOnly(id))
      visible = visible && !remote;
    else if (IsRemoteOnly(id))
      visible = visible && remote;
    ::ShowWindow(child, visible ? SW_SHOW : SW_HIDE);
  }
  Invalidate();
  if (page == kPageModels) {
    UpdateProfileUsage();
    if (!file_busy_)
      PopulateModelFiles(model_files_.GetCurSel() >= 0 ? model_file_paths_[model_files_.GetCurSel()] : L"");
  }
  if (page == kPageDict) {
    if (!words_loaded_)
      LoadPersonalWords();
    if (!dicts_loaded_)
      PopulateDicts();
  }
  // 個人詞庫頁：顯示時向輸入法要最新狀態，並每秒更新（精煉進度）
  if (page == kPagePersonal) {
    SendPersonalCommand(1);
    RefreshPersonalStatus();
    personal_ticks_ = 0;
    SetTimer(kTimerPersonalPoll, 1000);
  } else {
    KillTimer(kTimerPersonalPoll);
  }
}

bool SettingsDialog::IsRemoteProvider() const {
  return profile_sel_ >= 0 && profile_sel_ < (int)profiles_.size() && profiles_[profile_sel_].remote;
}

void SettingsDialog::GoToPage(int page) {
  nav_.SetCurSel(page);
  ShowPage(page);
}

LRESULT SettingsDialog::OnProviderChange(WORD, WORD, HWND, BOOL&) {
  if (loading_profile_ || profile_sel_ < 0)
    return 0;
  ModelProfile& p = profiles_[profile_sel_];
  p.remote = IsDlgButtonChecked(IDC_P5_REMOTE) == BST_CHECKED;
  if (p.remote && p.api_url.empty()) {
    p.api_url = kDefaultApiUrl;
    loading_profile_ = true;
    GetDlgItem(IDC_P5_API_URL).SetWindowTextW(p.api_url.c_str());
    loading_profile_ = false;
  }
  ShowPage(page_);
  RefreshProfileCombos();
  if (loaded_)
    llm_modified_ = true;
  return 0;
}

// 選項按鈕的文字另做成可點的標籤（深色主題不會把選項按鈕的文字畫成淺色）
LRESULT SettingsDialog::OnProviderLabelClick(WORD, WORD id, HWND, BOOL& handled) {
  if (!GetDlgItem(IDC_P5_LOCAL).IsWindowEnabled())
    return 0;
  CheckRadioButton(IDC_P5_LOCAL, IDC_P5_REMOTE,
                   id == IDC_P5_REMOTE_LABEL ? IDC_P5_REMOTE : IDC_P5_LOCAL);
  return OnProviderChange(0, 0, NULL, handled);
}

LRESULT SettingsDialog::OnNavChange(WORD, WORD, HWND, BOOL&) {
  ShowPage(nav_.GetCurSel());
  return 0;
}

LRESULT SettingsDialog::OnEraseBkgnd(UINT, WPARAM wParam, LPARAM, BOOL&) {
  // 右側內容區用 bg；左側導覽欄（含下方的主題選單）整欄用 nav
  CRect client, nav;
  GetClientRect(&client);
  ::FillRect((HDC)wParam, &client, bg_brush_);
  if (nav_.m_hWnd) {
    nav_.GetWindowRect(&nav);
    ScreenToClient(&nav);
    CRect column(client.left, client.top, nav.right, client.bottom);
    ::FillRect((HDC)wParam, &column, nav_brush_);
  }
  return 1;
}

LRESULT SettingsDialog::OnCtlColorDlg(UINT, WPARAM wParam, LPARAM, BOOL&) {
  ::SetBkColor((HDC)wParam, pal_->bg);
  ::SetTextColor((HDC)wParam, pal_->text);
  return (LRESULT)bg_brush_;
}

LRESULT SettingsDialog::OnCtlColorStatic(UINT, WPARAM wParam, LPARAM lParam, BOOL&) {
  HDC dc = (HDC)wParam;
  const int id = ::GetDlgCtrlID((HWND)lParam);
  if (id == IDC_SEPARATOR) {
    ::SetBkColor(dc, pal_->separator);
    return (LRESULT)separator_brush_;
  }
  if (id == IDC_P1_HOTKEYS) {  // 唯讀輸入框也走 WM_CTLCOLORSTATIC
    ::SetBkColor(dc, pal_->input);
    ::SetTextColor(dc, pal_->text);
    return (LRESULT)input_brush_;
  }
  const bool on_nav = id == IDC_THEME_LABEL;
  ::SetBkColor(dc, on_nav ? pal_->nav : pal_->bg);
  ::SetTextColor(dc, IsSubtleText(id) ? pal_->subtle : pal_->text);
  return (LRESULT)(on_nav ? nav_brush_ : bg_brush_);
}

LRESULT SettingsDialog::OnCtlColorEdit(UINT, WPARAM wParam, LPARAM, BOOL&) {
  ::SetBkColor((HDC)wParam, pal_->input);
  ::SetTextColor((HDC)wParam, pal_->text);
  return (LRESULT)input_brush_;
}

LRESULT SettingsDialog::OnCtlColorListBox(UINT, WPARAM wParam, LPARAM lParam, BOOL&) {
  // 導覽欄用 nav 色；其他清單（含下拉選單展開的清單）用輸入框底色
  const bool nav = (HWND)lParam == nav_.m_hWnd;
  ::SetBkColor((HDC)wParam, nav ? pal_->nav : pal_->input);
  ::SetTextColor((HDC)wParam, pal_->text);
  return (LRESULT)(nav ? nav_brush_ : input_brush_);
}

LRESULT SettingsDialog::OnMeasureItem(UINT, WPARAM, LPARAM lParam, BOOL& handled) {
  auto* mis = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
  if (mis->CtlID != IDC_NAV) {
    handled = FALSE;
    return 0;
  }
  mis->itemHeight = Scale(40);
  return TRUE;
}

LRESULT SettingsDialog::OnDrawItem(UINT, WPARAM, LPARAM lParam, BOOL& handled) {
  auto* d = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
  if (d->CtlID != IDC_NAV) {
    handled = FALSE;
    return 0;
  }
  if (d->itemID == (UINT)-1)
    return TRUE;
  CDCHandle dc(d->hDC);
  CRect rc(d->rcItem);
  dc.FillRect(&rc, nav_brush_);

  const bool selected = (d->itemState & ODS_SELECTED) != 0;
  if (selected) {
    CRect pill(rc);
    pill.DeflateRect(Scale(6), Scale(3));
    HBRUSH sel = ::CreateSolidBrush(pal_->nav_selected);
    HBRUSH old_brush = dc.SelectBrush(sel);
    HPEN old_pen = dc.SelectPen((HPEN)::GetStockObject(NULL_PEN));
    dc.RoundRect(pill.left, pill.top, pill.right, pill.bottom, Scale(8), Scale(8));
    dc.SelectPen(old_pen);
    dc.SelectBrush(old_brush);
    ::DeleteObject(sel);
    CRect bar(pill.left, pill.top + Scale(9), pill.left + Scale(3), pill.bottom - Scale(9));
    HBRUSH accent = ::CreateSolidBrush(pal_->accent);
    dc.FillRect(&bar, accent);
    ::DeleteObject(accent);
  }

  dc.SetBkMode(TRANSPARENT);
  if (d->itemID < (UINT)kPageCount) {
    HFONT old_font = dc.SelectFont(icon_font_);
    dc.SetTextColor(selected ? pal_->accent : pal_->subtle);
    CRect icon_rc(rc.left + Scale(14), rc.top, rc.left + Scale(38), rc.bottom);
    dc.DrawText(kPages[d->itemID].icon, -1, &icon_rc,
                DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
    dc.SelectFont(nav_font_);
    dc.SetTextColor(pal_->text);
    CRect text_rc(rc.left + Scale(44), rc.top, rc.right - Scale(6), rc.bottom);
    dc.DrawText(kPages[d->itemID].nav, -1, &text_rc,
                DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
    dc.SelectFont(old_font);
  }
  return TRUE;
}

void SettingsDialog::SetStatus(const std::wstring& text) {
  GetDlgItem(IDC_STATUS).SetWindowTextW(text.c_str());
}

// ---------------------------------------------------------------------------
// 輸入方案（沿用原 SwitcherSettingsDialog 的邏輯）

void SettingsDialog::PopulateSchemas() {
  if (!switcher_settings_)
    return;
  RimeSchemaList available = {0};
  api_->get_available_schema_list(switcher_settings_, &available);
  RimeSchemaList selected = {0};
  api_->get_selected_schema_list(switcher_settings_, &selected);
  schema_list_.DeleteAllItems();
  int k = 0;
  std::set<RimeSchemaInfo*> recruited;
  for (size_t i = 0; i < selected.size; ++i) {
    const char* schema_id = selected.list[i].schema_id;
    for (size_t j = 0; j < available.size; ++j) {
      RimeSchemaListItem& item(available.list[j]);
      RimeSchemaInfo* info = (RimeSchemaInfo*)item.reserved;
      if (!strcmp(item.schema_id, schema_id) && recruited.find(info) == recruited.end()) {
        recruited.insert(info);
        schema_list_.AddItem(k, 0, u8tow(item.name).c_str());
        schema_list_.SetItemData(k, (DWORD_PTR)info);
        schema_list_.SetCheckState(k, TRUE);
        ++k;
        break;
      }
    }
  }
  for (size_t i = 0; i < available.size; ++i) {
    RimeSchemaListItem& item(available.list[i]);
    RimeSchemaInfo* info = (RimeSchemaInfo*)item.reserved;
    if (recruited.find(info) == recruited.end()) {
      recruited.insert(info);
      schema_list_.AddItem(k, 0, u8tow(item.name).c_str());
      schema_list_.SetItemData(k, (DWORD_PTR)info);
      ++k;
    }
  }
  if (const char* hotkeys = api_->get_hotkeys(switcher_settings_))
    GetDlgItem(IDC_P1_HOTKEYS).SetWindowTextW(u8tow(hotkeys).c_str());
  if (k > 0) {
    schema_list_.SelectItem(0);
    ShowSchemaDetails((RimeSchemaInfo*)schema_list_.GetItemData(0));
  }
  schemas_modified_ = false;
}

void SettingsDialog::ShowSchemaDetails(RimeSchemaInfo* info) {
  if (!info)
    return;
  std::string details;
  if (const char* name = api_->get_schema_name(info))
    details += name;
  if (const char* author = api_->get_schema_author(info))
    (details += "\n\n") += author;
  if (const char* description = api_->get_schema_description(info))
    (details += "\n\n") += description;
  GetDlgItem(IDC_P1_SCHEMA_DESC).SetWindowTextW(u8tow(details.c_str()).c_str());
}

LRESULT SettingsDialog::OnSchemaListItemChanged(int, LPNMHDR p, BOOL&) {
  LPNMLISTVIEW lv = reinterpret_cast<LPNMLISTVIEW>(p);
  if (!loaded_ || !lv || lv->iItem < 0 || lv->iItem >= schema_list_.GetItemCount())
    return 0;
  if ((lv->uNewState & LVIS_STATEIMAGEMASK) != (lv->uOldState & LVIS_STATEIMAGEMASK)) {
    schemas_modified_ = true;
  } else if ((lv->uNewState & LVIS_SELECTED) && !(lv->uOldState & LVIS_SELECTED)) {
    ShowSchemaDetails((RimeSchemaInfo*)(schema_list_.GetItemData(lv->iItem)));
  }
  return 0;
}

LRESULT SettingsDialog::OnGetSchemata(WORD, WORD, HWND hWndCtl, BOOL&) {
  HKEY hKey;
  std::wstring hPath =
      is_wow64() ? L"Software\\WOW6432Node\\Rime\\Weasel" : L"Software\\Rime\\Weasel";
  if (RegOpenKey(HKEY_LOCAL_MACHINE, hPath.c_str(), &hKey) != ERROR_SUCCESS)
    return 0;
  WCHAR value[MAX_PATH];
  DWORD len = sizeof(value);
  DWORD type = 0;
  if (RegQueryValueExW(hKey, L"WeaselRoot", NULL, &type, (LPBYTE)value, &len) ==
          ERROR_SUCCESS &&
      type == REG_SZ) {
    std::wstring parameters = std::wstring(L"/k \"") + value + L"\\rime-install.bat\"";
    SHELLEXECUTEINFOW cmd = {sizeof(SHELLEXECUTEINFO)};
    cmd.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    cmd.hwnd = hWndCtl;
    cmd.lpVerb = L"open";
    cmd.lpFile = L"cmd";
    cmd.lpParameters = parameters.c_str();
    cmd.nShow = SW_SHOW;
    if (ShellExecuteExW(&cmd) && cmd.hProcess) {
      WaitForSingleObject(cmd.hProcess, INFINITE);
      CloseHandle(cmd.hProcess);
    }
    api_->load_settings(reinterpret_cast<RimeCustomSettings*>(switcher_settings_));
    PopulateSchemas();
  }
  RegCloseKey(hKey);
  return 0;
}

// ---------------------------------------------------------------------------
// 外觀（沿用原 UIStyleSettingsDialog 的邏輯）

void SettingsDialog::PopulateColorSchemes() {
  if (!ui_settings_)
    return;
  const std::string active(ui_settings_->GetActiveColorScheme());
  int active_index = -1;
  ui_settings_->GetPresetColorSchemes(&preset_);
  color_schemes_.ResetContent();
  for (size_t i = 0; i < preset_.size(); ++i) {
    color_schemes_.AddString(u8tow(preset_[i].name).c_str());
    if (preset_[i].color_scheme_id == active)
      active_index = (int)i;
  }
  if (active_index >= 0) {
    color_schemes_.SetCurSel(active_index);
    PreviewColorScheme(active_index);
  }
}

void SettingsDialog::PreviewColorScheme(int index) {
  if (index < 0 || index >= (int)preset_.size())
    return;
  const std::string file_path(ui_settings_->GetColorSchemePreview(preset_[index].color_scheme_id));
  if (file_path.empty())
    return;
  preview_image_.Destroy();
  preview_image_.Load(acptow(file_path).c_str());  // 路徑是 ANSI 編碼，不是 UTF-8
  if (!preview_image_.IsNull())
    preview_.SetBitmap(preview_image_);
}

LRESULT SettingsDialog::OnColorSchemeChange(WORD, WORD, HWND, BOOL&) {
  const int index = color_schemes_.GetCurSel();
  if (index >= 0 && index < (int)preset_.size()) {
    ui_settings_->SelectColorScheme(preset_[index].color_scheme_id);
    PreviewColorScheme(index);
    style_modified_ = true;
  }
  return 0;
}

void SettingsDialog::UpdateFontSummary() {
  auto describe = [](const std::wstring& face, int point) {
    std::wstring name = face.empty() ? L"預設字體" : face.substr(0, face.find(L','));
    return name + L"，" + std::to_wstring(point) + L" pt";
  };
  std::wstring text = L"候選字：" + describe(ui_settings_->font_face, ui_settings_->font_point) +
                      L"　標籤：" + std::to_wstring(ui_settings_->label_font_point) +
                      L" pt　註解：" + std::to_wstring(ui_settings_->comment_font_point) + L" pt";
  GetDlgItem(IDC_P2_FONT_SUMMARY).SetWindowTextW(text.c_str());
}

LRESULT SettingsDialog::OnSelectFont(WORD, WORD, HWND, BOOL&) {
  FontSettingDialog dialog(ui_settings_, m_hWnd);
  if (dialog.ShowDialog() == IDOK) {
    ui_settings_->SetFontFace("style/font_face", wtou8(dialog.m_font_face));
    ui_settings_->SetFontFace("style/label_font_face", wtou8(dialog.m_label_font_face));
    ui_settings_->SetFontFace("style/comment_font_face", wtou8(dialog.m_comment_font_face));
    ui_settings_->SetFontPoint("style/font_point", dialog.m_font_point);
    ui_settings_->SetFontPoint("style/label_font_point", dialog.m_label_font_point);
    ui_settings_->SetFontPoint("style/comment_font_point", dialog.m_comment_font_point);
    ui_settings_->font_face = dialog.m_font_face;
    ui_settings_->label_font_face = dialog.m_label_font_face;
    ui_settings_->comment_font_face = dialog.m_comment_font_face;
    ui_settings_->font_point = dialog.m_font_point;
    ui_settings_->label_font_point = dialog.m_label_font_point;
    ui_settings_->comment_font_point = dialog.m_comment_font_point;
    UpdateFontSummary();
    style_modified_ = true;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// LLM 智慧預測

void SettingsDialog::LoadLLMSettings() {
  RimeApi* rime = rime_get_api();
  RimeConfig config = {0};
  api_->settings_get_config(ui_settings_->settings(), &config);
  auto get_bool = [&](const char* key, bool fallback) {
    Bool value = fallback;
    return rime->config_get_bool(&config, key, &value) ? !!value : fallback;
  };
  auto get_string = [&](const char* key) {
    char buffer[2048] = {0};
    return rime->config_get_string(&config, key, buffer, sizeof(buffer) - 1) ? u8tow(buffer)
                                                                             : std::wstring();
  };
  CheckDlgButton(IDC_P3_ENABLED, get_bool("llm/enabled", false) ? BST_CHECKED : BST_UNCHECKED);
  CheckDlgButton(IDC_P3_AFTER_COMMIT,
                 get_bool("llm/predict_after_commit", true) ? BST_CHECKED : BST_UNCHECKED);
  CheckDlgButton(IDC_P3_WHILE_TYPING,
                 get_bool("llm/predict_while_typing", true) ? BST_CHECKED : BST_UNCHECKED);

  LoadProfiles(&config);

  // 提示詞（Base 與 Instruct 共用）：llm/prompt，沒有時讀舊的 llm/llamacpp/prompt_prefix。
  // 設定裡用 \n 換行，編輯框要 \r\n；結尾的空行只是和前文隔開，不顯示
  std::wstring prefix = get_string("llm/prompt");
  if (prefix.empty())
    prefix = get_string("llm/llamacpp/prompt_prefix");
  while (!prefix.empty() && (prefix.back() == L'\n' || prefix.back() == L'\r'))
    prefix.pop_back();
  std::wstring display;
  for (wchar_t c : prefix) {
    if (c == L'\n')
      display += L"\r\n";
    else if (c != L'\r')
      display += c;
  }
  GetDlgItem(IDC_P3_PREFIX).SetWindowTextW(display.c_str());
  UpdateLLMEnableState();
  LoadPersonalSettings(&config);
  llm_modified_ = false;
}

int SettingsDialog::AddModel(const std::wstring& path) {
  const std::wstring key = ToLower(path);
  for (size_t i = 0; i < model_paths_.size(); ++i) {
    if (ToLower(model_paths_[i]) == key)
      return (int)i;
  }
  model_paths_.push_back(path);
  models_.AddString(FileNameOf(path).c_str());
  return (int)model_paths_.size() - 1;
}

void SettingsDialog::PopulateModels(const std::wstring& current) {
  models_.ResetContent();
  model_paths_.clear();
  std::vector<fs::path> dirs;
  if (!current.empty())
    dirs.push_back(fs::path(current).parent_path());
  wchar_t profile[MAX_PATH] = {0};
  if (GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH))
    dirs.push_back(fs::path(profile) / L"models");
  std::vector<std::wstring> found;
  for (const auto& dir : dirs) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
      if (entry.is_regular_file(ec) && ToLower(entry.path().extension().wstring()) == L".gguf")
        found.push_back(entry.path().wstring());
    }
  }
  std::sort(found.begin(), found.end(),
            [](const std::wstring& a, const std::wstring& b) { return ToLower(FileNameOf(a)) < ToLower(FileNameOf(b)); });
  for (const auto& path : found)
    AddModel(path);
  if (!current.empty())
    models_.SetCurSel(AddModel(current));
}

void SettingsDialog::UpdateLLMEnableState() {
  const bool enabled = IsDlgButtonChecked(IDC_P3_ENABLED) == BST_CHECKED;
  for (int id : {IDC_P3_AFTER_COMMIT, IDC_P3_WHILE_TYPING, IDC_P3_MODEL_LABEL, IDC_P3_PROFILE,
                 IDC_P3_PREFIX_LABEL, IDC_P3_PREFIX})
    GetDlgItem(id).EnableWindow(enabled);
}

LRESULT SettingsDialog::OnLLMEnabledClick(WORD, WORD, HWND, BOOL&) {
  UpdateLLMEnableState();
  llm_modified_ = true;
  return 0;
}

LRESULT SettingsDialog::OnLLMChanged(WORD, WORD, HWND, BOOL&) {
  if (loaded_)
    llm_modified_ = true;
  return 0;
}

LRESULT SettingsDialog::OnModelChange(WORD, WORD, HWND, BOOL&) {
  if (loading_profile_)
    return 0;
  const int sel = models_.GetCurSel();
  if (sel >= 0 && sel < (int)model_paths_.size()) {
    if (const wchar_t* type = GuessModelType(model_paths_[sel]))
      model_type_.SetCurSel(wcscmp(type, L"Base") == 0 ? 0 : 1);
  }
  CommitProfileEditor();
  RefreshProfileCombos();
  llm_modified_ = true;
  return 0;
}

LRESULT SettingsDialog::OnBrowseModel(WORD, WORD, HWND, BOOL&) {
  CFileDialog dialog(TRUE, L"gguf", NULL, OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER,
                     L"GGUF 模型 (*.gguf)\0*.gguf\0所有檔案 (*.*)\0*.*\0", m_hWnd);
  if (dialog.DoModal() == IDOK) {
    models_.SetCurSel(AddModel(dialog.m_szFileName));
    BOOL handled = TRUE;
    OnModelChange(0, 0, NULL, handled);
  }
  return 0;
}

bool SettingsDialog::SaveLLMSettings() {
  RimeApi* rime = rime_get_api();
  RimeConfig config = {0};
  api_->settings_get_config(ui_settings_->settings(), &config);
  // 以目前的 llm 設定為底（保留 prompt_prefix、n_ctx 等介面上沒有的項目），只改介面上的值
  RimeConfig llm = {0};
  if (!rime->config_get_item(&config, "llm", &llm))
    rime->config_init(&llm);
  rime->config_set_bool(&llm, "enabled", IsDlgButtonChecked(IDC_P3_ENABLED) == BST_CHECKED);
  rime->config_set_bool(&llm, "predict_after_commit",
                        IsDlgButtonChecked(IDC_P3_AFTER_COMMIT) == BST_CHECKED);
  rime->config_set_bool(&llm, "predict_while_typing",
                        IsDlgButtonChecked(IDC_P3_WHILE_TYPING) == BST_CHECKED);
  auto get_text = [&](int id) {
    CString text;
    GetDlgItem(id).GetWindowTextW(text);
    return LLMTrim(std::wstring((LPCWSTR)text));
  };
  SaveProfiles(&llm);

  // 提示詞（兩種模式共用）：統一換行為 \n；改存 llm/prompt，移除舊的 llamacpp/prompt_prefix
  CString text;
  GetDlgItem(IDC_P3_PREFIX).GetWindowTextW(text);
  std::wstring prefix;
  for (const wchar_t* p = text; *p; ++p) {
    if (*p != L'\r')
      prefix += *p;
  }
  rime->config_set_string(&llm, "prompt", wtou8(LLMTrim(prefix)).c_str());
  rime->config_clear(&llm, "llamacpp/prompt_prefix");
  SavePersonalSettings(&llm);
  const bool ok = !!api_->customize_item(ui_settings_->settings(), "llm", &llm);
  rime->config_close(&llm);
  return ok;
}

// ---------------------------------------------------------------------------
// 預測測試：寫入請求檔 → 通知輸入法 → 輪詢回覆檔（不等待輸入法的鎖，避免卡住）

void SettingsDialog::SendLLMRequest(const std::wstring& context, bool is_test) {
  ++request_id_;
  request_is_test_ = is_test;
  std::error_code ec;
  fs::remove(ResponseFile(), ec);
  {
    std::ofstream out(RequestFile(), std::ios::binary | std::ios::trunc);
    out << request_id_ << "\n" << wtou8(context);
  }
  weasel::Client client;
  if (!client.Connect()) {
    GetDlgItem(IDC_P3_LOADED).SetWindowTextW(L"無法連線到輸入法服務。");
    if (is_test)
      GetDlgItem(IDC_P3_TEST_STATUS).SetWindowTextW(L"無法連線到輸入法服務，請確認小狼毫正在執行。");
    return;
  }
  client.LLMTestRequest();
  request_started_ = GetTickCount64();
  if (is_test) {
    GetDlgItem(IDC_P3_TEST_RUN).EnableWindow(FALSE);
    GetDlgItem(IDC_P3_TEST_STATUS).SetWindowTextW(L"預測中…");
    test_result_.ResetContent();
  }
  SetTimer(kTimerTestPoll, 150);
}

bool SettingsDialog::PollLLMResponse() {
  std::string text;
  {
    std::ifstream in(ResponseFile(), std::ios::binary);
    if (!in)
      return false;
    text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  std::string id, status, model, ms;
  std::vector<std::wstring> candidates;
  std::istringstream lines(text);
  for (std::string line; std::getline(lines, line);) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    const size_t eq = line.find('=');
    if (eq == std::string::npos)
      continue;
    const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
    if (key == "id") id = value;
    else if (key == "status") status = value;
    else if (key == "model") model = value;
    else if (key == "ms") ms = value;
    else if (key == "cand") candidates.push_back(u8tow(value));
  }
  if (id != std::to_string(request_id_))
    return false;  // 還沒輪到這次的回覆

  KillTimer(kTimerTestPoll);
  const std::wstring model_name = LoadedModelDisplay(u8tow(model));
  GetDlgItem(IDC_P3_LOADED)
      .SetWindowTextW(status == "disabled"
                          ? L"輸入法目前沒有載入 LLM 模型（LLM 智慧預測已關閉）。"
                          : (L"輸入法目前載入：" + model_name).c_str());
  if (request_is_test_) {
    GetDlgItem(IDC_P3_TEST_RUN).EnableWindow(TRUE);
    if (status == "disabled") {
      GetDlgItem(IDC_P3_TEST_STATUS)
          .SetWindowTextW(L"LLM 智慧預測目前關閉，無法測試。請先啟用並按「套用」。");
    } else {
      for (size_t i = 0; i < candidates.size(); ++i)
        test_result_.AddString((std::to_wstring(i + 1) + L".  " + candidates[i]).c_str());
      std::wstring summary = candidates.empty() ? L"沒有產生候選。" : L"";
      summary += L"耗時 " + u8tow(ms) + L" ms（" + model_name + L"）";
      GetDlgItem(IDC_P3_TEST_STATUS).SetWindowTextW(summary.c_str());
    }
  }
  return true;
}

LRESULT SettingsDialog::OnTestRun(WORD, WORD, HWND, BOOL&) {
  CString text;
  GetDlgItem(IDC_P3_TEST_INPUT).GetWindowTextW(text);
  if (text.IsEmpty()) {
    GetDlgItem(IDC_P3_TEST_STATUS).SetWindowTextW(L"請先輸入一段前文。");
    return 0;
  }
  KillTimer(kTimerStatusPoll);
  SendLLMRequest((LPCWSTR)text, true);
  return 0;
}

LRESULT SettingsDialog::OnTimer(UINT, WPARAM wParam, LPARAM, BOOL& handled) {
  if (wParam == kTimerTestPoll) {
    if (!PollLLMResponse() && GetTickCount64() - request_started_ > 30000) {
      KillTimer(kTimerTestPoll);
      GetDlgItem(IDC_P3_TEST_RUN).EnableWindow(TRUE);
      const wchar_t* msg = L"輸入法沒有回應（可能正在載入模型），請稍後再試。";
      GetDlgItem(request_is_test_ ? IDC_P3_TEST_STATUS : IDC_P3_LOADED).SetWindowTextW(msg);
    }
    return 0;
  }
  if (wParam == kTimerPersonalPoll) {
    // 每 5 秒請輸入法重寫一次狀態（詞數會隨打字變動）；精煉中每秒讀檔更新
    if (++personal_ticks_ % 5 == 0)
      SendPersonalCommand(1);
    RefreshPersonalStatus();
    return 0;
  }
  if (wParam == kTimerStatusPoll) {
    // 重新部署後，輸入法會重新載入模型；每秒詢問一次目前載入的模型，更新顯示
    if (--status_polls_left_ <= 0)
      KillTimer(kTimerStatusPoll);
    SendLLMRequest(L"", false);
    return 0;
  }
  handled = FALSE;
  return 0;
}

// ---------------------------------------------------------------------------
// 儲存

bool SettingsDialog::Save() {
  bool saved = false;
  if (schemas_modified_ && schema_list_.GetItemCount() > 0) {
    std::vector<const char*> selection;
    for (int i = 0; i < schema_list_.GetItemCount(); ++i) {
      if (!schema_list_.GetCheckState(i))
        continue;
      if (RimeSchemaInfo* info = (RimeSchemaInfo*)schema_list_.GetItemData(i))
        selection.push_back(api_->get_schema_id(info));
    }
    if (selection.empty()) {
      nav_.SetCurSel(0);
      ShowPage(0);
      MSG_BY_IDS(IDS_STR_ERR_AT_LEAST_ONE_SEL, IDS_STR_NOT_REGULAR, MB_OK | MB_ICONEXCLAMATION);
      return false;
    }
    api_->select_schemas(switcher_settings_, selection.data(), (int)selection.size());
    api_->save_settings((RimeCustomSettings*)switcher_settings_);
    schemas_modified_ = false;
    saved = true;
  }
  CommitProfileEditor();
  if ((llm_modified_ || personal_modified_) && !ValidateProfiles())
    return false;
  if (personal_modified_)
    llm_modified_ = true;  // 個人詞庫的設定也在 llm 之下，一起儲存
  if (llm_modified_ && !SaveLLMSettings()) {
    SetStatus(L"LLM 設定儲存失敗。");
    return false;
  }
  // 注音排序有變更：修改注音方案並準備詞典（隨後的重新部署會編譯）
  const bool rime_boost = IsDlgButtonChecked(IDC_P4_ENABLED) == BST_CHECKED &&
                          IsDlgButtonChecked(IDC_P4_RIME_BOOST) == BST_CHECKED;
  std::wstring boost_error;
  if (llm_modified_ && rime_boost != rime_boost_loaded_) {
    ApplyRimeBoost(rime_boost, &boost_error);
    rime_boost_loaded_ = rime_boost;
  }
  if (style_modified_ || llm_modified_) {
    api_->save_settings(ui_settings_->settings());
    style_modified_ = llm_modified_ = personal_modified_ = false;
    saved = true;
  }
  if (!saved) {
    SetStatus(L"沒有需要套用的變更。");
    return true;
  }
  SetStatus(L"正在重新部署…");
  {
    CWaitCursor wait;
    deploy_();
  }
  deployed_ = true;
  SetStatus(boost_error.empty() ? L"已套用。" : L"已套用；注音排序：" + boost_error);
  // 等輸入法重新載入模型後更新「目前載入」
  status_polls_left_ = 15;
  SetTimer(kTimerStatusPoll, 1000);
  return true;
}

// ---------------------------------------------------------------------------
// 個人詞庫

void SettingsDialog::LoadPersonalSettings(RimeConfig* config) {
  RimeApi* rime = rime_get_api();
  auto get_bool = [&](const char* key, bool fallback) {
    Bool value = fallback;
    return rime->config_get_bool(config, key, &value) ? !!value : fallback;
  };
  auto get_string = [&](const char* key) {
    char buffer[2048] = {0};
    return rime->config_get_string(config, key, buffer, sizeof(buffer) - 1) ? u8tow(buffer)
                                                                            : std::wstring();
  };
  auto get_int = [&](const char* key, int fallback) {
    int value = fallback;
    return rime->config_get_int(config, key, &value) ? value : fallback;
  };
  CheckDlgButton(IDC_P4_ENABLED,
                 get_bool("llm/personal/enabled", true) ? BST_CHECKED : BST_UNCHECKED);
  CheckDlgButton(IDC_P4_KEEP_LOG,
                 get_bool("llm/personal/keep_raw_log", true) ? BST_CHECKED : BST_UNCHECKED);
  rime_boost_loaded_ = get_bool("llm/personal/rime_boost", false);
  CheckDlgButton(IDC_P4_RIME_BOOST, rime_boost_loaded_ ? BST_CHECKED : BST_UNCHECKED);
  const int max = get_int("llm/personal/max_candidates", 3);
  CComboBox(GetDlgItem(IDC_P4_MAX)).SetCurSel((std::max)(0, (std::min)(max, 5)));
  const int half_life = get_int("llm/personal/half_life_days", 30);
  GetDlgItem(IDC_P4_HALFLIFE).SetWindowTextW(std::to_wstring(half_life > 0 ? half_life : 30).c_str());
  std::wstring interval = get_string("llm/personal/refine/interval_days");
  if (interval.empty())
    interval = L"1";
  GetDlgItem(IDC_P4_INTERVAL).SetWindowTextW(interval.c_str());
  UpdatePersonalEnableState();
  personal_modified_ = false;
}

void SettingsDialog::SavePersonalSettings(RimeConfig* llm) {
  RimeApi* rime = rime_get_api();
  auto get_text = [&](int id) {
    CString text;
    GetDlgItem(id).GetWindowTextW(text);
    return LLMTrim(std::wstring((LPCWSTR)text));
  };
  auto get_number = [&](int id, int fallback) {
    const std::wstring text = get_text(id);
    return text.empty() ? fallback : _wtoi(text.c_str());
  };
  rime->config_set_bool(llm, "personal/enabled", IsDlgButtonChecked(IDC_P4_ENABLED) == BST_CHECKED);
  rime->config_set_bool(llm, "personal/keep_raw_log",
                        IsDlgButtonChecked(IDC_P4_KEEP_LOG) == BST_CHECKED);
  rime->config_set_bool(llm, "personal/rime_boost",
                        IsDlgButtonChecked(IDC_P4_ENABLED) == BST_CHECKED &&
                            IsDlgButtonChecked(IDC_P4_RIME_BOOST) == BST_CHECKED);
  const int max = CComboBox(GetDlgItem(IDC_P4_MAX)).GetCurSel();
  rime->config_set_int(llm, "personal/max_candidates", max < 0 ? 3 : max);
  const int half_life = get_number(IDC_P4_HALFLIFE, 30);
  rime->config_set_int(llm, "personal/half_life_days", half_life > 0 ? half_life : 30);
  rime->config_set_int(llm, "personal/refine/interval_days",
                       (std::max)(0, get_number(IDC_P4_INTERVAL, 1)));
}

void SettingsDialog::UpdatePersonalEnableState() {
  const bool enabled = IsDlgButtonChecked(IDC_P4_ENABLED) == BST_CHECKED;
  for (int id : {IDC_P4_MAX_LABEL, IDC_P4_MAX, IDC_P4_HALFLIFE_LABEL, IDC_P4_HALFLIFE,
                 IDC_P4_KEEP_LOG, IDC_P4_RIME_BOOST, IDC_P4_INTERVAL_LABEL, IDC_P4_INTERVAL, IDC_P4_PROFILE_LABEL,
                 IDC_P4_PROFILE})
    GetDlgItem(id).EnableWindow(enabled);
  // 精煉按鈕要輸入法那邊的個人詞庫開著；清除永遠可以
  GetDlgItem(IDC_P4_REFINE).EnableWindow(enabled && !personal_running_);
  GetDlgItem(IDC_P4_REFINE_ALL).EnableWindow(enabled && !personal_running_);
  GetDlgItem(IDC_P4_CLEAR).EnableWindow(!personal_running_);
}

LRESULT SettingsDialog::OnPersonalEnabledClick(WORD, WORD, HWND, BOOL&) {
  UpdatePersonalEnableState();
  personal_modified_ = true;
  return 0;
}

LRESULT SettingsDialog::OnPersonalChanged(WORD, WORD, HWND, BOOL&) {
  if (loaded_)
    personal_modified_ = true;
  return 0;
}

bool SettingsDialog::SendPersonalCommand(DWORD command) {
  weasel::Client client;
  if (!client.Connect())
    return false;
  client.PersonalCommand(command);
  return true;
}

void SettingsDialog::RefreshPersonalStatus() {
  std::map<std::string, std::string> v;
  {
    std::ifstream in(PersonalStatusFile(), std::ios::binary);
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
  if (v.empty()) {
    text << L"無法取得狀態：請確認小狼毫正在執行。";
    personal_running_ = false;
  } else if (v.count("disabled")) {
    text << L"個人詞庫目前關閉。勾選上方的「啟用個人詞庫」並按「套用」即可開始學習。";
    personal_running_ = false;
  } else {
    personal_running_ = v["running"] == "1";
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
    if (personal_running_)
      text << L"精煉中…" << (v["progress"].empty() ? L"" : L"（" + u8tow(v["progress"]) + L"）");
    else if (!v["last_result"].empty())
      text << L"上次結果：" << u8tow(v["last_result"]);
  }
  CString current;
  GetDlgItem(IDC_P4_STATUS).GetWindowTextW(current);
  const std::wstring next = text.str();
  // 內容沒變就不重設，避免每秒閃爍
  std::wstring display;
  for (wchar_t c : next) {
    if (c == L'\n')
      display += L"\r\n";
    else
      display += c;
  }
  if (display != (LPCWSTR)current)
    GetDlgItem(IDC_P4_STATUS).SetWindowTextW(display.c_str());
  UpdatePersonalEnableState();
}

LRESULT SettingsDialog::OnPersonalCommand(WORD, WORD id, HWND, BOOL&) {
  if (id == IDC_P4_CLEAR) {
    if (MessageBoxW(L"確定要清除個人詞庫的所有資料嗎？\n\n包含學到的詞、原始輸入紀錄與所有封存，清除後無法復原。",
                    L"清除個人詞庫", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
      return 0;
  } else if (personal_modified_) {
    SetStatus(L"個人詞庫的設定有變更，請先按「套用」再精煉。");
    return 0;
  }
  const DWORD command = id == IDC_P4_REFINE ? 2 : id == IDC_P4_REFINE_ALL ? 3 : 4;
  if (!SendPersonalCommand(command)) {
    SetStatus(L"無法連線到輸入法服務。");
    return 0;
  }
  SetStatus(command == 4 ? L"已清除個人詞庫。" : L"已開始精煉，完成後會顯示結果。");
  words_loaded_ = false;  // 詞庫管理頁下次顯示時重新讀取
  // 輸入法回覆前先停用按鈕，避免連按
  if (command != 4)
    personal_running_ = true;
  UpdatePersonalEnableState();
  Sleep(100);
  RefreshPersonalStatus();
  return 0;
}

// ---------------------------------------------------------------------------
// 語言模型（多組設定）
//
// 設定存在 llm/profiles/pNN/{name,type,model_path,model_type,api_url,api_key,model}；
// llm/predict_profile 與 llm/personal/refine/profile 記住各自選用的 pNN。
// 為了讓輸入法不必理解多組設定，儲存時也把選到的那組展開到原本的欄位
// （llm/provider_type、llm/llamacpp/*、llm/openai/*，以及 llm/personal/refine/*）。

std::wstring SettingsDialog::ProfileLabel(const ModelProfile& p) {
  return (p.name.empty() ? std::wstring(L"（未命名）") : p.name) +
         (p.remote ? L"　［API］" : L"　［本機］");
}

void SettingsDialog::LoadProfiles(RimeConfig* config) {
  RimeApi* rime = rime_get_api();
  auto get = [&](const std::string& key) {
    char buffer[2048] = {0};
    return rime->config_get_string(config, key.c_str(), buffer, sizeof(buffer) - 1)
               ? u8tow(buffer)
               : std::wstring();
  };
  auto to_windows_path = [](std::wstring path) {
    std::replace(path.begin(), path.end(), L'/', L'\\');
    return path;
  };
  profiles_.clear();
  std::vector<std::string> keys;
  RimeConfigIterator it = {0};
  if (rime->config_begin_map(&it, config, "llm/profiles")) {
    while (rime->config_next(&it))
      keys.push_back(it.key);
    rime->config_end(&it);
  }
  std::sort(keys.begin(), keys.end());
  std::map<std::wstring, int> index_of;
  for (const auto& key : keys) {
    const std::string base = "llm/profiles/" + key + "/";
    ModelProfile p;
    p.name = get(base + "name");
    p.remote = ToLower(get(base + "type")) == L"openai";
    p.model_path = to_windows_path(get(base + "model_path"));
    p.model_type = ToLower(get(base + "model_type")) == L"instruct" ? L"Instruct" : L"Base";
    p.api_url = get(base + "api_url");
    p.api_key = get(base + "api_key");
    p.model = get(base + "model");
    index_of[u8tow(key)] = (int)profiles_.size();
    profiles_.push_back(p);
  }

  int predict = -1, refine = -1;
  if (profiles_.empty()) {
    // 舊設定轉換：本機模型、預測用的 API、精煉用的 API 各一組
    const std::wstring provider = ToLower(get("llm/provider_type"));
    const std::wstring path = to_windows_path(get("llm/llamacpp/model_path"));
    if (!path.empty()) {
      ModelProfile p;
      p.name = fs::path(path).stem().wstring();
      p.model_path = path;
      // 未設定時 LlamaCppProvider 預設 Instruct
      p.model_type = ToLower(get("llm/llamacpp/model_type")) == L"base" ? L"Base" : L"Instruct";
      if (provider != L"openai")
        predict = (int)profiles_.size();
      profiles_.push_back(p);
    }
    const std::wstring url = get("llm/openai/api_url");
    if (!url.empty() || provider == L"openai") {
      ModelProfile p;
      p.remote = true;
      p.api_url = url.empty() ? kDefaultApiUrl : url;
      p.api_key = get("llm/openai/api_key");
      p.model = get("llm/openai/model");
      p.name = p.model.empty() ? L"OpenAI 相容 API" : p.model;
      if (provider == L"openai")
        predict = (int)profiles_.size();
      profiles_.push_back(p);
    }
    const std::wstring refine_url = get("llm/personal/refine/api_url");
    if (!refine_url.empty()) {
      ModelProfile p;
      p.remote = true;
      p.api_url = refine_url;
      p.api_key = get("llm/personal/refine/api_key");
      p.model = get("llm/personal/refine/model");
      p.name = L"精煉用 " + (p.model.empty() ? std::wstring(L"API") : p.model);
      for (size_t i = 0; i < profiles_.size(); ++i) {
        const auto& q = profiles_[i];
        if (q.remote && q.api_url == p.api_url && q.api_key == p.api_key && q.model == p.model)
          refine = (int)i;
      }
      if (refine < 0) {
        refine = (int)profiles_.size();
        profiles_.push_back(p);
      }
    }
  } else {
    auto find = [&](const std::wstring& key) {
      auto i = index_of.find(key);
      return i == index_of.end() ? -1 : i->second;
    };
    predict = find(get("llm/predict_profile"));
    refine = find(get("llm/personal/refine/profile"));
  }
  PopulateProfileList();
  RefreshProfileCombos(predict, refine);
  SelectProfile(profiles_.empty() ? -1 : 0);
}

void SettingsDialog::SaveProfiles(RimeConfig* llm) {
  RimeApi* rime = rime_get_api();
  auto set = [&](const std::string& key, const std::wstring& value) {
    rime->config_set_string(llm, key.c_str(), wtou8(value).c_str());
  };
  auto key_of = [](int i) {
    char key[16];
    sprintf_s(key, "p%02d", i + 1);
    return std::string(key);
  };
  auto yaml_path = [](std::wstring path) {
    std::replace(path.begin(), path.end(), L'\\', L'/');
    return path;
  };
  rime->config_clear(llm, "profiles");
  for (int i = 0; i < (int)profiles_.size(); ++i) {
    const ModelProfile& p = profiles_[i];
    const std::string base = "profiles/" + key_of(i) + "/";
    set(base + "name", p.name);
    set(base + "type", p.remote ? L"openai" : L"llamacpp");
    set(base + "model_path", yaml_path(p.model_path));
    set(base + "model_type", p.model_type);
    set(base + "api_url", p.api_url);
    set(base + "api_key", p.api_key);
    set(base + "model", p.model);
  }
  // 預測：展開到輸入法讀取的欄位
  const int predict = ComboProfile(IDC_P3_PROFILE);
  set("predict_profile", predict >= 0 ? u8tow(key_of(predict)) : L"");
  if (predict >= 0) {
    const ModelProfile& p = profiles_[predict];
    set("provider_type", p.remote ? L"openai" : L"llamacpp");
    if (p.remote) {
      set("openai/api_url", p.api_url);
      set("openai/api_key", p.api_key);
      set("openai/model", p.model);
    } else {
      set("llamacpp/model_path", yaml_path(p.model_path));
      set("llamacpp/model_type", p.model_type);
    }
  }
  // 精煉
  const int refine = ComboProfile(IDC_P4_PROFILE);
  const ModelProfile none;
  const ModelProfile& r = refine >= 0 ? profiles_[refine] : none;
  set("personal/refine/profile", refine >= 0 ? u8tow(key_of(refine)) : L"");
  set("personal/refine/type", refine < 0 ? L"" : r.remote ? L"openai" : L"llamacpp");
  set("personal/refine/name", refine >= 0 ? r.name : L"");
  set("personal/refine/model_path", refine >= 0 && !r.remote ? yaml_path(r.model_path) : L"");
  set("personal/refine/model_type", refine >= 0 && !r.remote ? r.model_type : L"");
  set("personal/refine/api_url", refine >= 0 && r.remote ? r.api_url : L"");
  set("personal/refine/api_key", refine >= 0 && r.remote ? r.api_key : L"");
  set("personal/refine/model", refine >= 0 && r.remote ? r.model : L"");
}

bool SettingsDialog::ValidateProfiles() {
  auto fail = [&](int index, const wchar_t* message, int focus) {
    GoToPage(kPageModels);
    if (index >= 0) {
      SelectProfile(index);
      profile_list_.SetCurSel(index);
    }
    SetStatus(message);
    if (focus)
      GetDlgItem(focus).SetFocus();
    return false;
  };
  const bool llm_on = IsDlgButtonChecked(IDC_P3_ENABLED) == BST_CHECKED;
  const int predict = ComboProfile(IDC_P3_PROFILE);
  if (llm_on && predict < 0) {
    GoToPage(kPagePredict);
    SetStatus(L"請選擇預測使用的模型（可在「語言模型」頁新增）。");
    return false;
  }
  const int refine = ComboProfile(IDC_P4_PROFILE);
  for (int index : {llm_on ? predict : -1, refine}) {
    if (index < 0)
      continue;
    const ModelProfile& p = profiles_[index];
    if (p.remote && p.api_url.empty())
      return fail(index, L"請填寫 OpenAI 相容 API 的網址。", IDC_P5_API_URL);
    if (!p.remote && p.model_path.empty())
      return fail(index, L"請選擇模型檔。", IDC_P5_MODEL);
  }
  return true;
}

void SettingsDialog::PopulateProfileList() {
  profile_list_.ResetContent();
  for (const auto& p : profiles_)
    profile_list_.AddString(ProfileLabel(p).c_str());
  if (profile_sel_ >= 0 && profile_sel_ < (int)profiles_.size())
    profile_list_.SetCurSel(profile_sel_);
}

void SettingsDialog::SelectProfile(int index) {
  if (index >= (int)profiles_.size())
    index = (int)profiles_.size() - 1;
  profile_sel_ = index;
  loading_profile_ = true;
  const bool has = index >= 0;
  const ModelProfile empty;
  const ModelProfile& p = has ? profiles_[index] : empty;
  profile_list_.SetCurSel(index);
  GetDlgItem(IDC_P5_NAME).SetWindowTextW(p.name.c_str());
  CheckRadioButton(IDC_P5_LOCAL, IDC_P5_REMOTE, p.remote ? IDC_P5_REMOTE : IDC_P5_LOCAL);
  PopulateModels(p.model_path);
  model_type_.SetCurSel(p.model_type == L"Instruct" ? 1 : 0);
  GetDlgItem(IDC_P5_API_URL).SetWindowTextW(p.api_url.c_str());
  GetDlgItem(IDC_P5_API_KEY).SetWindowTextW(p.api_key.c_str());
  GetDlgItem(IDC_P5_API_MODEL).SetWindowTextW(p.model.c_str());
  for (int id = IDC_P5_NAME_LABEL; id <= IDC_P5_REMOTE_HINT; ++id)
    GetDlgItem(id).EnableWindow(has);
  GetDlgItem(IDC_P5_COPY).EnableWindow(has);
  GetDlgItem(IDC_P5_DELETE).EnableWindow(has);
  GetDlgItem(IDC_P5_API_TEST).EnableWindow(has && !api_busy_);
  // 說明文字（測試結果會暫時寫在這裡）
  GetDlgItem(IDC_P5_REMOTE_HINT)
      .SetWindowTextW(L"例如 https://api.openai.com/v1/chat/completions，或 Ollama 的 "
                      L"http://localhost:11434/v1/chat/completions。金鑰以明碼存在 weasel.custom.yaml。");
  loading_profile_ = false;
  if (page_ == kPageModels)
    ShowPage(page_);  // 依本機 / API 切換顯示的欄位
}

void SettingsDialog::CommitProfileEditor() {
  if (loading_profile_ || profile_sel_ < 0 || profile_sel_ >= (int)profiles_.size())
    return;
  auto get_text = [&](int id) {
    CString text;
    GetDlgItem(id).GetWindowTextW(text);
    return LLMTrim(std::wstring((LPCWSTR)text));
  };
  ModelProfile& p = profiles_[profile_sel_];
  const std::wstring old_label = ProfileLabel(p);
  p.name = get_text(IDC_P5_NAME);
  p.remote = IsDlgButtonChecked(IDC_P5_REMOTE) == BST_CHECKED;
  const int sel = models_.GetCurSel();
  if (sel >= 0 && sel < (int)model_paths_.size())
    p.model_path = model_paths_[sel];
  p.model_type = model_type_.GetCurSel() == 1 ? L"Instruct" : L"Base";
  p.api_url = get_text(IDC_P5_API_URL);
  p.api_key = get_text(IDC_P5_API_KEY);
  p.model = get_text(IDC_P5_API_MODEL);
  if (ProfileLabel(p) != old_label) {
    profile_list_.DeleteString(profile_sel_);
    profile_list_.InsertString(profile_sel_, ProfileLabel(p).c_str());
    profile_list_.SetCurSel(profile_sel_);
  }
}

int SettingsDialog::ComboProfile(int combo_id) const {
  const int sel = (int)::SendMessageW(::GetDlgItem(m_hWnd, combo_id), CB_GETCURSEL, 0, 0);
  const int index = combo_id == IDC_P4_PROFILE ? sel - 1 : sel;  // 精煉的第一項是「不使用」
  return index >= 0 && index < (int)profiles_.size() ? index : -1;
}

void SettingsDialog::RefreshProfileCombos(int predict, int refine) {
  if (predict == -2)
    predict = ComboProfile(IDC_P3_PROFILE);
  if (refine == -2)
    refine = ComboProfile(IDC_P4_PROFILE);
  predict_profile_.ResetContent();
  refine_profile_.ResetContent();
  refine_profile_.AddString(L"不使用 LLM（只做統計整理）");
  for (const auto& p : profiles_) {
    predict_profile_.AddString(ProfileLabel(p).c_str());
    refine_profile_.AddString(ProfileLabel(p).c_str());
  }
  predict_profile_.SetCurSel(predict >= 0 && predict < (int)profiles_.size() ? predict : -1);
  refine_profile_.SetCurSel(refine >= 0 && refine < (int)profiles_.size() ? refine + 1 : 0);
  UpdateProfileUsage();
}

void SettingsDialog::UpdateProfileUsage() {
  std::wstring text;
  if (profile_sel_ >= 0) {
    std::vector<std::wstring> uses;
    if (ComboProfile(IDC_P3_PROFILE) == profile_sel_)
      uses.push_back(L"智慧預測");
    if (ComboProfile(IDC_P4_PROFILE) == profile_sel_)
      uses.push_back(L"個人詞庫精煉");
    if (uses.empty()) {
      text = L"目前沒有被使用。可在「智慧預測」或「個人詞庫」頁選用。";
    } else {
      text = L"用於：";
      for (size_t i = 0; i < uses.size(); ++i)
        text += (i ? L"、" : L"") + uses[i];
      const ModelProfile& p = profiles_[profile_sel_];
      if (!p.remote && p.model_type == L"Base" && ComboProfile(IDC_P4_PROFILE) == profile_sel_)
        text += L"\r\n注意：Base 模型不會照指示回答，精煉效果可能很差。";
    }
  } else {
    text = L"按「新增」建立一組模型設定。";
  }
  GetDlgItem(IDC_P5_USAGE).SetWindowTextW(text.c_str());
}

LRESULT SettingsDialog::OnProfileSelect(WORD, WORD, HWND, BOOL&) {
  CommitProfileEditor();
  SelectProfile(profile_list_.GetCurSel());
  return 0;
}

LRESULT SettingsDialog::OnProfileAdd(WORD, WORD id, HWND, BOOL&) {
  CommitProfileEditor();
  ModelProfile p;
  if (id == IDC_P5_COPY && profile_sel_ >= 0) {
    p = profiles_[profile_sel_];
    p.name += L" 複本";
  } else {
    p.name = L"新模型";
    p.model_type = L"Instruct";
  }
  profiles_.push_back(p);
  PopulateProfileList();
  SelectProfile((int)profiles_.size() - 1);
  RefreshProfileCombos();
  llm_modified_ = true;
  CEdit name(GetDlgItem(IDC_P5_NAME));
  name.SetFocus();
  name.SetSelAll();
  return 0;
}

LRESULT SettingsDialog::OnProfileDelete(WORD, WORD, HWND, BOOL&) {
  if (profile_sel_ < 0)
    return 0;
  CommitProfileEditor();
  const int index = profile_sel_;
  int predict = ComboProfile(IDC_P3_PROFILE), refine = ComboProfile(IDC_P4_PROFILE);
  std::wstring message = L"確定要刪除「" + profiles_[index].name + L"」嗎？";
  if (predict == index || refine == index)
    message += L"\n\n這組設定正在使用中，刪除後請另外選擇模型。";
  if (MessageBoxW(message.c_str(), L"刪除模型設定", MB_YESNO | MB_ICONQUESTION) != IDYES)
    return 0;
  auto shift = [&](int i) { return i == index ? -1 : i > index ? i - 1 : i; };
  predict = shift(predict);
  refine = shift(refine);
  profiles_.erase(profiles_.begin() + index);
  profile_sel_ = -1;
  PopulateProfileList();
  SelectProfile((std::min)(index, (int)profiles_.size() - 1));
  RefreshProfileCombos(predict, refine);
  llm_modified_ = true;
  return 0;
}

LRESULT SettingsDialog::OnProfileEdit(WORD, WORD, HWND, BOOL&) {
  if (loading_profile_ || !loaded_)
    return 0;
  CommitProfileEditor();
  RefreshProfileCombos();
  llm_modified_ = true;
  return 0;
}

LRESULT SettingsDialog::OnProfileChoice(WORD, WORD, HWND, BOOL&) {
  if (loaded_)
    llm_modified_ = true;
  UpdateProfileUsage();
  return 0;
}

LRESULT SettingsDialog::OnGoModels(WORD, WORD id, HWND, BOOL&) {
  const int index = ComboProfile(id == IDC_P3_MANAGE ? IDC_P3_PROFILE : IDC_P4_PROFILE);
  CommitProfileEditor();
  GoToPage(kPageModels);
  if (index >= 0)
    SelectProfile(index);
  return 0;
}

LRESULT SettingsDialog::OnGoDict(WORD, WORD, HWND, BOOL&) {
  GoToPage(kPageDict);
  return 0;
}

// ---------------------------------------------------------------------------
// 模型檔案：%USERPROFILE%\models 裡的 GGUF（加入、下載、移到回收筒）

namespace {

fs::path ModelsDir() {
  wchar_t profile[MAX_PATH] = {0};
  GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
  return fs::path(profile) / L"models";
}

std::wstring FormatSize(ULONGLONG bytes) {
  wchar_t buf[32];
  if (bytes >= (1ULL << 30))
    swprintf_s(buf, L"%.2f GB", bytes / 1073741824.0);
  else
    swprintf_s(buf, L"%.0f MB", bytes / 1048576.0);
  return buf;
}

bool IsGguf(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  char magic[4] = {0};
  return in.read(magic, 4) && memcmp(magic, "GGUF", 4) == 0;
}

// 網址 → 檔名；Hugging Face 的頁面網址（/blob/）換成下載網址（/resolve/）
std::wstring NormalizeModelUrl(std::wstring url, std::wstring* file_name) {
  url = LLMTrim(url);
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

// 以 WinHTTP 下載（自動跟隨轉址，例如 Hugging Face → CDN）
bool HttpDownload(const std::wstring& url, const fs::path& dest,
                  const std::function<bool(ULONGLONG, ULONGLONG)>& progress, std::wstring* error) {
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

struct ModelCopyContext {
  std::function<bool(ULONGLONG, ULONGLONG)> progress;
};

DWORD CALLBACK CopyProgress(LARGE_INTEGER total, LARGE_INTEGER done, LARGE_INTEGER, LARGE_INTEGER,
                            DWORD, DWORD, HANDLE, HANDLE, LPVOID data) {
  auto* ctx = (ModelCopyContext*)data;
  return ctx->progress(done.QuadPart, total.QuadPart) ? PROGRESS_CONTINUE : PROGRESS_CANCEL;
}

}  // namespace

void SettingsDialog::PopulateModelFiles(const std::wstring& select) {
  model_files_.ResetContent();
  model_file_paths_.clear();
  std::vector<fs::path> files;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(ModelsDir(), ec)) {
    if (entry.is_regular_file(ec) && ToLower(entry.path().extension().wstring()) == L".gguf")
      files.push_back(entry.path());
  }
  std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
    return ToLower(a.filename().wstring()) < ToLower(b.filename().wstring());
  });
  int selected = -1;
  for (const auto& file : files) {
    std::wstring text = file.filename().wstring() + L"　" + FormatSize(fs::file_size(file, ec));
    int uses = 0;
    for (const auto& p : profiles_) {
      if (!p.remote && ToLower(p.model_path) == ToLower(file.wstring()))
        ++uses;
    }
    if (uses)
      text += L"　（使用中）";
    if (!select.empty() && ToLower(file.wstring()) == ToLower(select))
      selected = (int)model_file_paths_.size();
    model_file_paths_.push_back(file.wstring());
    model_files_.AddString(text.c_str());
  }
  model_files_.SetCurSel(selected);
  UpdateModelFileButtons();
}

void SettingsDialog::UpdateModelFileButtons() {
  const bool busy = file_busy_;
  GetDlgItem(IDC_P5_FILE_ADD).EnableWindow(!busy);
  GetDlgItem(IDC_P5_FILE_DELETE).EnableWindow(!busy && model_files_.GetCurSel() >= 0);
  GetDlgItem(IDC_P5_URL).EnableWindow(!busy);
  GetDlgItem(IDC_P5_DOWNLOAD).SetWindowTextW(busy ? L"取消" : L"下載");
}

void SettingsDialog::SetFileStatus(const std::wstring& text) {
  GetDlgItem(IDC_P5_FILE_STATUS).SetWindowTextW(text.c_str());
}

LRESULT SettingsDialog::OnModelFileSel(WORD, WORD, HWND, BOOL&) {
  UpdateModelFileButtons();
  return 0;
}

LRESULT SettingsDialog::OnModelFileOpen(WORD, WORD, HWND, BOOL&) {
  std::error_code ec;
  fs::create_directories(ModelsDir(), ec);
  const int sel = model_files_.GetCurSel();
  if (sel >= 0 && sel < (int)model_file_paths_.size())
    OpenFolderAndSelectItem(model_file_paths_[sel]);
  else
    ShellExecuteW(m_hWnd, L"open", ModelsDir().c_str(), NULL, NULL, SW_SHOWNORMAL);
  return 0;
}

LRESULT SettingsDialog::OnModelFileAdd(WORD, WORD, HWND, BOOL&) {
  if (file_busy_)
    return 0;
  CFileDialog dialog(TRUE, L"gguf", NULL, OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER,
                     L"GGUF 模型 (*.gguf)\0*.gguf\0所有檔案 (*.*)\0*.*\0", m_hWnd);
  if (dialog.DoModal() != IDOK)
    return 0;
  const fs::path src = dialog.m_szFileName;
  if (!IsGguf(src)) {
    SetFileStatus(L"這不是 GGUF 模型檔。");
    return 0;
  }
  const fs::path dest = ModelsDir() / src.filename();
  std::error_code ec;
  if (fs::equivalent(src, dest, ec)) {
    SetFileStatus(L"這個檔案已經在模型資料夾裡。");
    return 0;
  }
  if (fs::exists(dest, ec) &&
      MessageBoxW((L"模型資料夾已有「" + src.filename().wstring() + L"」，要取代嗎？").c_str(),
                  L"加入模型檔", MB_YESNO | MB_ICONQUESTION) != IDYES)
    return 0;
  StartModelFileJob(src.wstring(), dest.wstring(), false);
  return 0;
}

LRESULT SettingsDialog::OnModelDownload(WORD, WORD, HWND, BOOL&) {
  if (file_busy_) {
    file_cancel_ = true;  // 按鈕此時是「取消」
    return 0;
  }
  CString text;
  GetDlgItem(IDC_P5_URL).GetWindowTextW(text);
  std::wstring name;
  const std::wstring url = NormalizeModelUrl((LPCWSTR)text, &name);
  if (url.rfind(L"http://", 0) != 0 && url.rfind(L"https://", 0) != 0) {
    SetFileStatus(L"請貼上以 https:// 開頭的下載網址。");
    GetDlgItem(IDC_P5_URL).SetFocus();
    return 0;
  }
  if (ToLower(fs::path(name).extension().wstring()) != L".gguf" ||
      name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) {
    SetFileStatus(L"網址要指向 .gguf 檔（例如 …/resolve/main/model.gguf）。");
    return 0;
  }
  const fs::path dest = ModelsDir() / name;
  std::error_code ec;
  if (fs::exists(dest, ec) &&
      MessageBoxW((L"模型資料夾已有「" + name + L"」，要重新下載並取代嗎？").c_str(), L"下載模型",
                  MB_YESNO | MB_ICONQUESTION) != IDYES)
    return 0;
  StartModelFileJob(url, dest.wstring(), true);
  return 0;
}

void SettingsDialog::StartModelFileJob(const std::wstring& src, const std::wstring& dest,
                                       bool download) {
  if (file_worker_.joinable())
    file_worker_.join();
  std::error_code ec;
  fs::create_directories(ModelsDir(), ec);
  file_busy_ = true;
  file_cancel_ = false;
  UpdateModelFileButtons();
  SetFileStatus(download ? L"連線中…" : L"複製中…");
  HWND hwnd = m_hWnd;
  file_worker_ = std::thread([this, hwnd, src, dest, download]() {
    // 先寫到 .part，完成並確認是 GGUF 後才改名，避免留下半個模型檔
    const fs::path part = dest + L".part";
    ULONGLONG last_tick = 0;
    auto progress = [&](ULONGLONG done, ULONGLONG total) {
      const ULONGLONG now = GetTickCount64();
      if (now - last_tick >= 300) {
        last_tick = now;
        std::wstring text = (download ? L"下載中 " : L"複製中 ") + FormatSize(done);
        if (total) {
          wchar_t pct[16];
          swprintf_s(pct, L"%.0f%%", done * 100.0 / total);
          text += L" / " + FormatSize(total) + L"（" + pct + L"）";
        }
        {
          std::lock_guard<std::mutex> lock(file_mutex_);
          file_message_ = text;
        }
        ::PostMessageW(hwnd, WM_APP_FILE_PROGRESS, 0, 0);
      }
      return !file_cancel_.load();
    };
    std::wstring error;
    bool ok;
    if (download) {
      ok = HttpDownload(src, part, progress, &error);
    } else {
      ModelCopyContext ctx{progress};
      BOOL cancel = FALSE;
      ok = !!CopyFileExW(src.c_str(), part.c_str(), CopyProgress, &ctx, &cancel, 0);
      if (!ok)
        error = file_cancel_ ? L"已取消" : L"複製失敗（錯誤 " + std::to_wstring(GetLastError()) + L"）";
    }
    std::error_code ec2;
    if (ok && !IsGguf(part)) {
      ok = false;
      error = L"下載的內容不是 GGUF 模型檔（網址可能指向網頁）";
    }
    if (ok && !MoveFileExW(part.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING)) {
      ok = false;
      error = L"無法取代原本的檔案（可能正在使用中）";
    }
    if (!ok)
      fs::remove(part, ec2);
    {
      std::lock_guard<std::mutex> lock(file_mutex_);
      file_message_ = ok ? (download ? L"下載完成：" : L"已加入：") + fs::path(dest).filename().wstring()
                         : (download ? L"下載失敗：" : L"加入失敗：") + error;
      file_result_ = ok ? dest : L"";
    }
    file_busy_ = false;
    ::PostMessageW(hwnd, WM_APP_FILE_PROGRESS, ok ? 1 : 2, 0);
  });
}

LRESULT SettingsDialog::OnFileProgress(UINT, WPARAM state, LPARAM, BOOL&) {
  std::wstring message, result;
  {
    std::lock_guard<std::mutex> lock(file_mutex_);
    message = file_message_;
    result = file_result_;
  }
  SetFileStatus(message);
  if (state == 0)
    return 0;
  if (file_worker_.joinable())
    file_worker_.join();
  if (state == 1) {
    GetDlgItem(IDC_P5_URL).SetWindowTextW(L"");
    PopulateModelFiles(result);
    // 模型檔下拉選單也更新（目前編輯的模型設定維持原本的選擇）
    loading_profile_ = true;
    PopulateModels(profile_sel_ >= 0 ? profiles_[profile_sel_].model_path : L"");
    loading_profile_ = false;
  }
  UpdateModelFileButtons();
  return 0;
}

LRESULT SettingsDialog::OnModelFileDelete(WORD, WORD, HWND, BOOL&) {
  const int sel = model_files_.GetCurSel();
  if (sel < 0 || sel >= (int)model_file_paths_.size() || file_busy_)
    return 0;
  const std::wstring path = model_file_paths_[sel];
  std::wstring users;
  for (const auto& p : profiles_) {
    if (!p.remote && ToLower(p.model_path) == ToLower(path))
      users += (users.empty() ? L"" : L"、") + p.name;
  }
  std::wstring message = L"要把「" + fs::path(path).filename().wstring() + L"」移到資源回收筒嗎？";
  if (!users.empty())
    message += L"\n\n這個檔案正被模型設定「" + users + L"」使用，移除後請改選其他模型檔。";
  if (MessageBoxW(message.c_str(), L"移除模型檔", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
    return 0;
  // 輸入法載入中的模型檔會被鎖住：先試著獨占開啟，打不開就說明原因
  HANDLE h = CreateFileW(path.c_str(), DELETE, 0, NULL, OPEN_EXISTING, 0, NULL);
  if (h == INVALID_HANDLE_VALUE) {
    SetFileStatus(L"檔案正在使用中（輸入法已載入這個模型）。請先改用其他模型並套用。");
    return 0;
  }
  CloseHandle(h);
  std::wstring from = path;
  from.push_back(L'\0');  // SHFileOperation 需要雙 NUL 結尾
  SHFILEOPSTRUCTW op = {0};
  op.hwnd = m_hWnd;
  op.wFunc = FO_DELETE;
  op.pFrom = from.c_str();
  op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
  if (SHFileOperationW(&op) != 0 || op.fAnyOperationsAborted) {
    SetFileStatus(L"移除失敗。");
    return 0;
  }
  SetFileStatus(L"已移到資源回收筒：" + fs::path(path).filename().wstring());
  PopulateModelFiles();
  loading_profile_ = true;
  PopulateModels(profile_sel_ >= 0 ? profiles_[profile_sel_].model_path : L"");
  loading_profile_ = false;
  return 0;
}

// ---------------------------------------------------------------------------
// API 連線測試：直接用畫面上的網址、金鑰、模型名稱送一個很小的請求（不必先套用）

namespace {

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

std::wstring TestApi(const SettingsDialog::ModelProfile& p) {
  std::string body = "{\"model\":\"" + LLMJsonEscapeLite(wtou8(p.model)) +
                     "\",\"messages\":[{\"role\":\"user\",\"content\":\"" +
                     LLMJsonEscapeLite(u8"請只回覆「OK」兩個字。") +
                     "\"}],\"max_tokens\":64,\"temperature\":0,\"stream\":false}";
  const ULONGLONG t0 = GetTickCount64();
  DWORD status = 0;
  std::string response;
  std::wstring error;
  if (!HttpRequest(L"POST", p.api_url, p.api_key, body, &status, &response, &error))
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
    content = LLMTrim(content);
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
  if (status == 400 || status == 404 || p.model.empty()) {
    const std::wstring models = ListModels(p.api_url, p.api_key);
    if (!models.empty())
      text += L"\r\n" + models;
  }
  return text;
}

}  // namespace

LRESULT SettingsDialog::OnApiTest(WORD, WORD, HWND, BOOL&) {
  if (api_busy_ || profile_sel_ < 0)
    return 0;
  CommitProfileEditor();
  const ModelProfile p = profiles_[profile_sel_];
  if (p.api_url.empty()) {
    GetDlgItem(IDC_P5_REMOTE_HINT).SetWindowTextW(L"請先填寫網址。");
    return 0;
  }
  if (api_worker_.joinable())
    api_worker_.join();
  api_busy_ = true;
  api_profile_ = profile_sel_;
  GetDlgItem(IDC_P5_API_TEST).EnableWindow(FALSE);
  GetDlgItem(IDC_P5_REMOTE_HINT).SetWindowTextW(L"測試中…");
  HWND hwnd = m_hWnd;
  api_worker_ = std::thread([this, hwnd, p]() {
    const std::wstring result = TestApi(p);
    {
      std::lock_guard<std::mutex> lock(file_mutex_);
      api_result_ = result;
    }
    api_busy_ = false;
    ::PostMessageW(hwnd, WM_APP_API_TEST, 0, 0);
  });
  return 0;
}

LRESULT SettingsDialog::OnApiTestDone(UINT, WPARAM, LPARAM, BOOL&) {
  if (api_worker_.joinable())
    api_worker_.join();
  std::wstring result;
  {
    std::lock_guard<std::mutex> lock(file_mutex_);
    result = api_result_;
  }
  GetDlgItem(IDC_P5_API_TEST).EnableWindow(TRUE);
  // 使用者在測試期間切到別組就不顯示
  if (api_profile_ == profile_sel_)
    GetDlgItem(IDC_P5_REMOTE_HINT).SetWindowTextW(result.c_str());
  return 0;
}

// ---------------------------------------------------------------------------
// 注音排序：讓個人詞庫的常用詞影響 Rime 的選字
//
// 注音方案（bopomofo 系列）改用 terra_pinyin.personal 詞典：它匯入原本的 terra_pinyin，
// 再加上輸入法產生的常用詞（權重較高）。使用者詞典仍是 terra_pinyin.userdb，學到的排序不受影響。
// 方案的 custom.yaml 只增刪我們自己的標記區塊，保留使用者原有的設定與註解。

namespace {

const wchar_t* const kZhuyinSchemas[] = {L"bopomofo", L"bopomofo_express", L"bopomofo_tw"};
const char kBoostBegin[] = "  # >>> weasel-personal-dict";
const char kBoostEnd[] = "  # <<< weasel-personal-dict";

// 改寫一個方案的 custom.yaml；回傳 false 表示無法安全修改（例如使用者自己改過詞典）
bool PatchSchemaDictionary(const fs::path& file, bool enable, std::wstring* error) {
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
      if (line.rfind(kBoostBegin, 0) == 0) {
        inside = true;
        continue;
      }
      if (inside) {
        if (line.rfind(kBoostEnd, 0) == 0)
          inside = false;
        continue;
      }
      lines.push_back(line);
    }
  }
  if (enable) {
    for (const auto& line : lines) {
      if (line.find("translator/dictionary") != std::string::npos ||
          line.find("translator/user_dict") != std::string::npos) {
        *error = file.filename().wstring() + L" 已自行設定詞典，沒有修改";
        return false;
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
    const std::vector<std::string> block = {
        std::string(kBoostBegin) + u8"：個人詞庫的常用詞影響選字排序（小狼毫設定自動管理）",
        "  translator/dictionary: terra_pinyin.personal",
        "  translator/user_dict: terra_pinyin",
        kBoostEnd,
    };
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

}  // namespace

bool SettingsDialog::ApplyRimeBoost(bool enable, std::wstring* error) {
  const fs::path user_dir = WeaselUserDataPath();
  const fs::path dict = user_dir / L"terra_pinyin.personal.dict.yaml";
  // 目前選用的方案
  std::set<std::wstring> selected;
  RimeSchemaList list = {0};
  if (api_->get_selected_schema_list(switcher_settings_, &list)) {
    for (size_t i = 0; i < list.size; ++i)
      selected.insert(u8tow(list.list[i].schema_id));
    api_->schema_list_destroy(&list);
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

// ---------------------------------------------------------------------------
// 詞庫管理：個人詞庫的詞彙與精煉規則（經由輸入法讀寫，檔案以 DPAPI 加密）

namespace {
const size_t kMaxShownWords = 3000;
fs::path PersonalDir() {
  return WeaselUserDataPath() / L"personal";
}
}  // namespace

void SettingsDialog::LoadPersonalWords() {
  words_.clear();
  rules_.clear();
  words_loaded_ = true;
  personal_disabled_ = false;
  const fs::path file = PersonalDir() / L"export.dat";
  std::error_code ec;
  fs::remove(file, ec);
  std::string plain;
  if (!SendPersonalCommand(5)) {
    personal_disabled_ = true;
    GetDlgItem(IDC_P6_COUNT).SetWindowTextW(L"無法連線到輸入法服務。");
  } else if (!personal_crypto::ReadProtected(file, &plain)) {
    personal_disabled_ = true;  // 個人詞庫關閉時輸入法不匯出
  } else {
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
        words_.emplace_back(f[1], _wtof(f[2].c_str()));
      else if (f.size() == 2 && f[0] == L"A")
        rules_.push_back({WordRule::kAdd, f[1], L""});
      else if (f.size() == 2 && f[0] == L"R")
        rules_.push_back({WordRule::kBlock, f[1], L""});
      else if (f.size() == 3 && f[0] == L"M")
        rules_.push_back({WordRule::kMerge, f[1], f[2]});
    }
  }
  fs::remove(file, ec);
  // 清單本身不停用（深色主題下停用的清單會變成淺灰底），只停用操作
  for (int id : {IDC_P6_FILTER, IDC_P6_WORD, IDC_P6_ADD, IDC_P6_MERGE, IDC_P6_BLOCK, IDC_P6_UNRULE})
    GetDlgItem(id).EnableWindow(!personal_disabled_);
  PopulateWordList();
  PopulateRuleList();
}

void SettingsDialog::PopulateWordList() {
  CString filter_text;
  GetDlgItem(IDC_P6_FILTER).GetWindowTextW(filter_text);
  const std::wstring filter = LLMTrim((LPCWSTR)filter_text);
  words_list_.SetRedraw(FALSE);
  words_list_.DeleteAllItems();
  size_t matched = 0;
  for (size_t i = 0; i < words_.size(); ++i) {
    if (!filter.empty() && words_[i].first.find(filter) == std::wstring::npos)
      continue;
    if (matched++ >= kMaxShownWords)
      continue;
    const int row = words_list_.GetItemCount();
    words_list_.AddItem(row, 0, words_[i].first.c_str());
    wchar_t score[32];
    swprintf_s(score, L"%.1f", words_[i].second);
    words_list_.SetItemText(row, 1, score);
    words_list_.SetItemData(row, (DWORD_PTR)i);
  }
  words_list_.SetRedraw(TRUE);
  words_list_.Invalidate();
  std::wstring count;
  if (personal_disabled_)
    count = L"個人詞庫目前關閉，或輸入法沒有回應。";
  else if (filter.empty())
    count = L"共 " + std::to_wstring(words_.size()) + L" 個詞（依常用程度排序）";
  else
    count = L"符合 " + std::to_wstring(matched) + L" 個（共 " + std::to_wstring(words_.size()) + L" 個）";
  if (matched > kMaxShownWords)
    count += L"，顯示前 " + std::to_wstring(kMaxShownWords) + L" 個";
  GetDlgItem(IDC_P6_COUNT).SetWindowTextW(count.c_str());
}

void SettingsDialog::PopulateRuleList() {
  rules_list_.DeleteAllItems();
  for (size_t i = 0; i < rules_.size(); ++i) {
    const WordRule& r = rules_[i];
    const std::wstring text = r.kind == WordRule::kMerge ? L"合併　" + r.from + L" → " + r.to
                              : r.kind == WordRule::kAdd   ? L"加入　" + r.from
                                                           : L"封鎖　" + r.from;
    const int row = rules_list_.GetItemCount();
    rules_list_.AddItem(row, 0, text.c_str());
    rules_list_.SetItemData(row, (DWORD_PTR)i);
  }
}

bool SettingsDialog::SendWordEdits(const std::vector<std::wstring>& lines) {
  std::string plain = "WWPE1\n";
  for (const auto& line : lines)
    plain += wtou8(line) + "\n";
  if (!personal_crypto::WriteProtected(PersonalDir() / L"edit.dat", plain)) {
    SetStatus(L"無法寫入修改檔。");
    return false;
  }
  if (!SendPersonalCommand(6)) {
    SetStatus(L"無法連線到輸入法服務。");
    return false;
  }
  return true;
}

LRESULT SettingsDialog::OnWordEdit(WORD, WORD id, HWND, BOOL&) {
  CString text;
  GetDlgItem(IDC_P6_WORD).GetWindowTextW(text);
  const std::wstring word = LLMTrim((LPCWSTR)text);
  std::vector<std::wstring> selected;
  for (int i = words_list_.GetNextItem(-1, LVNI_SELECTED); i >= 0;
       i = words_list_.GetNextItem(i, LVNI_SELECTED))
    selected.push_back(words_[words_list_.GetItemData(i)].first);
  std::vector<std::wstring> lines;
  if (id == IDC_P6_ADD) {
    if (word.empty() || word.size() > 40) {
      SetStatus(L"請先在左方輸入要加入的詞（40 字以內）。");
      return 0;
    }
    lines.push_back(L"A\t" + word);
  } else if (id == IDC_P6_MERGE) {
    if (selected.empty()) {
      SetStatus(L"請先選取要合併的詞（可多選）。");
      return 0;
    }
    if (word.empty() || word.size() > 40) {
      SetStatus(L"請在左方輸入正確的寫法；所選的詞會併到這個詞。");
      return 0;
    }
    for (const auto& w : selected) {
      if (w != word)
        lines.push_back(L"M\t" + w + L"\t" + word);
    }
  } else if (id == IDC_P6_BLOCK) {
    if (selected.empty()) {
      SetStatus(L"請先選取要封鎖的詞（可多選）。");
      return 0;
    }
    for (const auto& w : selected)
      lines.push_back(L"R\t" + w);
  } else if (id == IDC_P6_UNRULE) {
    for (int i = rules_list_.GetNextItem(-1, LVNI_SELECTED); i >= 0;
         i = rules_list_.GetNextItem(i, LVNI_SELECTED)) {
      const WordRule& r = rules_[rules_list_.GetItemData(i)];
      lines.push_back((r.kind == WordRule::kMerge ? L"X\t"
                       : r.kind == WordRule::kAdd ? L"Y\t"
                                                  : L"U\t") +
                      r.from);
    }
    if (lines.empty()) {
      SetStatus(L"請先選取右方要移除的規則。");
      return 0;
    }
  }
  if (lines.empty())
    return 0;
  if (SendWordEdits(lines)) {
    LoadPersonalWords();
    SetStatus(L"已更新個人詞庫（" + std::to_wstring(lines.size()) + L" 項）。");
  }
  return 0;
}

LRESULT SettingsDialog::OnWordFilter(WORD, WORD, HWND, BOOL&) {
  if (words_loaded_)
    PopulateWordList();
  return 0;
}

LRESULT SettingsDialog::OnWordRefresh(WORD, WORD, HWND, BOOL&) {
  LoadPersonalWords();
  return 0;
}

LRESULT SettingsDialog::OnWordSelChanged(int, LPNMHDR, BOOL&) {
  return 0;
}

// 雙擊詞彙：帶入左下的輸入框（方便當作合併的正確寫法）
LRESULT SettingsDialog::OnWordDblClick(int, LPNMHDR, BOOL&) {
  const int i = words_list_.GetNextItem(-1, LVNI_SELECTED);
  if (i >= 0)
    GetDlgItem(IDC_P6_WORD).SetWindowTextW(words_[words_list_.GetItemData(i)].first.c_str());
  return 0;
}

// ---------------------------------------------------------------------------
// 詞庫管理：輸入法的使用者詞典（原本的「用戶詞典管理」）

namespace {
// 詞典檔由輸入法服務開著；操作期間讓服務暫停（和原本的用戶詞典管理相同）
class ServiceMaintenance {
 public:
  ServiceMaintenance() {
    if (client_.Connect())
      client_.StartMaintenance();
  }
  ~ServiceMaintenance() {
    if (client_.Connect())
      client_.EndMaintenance();
  }

 private:
  weasel::Client client_;
};
}  // namespace

void SettingsDialog::PopulateDicts() {
  // 與原本的用戶詞典管理相同：先跑 installation_update（設定同步資料夾），詞典清單才讀得到
  RimeApi* rime = rime_get_api();
  if (!dict_task_ready_ && RIME_API_AVAILABLE(rime, run_task)) {
    rime->run_task("installation_update");
    dict_task_ready_ = true;
  }
  dicts_.ResetContent();
  RimeUserDictIterator iter = {0};
  api_->user_dict_iterator_init(&iter);
  while (const char* dict = api_->next_user_dict(&iter))
    dicts_.AddString(u8tow(dict).c_str());
  api_->user_dict_iterator_destroy(&iter);
  dicts_loaded_ = true;
  UpdateDictButtons();
}

void SettingsDialog::UpdateDictButtons() {
  const bool selected = dicts_.GetCurSel() >= 0;
  GetDlgItem(IDC_P6_BACKUP).EnableWindow(selected);
  GetDlgItem(IDC_P6_EXPORT).EnableWindow(selected);
  GetDlgItem(IDC_P6_IMPORT).EnableWindow(selected);
  GetDlgItem(IDC_P6_RESTORE).EnableWindow(TRUE);
}

LRESULT SettingsDialog::OnDictSelChange(WORD, WORD, HWND, BOOL&) {
  UpdateDictButtons();
  return 0;
}

LRESULT SettingsDialog::OnDictCommand(WORD, WORD id, HWND, BOOL&) {
  std::wstring dict_name;
  const int sel = dicts_.GetCurSel();
  if (sel >= 0) {
    CString name;
    dicts_.GetText(sel, name);
    dict_name = (LPCWSTR)name;
  }
  if (id != IDC_P6_RESTORE && dict_name.empty())
    return 0;
  auto load = [](UINT ids) {
    CString s;
    s.LoadStringW(ids);
    return std::wstring((LPCWSTR)s);
  };
  // 先選檔（不必暫停輸入法），確定要做了才暫停
  std::wstring selected_path;
  if (id == IDC_P6_RESTORE) {
    const std::wstring snapshot = load(IDS_STR_DICT_SNAPSHOT) + L" (*.userdb.txt)";
    const std::wstring kcss = load(IDS_STR_KCSS_DICT_SNAPSHOT) + L" (*.userdb.kct.snapshot)";
    const std::wstring all = load(IDS_STR_ALL_FILES);
    COMDLG_FILTERSPEC filter[3] = {{snapshot.c_str(), L"*.userdb.txt"},
                                   {kcss.c_str(), L"*.userdb.kct.snapshot"},
                                   {all.c_str(), L"*.*"}};
    selected_path = DoFileDialog<IFileOpenDialog, FileOpenDialog>(
        m_hWnd, load(IDS_STR_OPEN).c_str(), ARRAYSIZE(filter), filter, NULL, L"snapshot");
    if (selected_path.empty())
      return 0;
  } else if (id == IDC_P6_EXPORT || id == IDC_P6_IMPORT) {
    const std::wstring txt = load(IDS_STR_TXT_FILES) + L" (*.txt)";
    const std::wstring all = load(IDS_STR_ALL_FILES);
    COMDLG_FILTERSPEC filter[2] = {{txt.c_str(), L"*.txt"}, {all.c_str(), L"*.*"}};
    const std::wstring file_name = dict_name + L"_export.txt";
    if (id == IDC_P6_EXPORT)
      selected_path = DoFileDialog<IFileSaveDialog, FileSaveDialog>(
          m_hWnd, load(IDS_STR_SAVE_AS).c_str(), ARRAYSIZE(filter), filter, file_name.c_str(), L"txt");
    else
      selected_path = DoFileDialog<IFileOpenDialog, FileOpenDialog>(
          m_hWnd, load(IDS_STR_OPEN).c_str(), ARRAYSIZE(filter), filter, file_name.c_str(), L"txt");
    if (selected_path.empty())
      return 0;
  }

  CWaitCursor wait;
  SetStatus(L"輸入法暫停中，正在處理詞典…");
  std::wstring open_path;  // 完成後在檔案總管顯示
  std::wstring report;
  UINT error_ids = 0;
  {
    ServiceMaintenance maintenance;
    RimeApi* rime = rime_get_api();
    if (!dict_task_ready_ && RIME_API_AVAILABLE(rime, run_task)) {
      rime->run_task("installation_update");  // 建立使用者資料同步資料夾
      dict_task_ready_ = true;
    }
    const std::string name_u8 = wtou8(dict_name);
    const std::string path_u8 = wtou8(selected_path);
    if (id == IDC_P6_BACKUP) {
      char dir[MAX_PATH] = {0};
      rime->get_user_data_sync_dir(dir, _countof(dir));
      WCHAR wdir[MAX_PATH] = {0};
      MultiByteToWideChar(CP_ACP, 0, dir, -1, wdir, _countof(wdir));
      std::wstring path = wdir;
      if (_waccess_s(path.c_str(), 0) != 0 && !CreateDirectoryW(path.c_str(), NULL) &&
          GetLastError() == ERROR_PATH_NOT_FOUND) {
        error_ids = IDS_STR_ERREXPORT_SYNC_UV;
      } else {
        path += L"\\" + dict_name + L".userdb.txt";
        if (!api_->backup_user_dict(name_u8.c_str()))
          error_ids = IDS_STR_ERR_EXPORT_UNKNOWN;
        else if (_waccess(path.c_str(), 0) != 0)
          error_ids = IDS_STR_ERR_EXPORT_SNAP_LOST;
        else
          open_path = path, report = L"已備份「" + dict_name + L"」。";
      }
    } else if (id == IDC_P6_RESTORE) {
      if (!api_->restore_user_dict(path_u8.c_str()))
        error_ids = IDS_STR_ERR_UNKNOWN;
      else
        report = L"已還原詞典快照。";
    } else if (id == IDC_P6_EXPORT) {
      const int result = api_->export_user_dict(name_u8.c_str(), path_u8.c_str());
      if (result < 0)
        error_ids = IDS_STR_ERR_UNKNOWN;
      else if (_waccess(selected_path.c_str(), 0) != 0)
        error_ids = IDS_STR_ERR_EXPORT_FILE_LOST;
      else
        open_path = selected_path,
        report = load(IDS_STR_EXPORTED) + L" " + std::to_wstring(result) + L" " +
                 load(IDS_STR_RECORD_COUNT);
    } else if (id == IDC_P6_IMPORT) {
      const int result = api_->import_user_dict(name_u8.c_str(), path_u8.c_str());
      if (result < 0)
        error_ids = IDS_STR_ERR_UNKNOWN;
      else
        report = load(IDS_STR_IMPORTED) + L" " + std::to_wstring(result) + L" " +
                 load(IDS_STR_RECORD_COUNT);
    }
  }
  if (error_ids) {
    SetStatus(L"");
    MSG_BY_IDS(error_ids, IDS_STR_SAD, MB_OK | MB_ICONERROR);
    return 0;
  }
  SetStatus(report);
  if (!open_path.empty())
    OpenFolderAndSelectItem(open_path);
  PopulateDicts();
  if (sel >= 0 && sel < dicts_.GetCount())
    dicts_.SetCurSel(sel);
  UpdateDictButtons();
  return 0;
}

LRESULT SettingsDialog::OnApply(WORD, WORD, HWND, BOOL&) {
  Save();
  return 0;
}

LRESULT SettingsDialog::OnOK(WORD, WORD, HWND, BOOL&) {
  if (Save())
    EndDialog(IDOK);
  return 0;
}

LRESULT SettingsDialog::OnCancel(WORD, WORD, HWND, BOOL&) {
  EndDialog(IDCANCEL);
  return 0;
}
