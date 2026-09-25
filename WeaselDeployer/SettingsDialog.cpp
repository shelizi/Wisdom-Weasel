#include "stdafx.h"
#include "SettingsDialog.h"
#include "FontSettingDialog.h"
#include "WeaselDeployer.h"
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
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

namespace fs = std::filesystem;

namespace {

// 顏色參考 Windows 11 設定 App 的淺色／深色外觀
//                                   bg                  nav                 nav_selected        accent              text                subtle              input               separator
const SettingsDialog::Palette kLight{RGB(255, 255, 255), RGB(243, 243, 243), RGB(224, 234, 246), RGB(0, 95, 184),    RGB(27, 27, 27),    RGB(96, 96, 96),    RGB(255, 255, 255), RGB(229, 229, 229)};
const SettingsDialog::Palette kDark{RGB(39, 39, 39),     RGB(32, 32, 32),    RGB(45, 55, 66),    RGB(76, 194, 255),  RGB(255, 255, 255), RGB(200, 200, 200), RGB(29, 29, 29),    RGB(58, 58, 58)};

const wchar_t kThemeRegKey[] = L"Software\\Rime\\Weasel";
const wchar_t kThemeRegValue[] = L"SettingsTheme";

struct PageInfo {
  const wchar_t* nav;
  const wchar_t* icon;  // Segoe Fluent Icons / Segoe MDL2 Assets
  const wchar_t* title;
  const wchar_t* desc;
};
const PageInfo kPages[] = {
    {L"輸入方案", L"\uE765", L"輸入方案", L"選擇要使用的輸入方案，並查看各方案的說明。"},
    {L"外觀", L"\uE790", L"外觀", L"候選視窗的配色與字體。"},
    {L"LLM 智慧預測", L"\uE82F", L"LLM 智慧預測",
     L"用本機的語言模型，預測你接下來要打的詞。"},
};
const int kPageCount = sizeof(kPages) / sizeof(kPages[0]);

// 控制項 ID 依範圍分頁：31xx → 0、32xx → 1、33xx → 2；其餘為共用
int PageOfControl(int id) {
  if (id >= 3100 && id < 3400)
    return id / 100 - 31;
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
    case IDC_STATUS:
    case IDC_THEME_LABEL:
      return true;
  }
  return false;
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

  models_.Attach(GetDlgItem(IDC_P3_MODEL));
  model_type_.Attach(GetDlgItem(IDC_P3_TYPE));
  model_type_.AddString(L"Base");
  model_type_.AddString(L"Instruct");
  test_result_.Attach(GetDlgItem(IDC_P3_TEST_RESULT));
  GetDlgItem(IDC_P3_TEST_INPUT).SetWindowTextW(L"今天天氣很好，我們一起去");

  PopulateSchemas();
  PopulateColorSchemes();
  UpdateFontSummary();
  LoadLLMSettings();

  InitCtrlRects();  // 依螢幕 DPI 縮放所有控制項
  ApplyFonts();
  ApplyTheme();

  nav_.SetCurSel(0);
  ShowPage(0);

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
                 IDC_P3_PREFIX_LABEL, IDC_P3_ENABLED})
    GetDlgItem(id).SetFont(section_font_);

  nav_.SetItemHeight(0, Scale(40));
  nav_.Invalidate();

  CRect rc;
  schema_list_.GetClientRect(&rc);
  schema_list_.SetColumnWidth(0, rc.Width() - 2);
}

void SettingsDialog::ShowPage(int page) {
  if (page < 0 || page >= kPageCount)
    return;
  page_ = page;
  GetDlgItem(IDC_PAGE_TITLE).SetWindowTextW(kPages[page].title);
  GetDlgItem(IDC_PAGE_DESC).SetWindowTextW(kPages[page].desc);
  for (HWND child = ::GetWindow(m_hWnd, GW_CHILD); child;
       child = ::GetWindow(child, GW_HWNDNEXT)) {
    const int owner = PageOfControl(::GetDlgCtrlID(child));
    if (owner >= 0)
      ::ShowWindow(child, owner == page ? SW_SHOW : SW_HIDE);
  }
  Invalidate();
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

  std::wstring model = get_string("llm/llamacpp/model_path");
  std::replace(model.begin(), model.end(), L'/', L'\\');
  PopulateModels(model);

  const std::wstring type = ToLower(get_string("llm/llamacpp/model_type"));
  // 未設定時 LlamaCppProvider 預設 Instruct
  model_type_.SetCurSel(type == L"base" ? 0 : 1);

  // 引導詞：設定裡用 \n 換行，編輯框要 \r\n；結尾的空行只是和前文隔開，不顯示
  std::wstring prefix = get_string("llm/llamacpp/prompt_prefix");
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
  for (int id : {IDC_P3_AFTER_COMMIT, IDC_P3_WHILE_TYPING, IDC_P3_MODEL_LABEL, IDC_P3_MODEL,
                 IDC_P3_BROWSE, IDC_P3_TYPE_LABEL, IDC_P3_TYPE, IDC_P3_PREFIX_LABEL,
                 IDC_P3_PREFIX})
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
  const int sel = models_.GetCurSel();
  if (sel >= 0 && sel < (int)model_paths_.size()) {
    if (const wchar_t* type = GuessModelType(model_paths_[sel]))
      model_type_.SetCurSel(wcscmp(type, L"Base") == 0 ? 0 : 1);
  }
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
  const int sel = models_.GetCurSel();
  if (sel >= 0 && sel < (int)model_paths_.size()) {
    std::wstring path = model_paths_[sel];
    std::replace(path.begin(), path.end(), L'\\', L'/');
    rime->config_set_string(&llm, "provider_type", "llamacpp");
    rime->config_set_string(&llm, "llamacpp/model_path", wtou8(path).c_str());
    rime->config_set_string(&llm, "llamacpp/model_type",
                            model_type_.GetCurSel() == 0 ? "Base" : "Instruct");
  }
  // 引導詞：統一換行為 \n，非空時結尾補一個換行，和後面的前文分開
  CString text;
  GetDlgItem(IDC_P3_PREFIX).GetWindowTextW(text);
  std::wstring prefix;
  for (const wchar_t* p = text; *p; ++p) {
    if (*p != L'\r')
      prefix += *p;
  }
  while (!prefix.empty() && (prefix.back() == L'\n' || prefix.back() == L' '))
    prefix.pop_back();
  if (!prefix.empty())
    prefix += L"\n";
  rime->config_set_string(&llm, "llamacpp/prompt_prefix", wtou8(prefix).c_str());
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
  const std::wstring model_name = model.empty() ? L"" : FileNameOf(u8tow(model));
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
  if (llm_modified_ && !SaveLLMSettings()) {
    SetStatus(L"LLM 設定儲存失敗。");
    return false;
  }
  if (style_modified_ || llm_modified_) {
    api_->save_settings(ui_settings_->settings());
    style_modified_ = llm_modified_ = false;
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
  SetStatus(L"已套用。");
  // 等輸入法重新載入模型後更新「目前載入」
  status_polls_left_ = 15;
  SetTimer(kTimerStatusPoll, 1000);
  return true;
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
