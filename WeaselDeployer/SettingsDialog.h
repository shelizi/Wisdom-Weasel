#pragma once

#include "resource.h"
#include "UIStyleSettings.h"
#include <CDialogDpiAware.h>
#include <rime_levers_api.h>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

// 合併後的「小狼毫設定」視窗：左側分頁（輸入方案／外觀／LLM 智慧預測），右側內容。
// 所有分頁的控制項都在同一個對話框範本裡（依 ID 範圍分頁、切換時顯示/隱藏），
// 讓 CDialogDpiAware 的 DPI 縮放能正確套用到每個控制項。
class SettingsDialog : public CDialogDpiAware<SettingsDialog> {
 public:
  enum { IDD = IDD_SETTINGS };

  // deploy：套用設定後重新部署（Configurator::UpdateWorkspace）
  SettingsDialog(RimeSwitcherSettings* switcher_settings,
                 UIStyleSettings* ui_style_settings,
                 std::function<void()> deploy);
  ~SettingsDialog();

  // 是否已在視窗內套用並重新部署過
  bool deployed() const { return deployed_; }

 protected:
  enum {
    WM_APP_REFONT = WM_APP + 1,  // DPI 變更後重新套用自訂字型
    kTimerTestPoll = 1,          // 等待輸入法回覆預測測試
    kTimerStatusPoll = 2,        // 重新部署後，等輸入法載入模型
  };

  BEGIN_MSG_MAP(SettingsDialog)
  MESSAGE_HANDLER(WM_DPICHANGED, OnDpiChangedPre)
  CHAIN_MSG_MAP(CDialogDpiAware<SettingsDialog>)
  MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
  MESSAGE_HANDLER(WM_CLOSE, OnClose)
  MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
  MESSAGE_HANDLER(WM_APP_REFONT, OnRefont)
  MESSAGE_HANDLER(WM_ERASEBKGND, OnEraseBkgnd)
  MESSAGE_HANDLER(WM_CTLCOLORDLG, OnCtlColorDlg)
  MESSAGE_HANDLER(WM_CTLCOLORSTATIC, OnCtlColorStatic)
  MESSAGE_HANDLER(WM_CTLCOLORBTN, OnCtlColorDlg)
  MESSAGE_HANDLER(WM_CTLCOLOREDIT, OnCtlColorEdit)
  MESSAGE_HANDLER(WM_CTLCOLORLISTBOX, OnCtlColorListBox)
  MESSAGE_HANDLER(WM_SETTINGCHANGE, OnSettingChange)
  COMMAND_HANDLER(IDC_THEME, CBN_SELCHANGE, OnThemeChange)
  COMMAND_HANDLER(IDC_P3_PREFIX, EN_CHANGE, OnLLMChanged)
  COMMAND_HANDLER(IDC_P3_API_URL, EN_CHANGE, OnLLMChanged)
  COMMAND_HANDLER(IDC_P3_API_KEY, EN_CHANGE, OnLLMChanged)
  COMMAND_HANDLER(IDC_P3_API_MODEL, EN_CHANGE, OnLLMChanged)
  COMMAND_ID_HANDLER(IDC_P3_LOCAL, OnProviderChange)
  COMMAND_ID_HANDLER(IDC_P3_REMOTE, OnProviderChange)
  COMMAND_HANDLER(IDC_P3_LOCAL_LABEL, STN_CLICKED, OnProviderLabelClick)
  COMMAND_HANDLER(IDC_P3_REMOTE_LABEL, STN_CLICKED, OnProviderLabelClick)
  MESSAGE_HANDLER(WM_MEASUREITEM, OnMeasureItem)
  MESSAGE_HANDLER(WM_DRAWITEM, OnDrawItem)
  MESSAGE_HANDLER(WM_TIMER, OnTimer)
  COMMAND_HANDLER(IDC_NAV, LBN_SELCHANGE, OnNavChange)
  COMMAND_ID_HANDLER(IDOK, OnOK)
  COMMAND_ID_HANDLER(IDCANCEL, OnCancel)
  COMMAND_ID_HANDLER(IDC_APPLY, OnApply)
  COMMAND_ID_HANDLER(IDC_P1_GET_SCHEMATA, OnGetSchemata)
  NOTIFY_HANDLER(IDC_P1_SCHEMA_LIST, LVN_ITEMCHANGED, OnSchemaListItemChanged)
  COMMAND_HANDLER(IDC_P2_COLOR_SCHEME, LBN_SELCHANGE, OnColorSchemeChange)
  COMMAND_ID_HANDLER(IDC_P2_SELECT_FONT, OnSelectFont)
  COMMAND_ID_HANDLER(IDC_P3_ENABLED, OnLLMEnabledClick)
  COMMAND_ID_HANDLER(IDC_P3_AFTER_COMMIT, OnLLMChanged)
  COMMAND_ID_HANDLER(IDC_P3_WHILE_TYPING, OnLLMChanged)
  COMMAND_HANDLER(IDC_P3_MODEL, CBN_SELCHANGE, OnModelChange)
  COMMAND_HANDLER(IDC_P3_TYPE, CBN_SELCHANGE, OnLLMChanged)
  COMMAND_ID_HANDLER(IDC_P3_BROWSE, OnBrowseModel)
  COMMAND_ID_HANDLER(IDC_P3_TEST_RUN, OnTestRun)
  END_MSG_MAP()

