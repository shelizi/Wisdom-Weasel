#pragma once

#include "resource.h"
#include "UIStyleSettings.h"
#include <CDialogDpiAware.h>
#include <rime_levers_api.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <map>
#include <string>
#include <utility>
#include <vector>

// 合併後的「小狼毫設定」視窗：左側分頁（輸入方案／外觀／智慧預測／語言模型／個人詞庫／詞庫管理），右側內容。
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

  // 分頁（左側導覽的順序）
  enum Page {
    kPageSchemas = 0,
    kPageStyle,
    kPagePredict,
    kPageModels,
    kPagePersonal,
    kPageDict,
  };
  // 開啟時顯示的分頁（例如托盤的「用戶詞典管理」直接開到詞庫管理）
  void SetStartPage(int page) { start_page_ = page; }

  // 一組模型設定（本機 llama.cpp 或 OpenAI 相容 API）；預測與個人詞庫精煉各自選用一組
  struct ModelProfile {
    std::wstring name;
    bool remote = false;
    std::wstring model_path;           // 本機
    std::wstring model_type = L"Base";  // Base / Instruct
    std::wstring api_url;              // OpenAI 相容 API
    std::wstring api_key;
    std::wstring model;
    bool no_think = false;             // 關閉思考（思考型模型）
    int think_tokens = 2048;           // 開啟思考時的思考長度上限（0 = 不限制）
  };

 protected:
  enum {
    WM_APP_REFONT = WM_APP + 1,  // DPI 變更後重新套用自訂字型
    WM_APP_FILE_PROGRESS,        // 模型檔下載／複製進度（wParam：0 進行中、1 完成、2 失敗）
    WM_APP_API_TEST,             // API 連線測試完成
    WM_APP_WORD_RENAME,          // 詞彙清單的就地編輯結束後，送出修改
    kTimerTestPoll = 1,          // 等待輸入法回覆預測測試
    kTimerStatusPoll = 2,        // 重新部署後，等輸入法載入模型
    kTimerPersonalPoll = 3,      // 個人詞庫頁：更新狀態（精煉進度）
  };

  BEGIN_MSG_MAP(SettingsDialog)
  MESSAGE_HANDLER(WM_DPICHANGED, OnDpiChangedPre)
  CHAIN_MSG_MAP(CDialogDpiAware<SettingsDialog>)
  MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
  MESSAGE_HANDLER(WM_CLOSE, OnClose)
  MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
  MESSAGE_HANDLER(WM_APP_REFONT, OnRefont)
  MESSAGE_HANDLER(WM_APP_FILE_PROGRESS, OnFileProgress)
  MESSAGE_HANDLER(WM_APP_API_TEST, OnApiTestDone)
  MESSAGE_HANDLER(WM_APP_WORD_RENAME, OnWordRename)
  MESSAGE_HANDLER(WM_ERASEBKGND, OnEraseBkgnd)
  MESSAGE_HANDLER(WM_CTLCOLORDLG, OnCtlColorDlg)
  MESSAGE_HANDLER(WM_CTLCOLORSTATIC, OnCtlColorStatic)
  MESSAGE_HANDLER(WM_CTLCOLORBTN, OnCtlColorDlg)
  MESSAGE_HANDLER(WM_CTLCOLOREDIT, OnCtlColorEdit)
  MESSAGE_HANDLER(WM_CTLCOLORLISTBOX, OnCtlColorListBox)
  MESSAGE_HANDLER(WM_SETTINGCHANGE, OnSettingChange)
  COMMAND_HANDLER(IDC_THEME, CBN_SELCHANGE, OnThemeChange)
  COMMAND_HANDLER(IDC_P3_PREFIX, EN_CHANGE, OnLLMChanged)
  MESSAGE_HANDLER(WM_MEASUREITEM, OnMeasureItem)
  MESSAGE_HANDLER(WM_DRAWITEM, OnDrawItem)
  MESSAGE_HANDLER(WM_TIMER, OnTimer)
  COMMAND_HANDLER(IDC_NAV, LBN_SELCHANGE, OnNavChange)
  COMMAND_ID_HANDLER(IDOK, OnOK)
  COMMAND_ID_HANDLER(IDCANCEL, OnCancel)
  COMMAND_ID_HANDLER(IDC_APPLY, OnApply)
  COMMAND_ID_HANDLER(IDC_P1_GET_SCHEMATA, OnGetSchemata)
  NOTIFY_HANDLER(IDC_P1_SCHEMA_LIST, LVN_ITEMCHANGED, OnSchemaListItemChanged)
  COMMAND_ID_HANDLER(IDC_P1_TYPO_RIME, OnTypoChange)
  COMMAND_ID_HANDLER(IDC_P1_TYPO_LLM, OnTypoChange)
  COMMAND_HANDLER(IDC_P1_TYPO_PROFILE, CBN_SELCHANGE, OnProfileChoice)
  COMMAND_HANDLER(IDC_P2_COLOR_SCHEME, LBN_SELCHANGE, OnColorSchemeChange)
  COMMAND_ID_HANDLER(IDC_P2_SELECT_FONT, OnSelectFont)
  COMMAND_ID_HANDLER(IDC_P3_ENABLED, OnLLMEnabledClick)
  COMMAND_ID_HANDLER(IDC_P3_AFTER_COMMIT, OnLLMChanged)
  COMMAND_ID_HANDLER(IDC_P3_WHILE_TYPING, OnLLMChanged)
  COMMAND_HANDLER(IDC_P3_PROFILE, CBN_SELCHANGE, OnProfileChoice)
  COMMAND_ID_HANDLER(IDC_P3_MANAGE, OnGoModels)
  COMMAND_ID_HANDLER(IDC_P3_TEST_RUN, OnTestRun)
  // 語言模型
  COMMAND_HANDLER(IDC_P5_LIST, LBN_SELCHANGE, OnProfileSelect)
  COMMAND_ID_HANDLER(IDC_P5_ADD, OnProfileAdd)
  COMMAND_ID_HANDLER(IDC_P5_COPY, OnProfileAdd)
  COMMAND_ID_HANDLER(IDC_P5_DELETE, OnProfileDelete)
  COMMAND_HANDLER(IDC_P5_NAME, EN_CHANGE, OnProfileEdit)
  COMMAND_HANDLER(IDC_P5_API_URL, EN_CHANGE, OnProfileEdit)
  COMMAND_HANDLER(IDC_P5_API_KEY, EN_CHANGE, OnProfileEdit)
  COMMAND_HANDLER(IDC_P5_API_MODEL, EN_CHANGE, OnProfileEdit)
  COMMAND_ID_HANDLER(IDC_P5_NO_THINK, OnProfileEdit)
  COMMAND_HANDLER(IDC_P5_THINK_TOKENS, EN_CHANGE, OnProfileEdit)
  COMMAND_HANDLER(IDC_P5_TYPE, CBN_SELCHANGE, OnProfileEdit)
  COMMAND_HANDLER(IDC_P5_MODEL, CBN_SELCHANGE, OnModelChange)
  COMMAND_ID_HANDLER(IDC_P5_BROWSE, OnBrowseModel)
  COMMAND_HANDLER(IDC_P5_FILES, LBN_SELCHANGE, OnModelFileSel)
  COMMAND_ID_HANDLER(IDC_P5_FILE_ADD, OnModelFileAdd)
  COMMAND_ID_HANDLER(IDC_P5_FILE_DELETE, OnModelFileDelete)
  COMMAND_ID_HANDLER(IDC_P5_FILE_OPEN, OnModelFileOpen)
  COMMAND_ID_HANDLER(IDC_P5_DOWNLOAD, OnModelDownload)
  COMMAND_ID_HANDLER(IDC_P5_API_TEST, OnApiTest)
  COMMAND_ID_HANDLER(IDC_P5_LOCAL, OnProviderChange)
  COMMAND_ID_HANDLER(IDC_P5_REMOTE, OnProviderChange)
  COMMAND_HANDLER(IDC_P5_LOCAL_LABEL, STN_CLICKED, OnProviderLabelClick)
  COMMAND_HANDLER(IDC_P5_REMOTE_LABEL, STN_CLICKED, OnProviderLabelClick)
  // 個人詞庫
  COMMAND_ID_HANDLER(IDC_P4_ENABLED, OnPersonalEnabledClick)
  COMMAND_ID_HANDLER(IDC_P4_KEEP_LOG, OnPersonalChanged)
  COMMAND_ID_HANDLER(IDC_P4_RIME_BOOST, OnPersonalChanged)
  COMMAND_HANDLER(IDC_P4_MAX, CBN_SELCHANGE, OnPersonalChanged)
  COMMAND_HANDLER(IDC_P4_HALFLIFE, EN_CHANGE, OnPersonalChanged)
  COMMAND_HANDLER(IDC_P4_INTERVAL, EN_CHANGE, OnPersonalChanged)
  COMMAND_HANDLER(IDC_P4_PROFILE, CBN_SELCHANGE, OnProfileChoice)
  COMMAND_ID_HANDLER(IDC_P4_MANAGE, OnGoModels)
  COMMAND_ID_HANDLER(IDC_P4_WORDS, OnGoDict)
  COMMAND_ID_HANDLER(IDC_P4_REFINE, OnPersonalCommand)
  COMMAND_ID_HANDLER(IDC_P4_REFINE_ALL, OnPersonalCommand)
  COMMAND_ID_HANDLER(IDC_P4_CLEAR, OnPersonalCommand)
  // 詞庫管理
  COMMAND_HANDLER(IDC_P6_FILTER, EN_CHANGE, OnWordFilter)
  COMMAND_ID_HANDLER(IDC_P6_ADD, OnWordEdit)
  COMMAND_ID_HANDLER(IDC_P6_MERGE, OnWordEdit)
  COMMAND_ID_HANDLER(IDC_P6_BLOCK, OnWordEdit)
  COMMAND_ID_HANDLER(IDC_P6_DELETE, OnWordEdit)
  COMMAND_ID_HANDLER(IDC_P6_UNRULE, OnWordEdit)
  COMMAND_ID_HANDLER(IDC_P6_REFRESH, OnWordRefresh)
  NOTIFY_HANDLER(IDC_P6_WORDS, LVN_ITEMCHANGED, OnWordSelChanged)
  NOTIFY_HANDLER(IDC_P6_WORDS, NM_DBLCLK, OnWordDblClick)
  NOTIFY_HANDLER(IDC_P6_WORDS, LVN_KEYDOWN, OnWordKeyDown)
  NOTIFY_HANDLER(IDC_P6_WORDS, LVN_BEGINLABELEDIT, OnWordBeginEdit)
  NOTIFY_HANDLER(IDC_P6_WORDS, LVN_ENDLABELEDIT, OnWordEndEdit)
  COMMAND_HANDLER(IDC_P6_DICTS, LBN_SELCHANGE, OnDictSelChange)
  COMMAND_ID_HANDLER(IDC_P6_BACKUP, OnDictCommand)
  COMMAND_ID_HANDLER(IDC_P6_RESTORE, OnDictCommand)
  COMMAND_ID_HANDLER(IDC_P6_EXPORT, OnDictCommand)
  COMMAND_ID_HANDLER(IDC_P6_IMPORT, OnDictCommand)
  END_MSG_MAP()

  LRESULT OnInitDialog(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnClose(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnDestroy(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnDpiChangedPre(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnRefont(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnFileProgress(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnApiTest(WORD, WORD, HWND, BOOL&);
  LRESULT OnApiTestDone(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnWordRename(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnModelFileSel(WORD, WORD, HWND, BOOL&);
  LRESULT OnModelFileAdd(WORD, WORD, HWND, BOOL&);
  LRESULT OnModelFileDelete(WORD, WORD, HWND, BOOL&);
  LRESULT OnModelFileOpen(WORD, WORD, HWND, BOOL&);
  LRESULT OnModelDownload(WORD, WORD, HWND, BOOL&);
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
  LRESULT OnTypoChange(WORD, WORD, HWND, BOOL&);
  LRESULT OnColorSchemeChange(WORD, WORD, HWND, BOOL&);
  LRESULT OnSelectFont(WORD, WORD, HWND, BOOL&);
  LRESULT OnLLMEnabledClick(WORD, WORD, HWND, BOOL&);
  LRESULT OnLLMChanged(WORD, WORD, HWND, BOOL&);
  LRESULT OnModelChange(WORD, WORD, HWND, BOOL&);
  LRESULT OnProviderChange(WORD, WORD, HWND, BOOL&);
  LRESULT OnProviderLabelClick(WORD, WORD, HWND, BOOL&);
  LRESULT OnBrowseModel(WORD, WORD, HWND, BOOL&);
  LRESULT OnTestRun(WORD, WORD, HWND, BOOL&);
  LRESULT OnProfileChoice(WORD, WORD, HWND, BOOL&);
  LRESULT OnGoModels(WORD, WORD, HWND, BOOL&);
  LRESULT OnGoDict(WORD, WORD, HWND, BOOL&);
  LRESULT OnProfileSelect(WORD, WORD, HWND, BOOL&);
  LRESULT OnProfileAdd(WORD, WORD, HWND, BOOL&);
  LRESULT OnProfileDelete(WORD, WORD, HWND, BOOL&);
  LRESULT OnProfileEdit(WORD, WORD, HWND, BOOL&);
  LRESULT OnWordFilter(WORD, WORD, HWND, BOOL&);
  LRESULT OnWordEdit(WORD, WORD, HWND, BOOL&);
  LRESULT OnWordRefresh(WORD, WORD, HWND, BOOL&);
  LRESULT OnWordSelChanged(int, LPNMHDR, BOOL&);
  LRESULT OnWordDblClick(int, LPNMHDR, BOOL&);
  LRESULT OnWordKeyDown(int, LPNMHDR, BOOL&);
  LRESULT OnWordBeginEdit(int, LPNMHDR, BOOL&);
  LRESULT OnWordEndEdit(int, LPNMHDR, BOOL&);
  LRESULT OnDictSelChange(WORD, WORD, HWND, BOOL&);
  LRESULT OnDictCommand(WORD, WORD, HWND, BOOL&);
  LRESULT OnPersonalEnabledClick(WORD, WORD, HWND, BOOL&);
  LRESULT OnPersonalChanged(WORD, WORD, HWND, BOOL&);
  LRESULT OnPersonalCommand(WORD, WORD, HWND, BOOL&);

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
  // 注音容錯：Rime 容錯（llm/typo/rime）與 LLM 整句校正（llm/typo/llm）各自開關
  void UpdateTypoState();
  // Rime 容錯：在注音方案的 custom.yaml 打開 translator/enable_correction
  bool ApplyTypoCorrection(bool enable, std::wstring* error);
  // 外觀
  void PopulateColorSchemes();
  void PreviewColorScheme(int index);
  void UpdateFontSummary();
  // LLM 智慧預測
  void LoadLLMSettings();
  void PopulateModels(const std::wstring& current);
  int AddModel(const std::wstring& path);
  void UpdateLLMEnableState();
  bool IsRemoteProvider() const;  // 目前編輯的模型設定是 OpenAI 相容 API（否則為本機 llama.cpp）
  void GoToPage(int page);
  // 語言模型（多組設定）
  void LoadProfiles(RimeConfig* config);
  void SaveProfiles(RimeConfig* llm);
  bool ValidateProfiles();
  void PopulateProfileList();
  void SelectProfile(int index);    // 顯示到右側編輯區
  void UpdateThinkState();          // 關閉思考時停用「思考長度上限」
  void CommitProfileEditor();       // 編輯區 → profiles_
  // 預測與精煉的下拉選單；參數為要選的 profiles_ 索引（-1 = 不選，-2 = 維持目前的選擇）
  void RefreshProfileCombos(int predict = -2, int refine = -2, int typo = -2);
  void UpdateProfileUsage();
  // 模型檔案（使用者資料夾的 models）
  void PopulateModelFiles(const std::wstring& select = L"");
  void UpdateModelFileButtons();
  // 在背景下載或複製模型檔（src 為網址或本機路徑）
  void StartModelFileJob(const std::wstring& src, const std::wstring& dest, bool download);
  void SetFileStatus(const std::wstring& text);
  int ComboProfile(int combo_id) const;  // 下拉選單選到的 profiles_ 索引（-1 = 沒有 / 不使用）
  static std::wstring ProfileLabel(const ModelProfile& p);
  bool SaveLLMSettings();
  // 預測測試（透過正在執行的輸入法）
  void SendLLMRequest(const std::wstring& context, bool is_test);
  bool PollLLMResponse();
  // 個人詞庫
  void LoadPersonalSettings(RimeConfig* llm_config);
  void SavePersonalSettings(RimeConfig* llm);
  void UpdatePersonalEnableState();
  // 注音排序：修改注音方案的 custom.yaml 改用 terra_pinyin.personal，並準備好詞典檔
  bool ApplyRimeBoost(bool enable, std::wstring* error);
  bool SendPersonalCommand(DWORD command);  // 見 WEASEL_IPC_PERSONAL
  void RefreshPersonalStatus();             // 讀 personal/status.txt
  // 詞庫管理
  void LoadPersonalWords();                 // 請輸入法匯出詞彙與規則後讀回
  void PopulateWordList();
  void PopulateRuleList();
  bool SendWordEdits(const std::vector<std::wstring>& lines);  // 格式見 PersonalLexicon::ApplyEdits
  bool EndWordLabelEdit(bool save);  // 詞彙清單正在就地編輯時結束編輯；沒在編輯時回傳 false
  void PopulateDicts();
  void UpdateDictButtons();

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
  bool personal_modified_ = false;
  bool rime_boost_loaded_ = false;  // 載入時的「注音排序」設定（變更時才改方案）
  bool typo_modified_ = false;
  bool typo_rime_loaded_ = false;   // 載入時的 Rime 容錯設定（有變才改方案）
  int start_page_ = 0;

  // 語言模型
  std::vector<ModelProfile> profiles_;
  int profile_sel_ = -1;
  bool loading_profile_ = false;  // 載入編輯區時不觸發變更
  CListBox model_files_;
  std::vector<std::wstring> model_file_paths_;  // 與 model_files_ 的項目一一對應
  std::thread file_worker_;
  std::atomic<bool> file_busy_{false};
  std::atomic<bool> file_cancel_{false};
  std::mutex file_mutex_;
  std::wstring file_message_;   // 背景工作的進度／結果文字（file_mutex_）
  std::wstring file_result_;    // 完成的檔案路徑
  std::thread api_worker_;      // API 連線測試
  std::atomic<bool> api_busy_{false};
  std::wstring api_result_;     // 測試結果（file_mutex_）
  int api_profile_ = -1;        // 測試的是哪一組
  CListBox profile_list_;
  CComboBox predict_profile_;
  CComboBox typo_profile_;  // 注音校正使用的模型
  CComboBox refine_profile_;

  // 詞庫管理
  struct WordRule {
    enum Kind { kBlock, kMerge, kAdd } kind;
    std::wstring from, to;
  };
  std::vector<std::pair<std::wstring, double>> words_;
  std::vector<WordRule> rules_;
  bool words_loaded_ = false;
  std::pair<std::wstring, std::wstring> pending_rename_;  // 就地編輯：原寫法 → 新寫法
  bool word_edit_cancel_ = false;  // 就地編輯按了 Esc：結束時不套用
  bool personal_disabled_ = false;
  CListViewCtrl words_list_;
  CListViewCtrl rules_list_;
  CListBox dicts_;
  bool dicts_loaded_ = false;
  bool dict_task_ready_ = false;
  bool personal_running_ = false;
  int personal_ticks_ = 0;

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
