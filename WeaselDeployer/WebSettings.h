#pragma once

// 網頁版設定視窗：WebView2 載入安裝資料夾的 web\settings\index.html，網頁經由
// WebView2 的 訊息通道呼叫 SettingsBackend（不開本機伺服器、不佔連接埠）。
#include <string>

class Configurator;

struct WebSettingsOptions {
  int start_page = 0;
  // 開發用：載入後把畫面存成 PNG 就結束（檢查版面）；theme 覆寫主題（1 淺色、2
  // 深色）
  std::wstring screenshot;
  int theme = 0;
  int width = 0, height = 0;
  // 開發用：網頁跑自我測試（phase 決定測試的階段），結果以 JSON
  // 寫到這個檔案後結束
  std::wstring selftest;
  std::wstring selftest_phase;
};

// 0：正常關閉；-1：無法使用 WebView2（呼叫端可改開舊的設定視窗）
int RunWebSettings(Configurator* configurator,
                   const WebSettingsOptions& options);