  LRESULT OnInitDialog(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnClose(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnDestroy(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnDpiChangedPre(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnRefont(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnEraseBkgnd(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnCtlColorDlg(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnCtlColorStatic(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnCtlColorEdit(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnCtlColorListBox(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnSettingChange(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnThemeChange(WORD, WORD, HWND, BOOL&);
  LRESULT OnMeasureItem(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnDrawItem(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnTimer(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnNavChange(WORD, WORD, HWND, BOOL&);
  LRESULT OnOK(WORD, WORD, HWND, BOOL&);
  LRESULT OnCancel(WORD, WORD, HWND, BOOL&);
  LRESULT OnApply(WORD, WORD, HWND, BOOL&);
  LRESULT OnGetSchemata(WORD, WORD, HWND, BOOL&);
  LRESULT OnSchemaListItemChanged(int, LPNMHDR, BOOL&);
  LRESULT OnColorSchemeChange(WORD, WORD, HWND, BOOL&);
  LRESULT OnSelectFont(WORD, WORD, HWND, BOOL&);
  LRESULT OnLLMEnabledClick(WORD, WORD, HWND, BOOL&);
  LRESULT OnLLMChanged(WORD, WORD, HWND, BOOL&);
  LRESULT OnModelChange(WORD, WORD, HWND, BOOL&);
  LRESULT OnProviderChange(WORD, WORD, HWND, BOOL&);
  LRESULT OnProviderLabelClick(WORD, WORD, HWND, BOOL&);
  LRESULT OnBrowseModel(WORD, WORD, HWND, BOOL&);
  LRESULT OnTestRun(WORD, WORD, HWND, BOOL&);

  // 版面與外觀
 public:
  struct Palette {
    COLORREF bg, nav, nav_selected, accent, text, subtle, input, separator;
  };

 protected:
  // 主題：0 = 跟隨系統、1 = 淺色、2 = 深色（存在 HKCU\Software\Rime\Weasel\SettingsTheme）
  void ApplyTheme();
  void ShowPage(int page);
  void ApplyFonts();
  void DestroyFonts();
  int Scale(int px) const;

  // 輸入方案
  void PopulateSchemas();
  void ShowSchemaDetails(RimeSchemaInfo* info);
  // 外觀
  void PopulateColorSchemes();
  void PreviewColorScheme(int index);
  void UpdateFontSummary();
  // LLM 智慧預測
  void LoadLLMSettings();
  void PopulateModels(const std::wstring& current);
  int AddModel(const std::wstring& path);
  void UpdateLLMEnableState();
  bool IsRemoteProvider() const;  // 選了 OpenAI 相容 API（否則為本機 llama.cpp）
  bool SaveLLMSettings();
  // 預測測試（透過正在執行的輸入法）
  void SendLLMRequest(const std::wstring& context, bool is_test);
  bool PollLLMResponse();

  // 儲存並重新部署；ok=true 時代表按下確定
  bool Save();
  void SetStatus(const std::wstring& text);

  RimeLeversApi* api_;
  RimeSwitcherSettings* switcher_settings_;
  UIStyleSettings* ui_settings_;
  std::function<void()> deploy_;
  bool deployed_ = false;
  bool loaded_ = false;

  bool schemas_modified_ = false;
  bool style_modified_ = false;
  bool llm_modified_ = false;

  int page_ = 0;
  CListBox nav_;
  CCheckListViewCtrl schema_list_;
  CListBox color_schemes_;
  CStatic preview_;
  CImage preview_image_;
  std::vector<ColorSchemeInfo> preset_;
  CComboBox models_;
  CComboBox model_type_;
  CListBox test_result_;
  std::vector<std::wstring> model_paths_;  // 與 models_ 的項目一一對應

  // 預測測試請求
  unsigned request_id_ = 0;
  bool request_is_test_ = false;
  ULONGLONG request_started_ = 0;
  int status_polls_left_ = 0;

  // 樣式
  DWORD theme_pref_ = 0;
  bool dark_ = false;
  std::map<HWND, std::pair<LONG, LONG>> original_frames_;  // 清單原本的 style / exstyle
  const Palette* pal_ = nullptr;
  HBRUSH input_brush_ = nullptr;
  HBRUSH separator_brush_ = nullptr;
  HFONT title_font_ = nullptr;
  HFONT section_font_ = nullptr;
  HFONT nav_font_ = nullptr;
  HFONT icon_font_ = nullptr;
  HBRUSH bg_brush_ = nullptr;
  HBRUSH nav_brush_ = nullptr;
};
