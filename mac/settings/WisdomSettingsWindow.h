#pragma once

// 網頁版設定視窗（WKWebView），與 Windows 的 WebView2 視窗共用 web/settings 的網頁與 settings::Backend。
// 在鼠鬚管（輸入法）行程裡開啟；呼叫前 Rime 必須已經 setup。
// 注意：尚未在 macOS 上編譯驗證。
#import <AppKit/AppKit.h>

#include "MacSettingsPlatform.h"

@interface WisdomSettingsWindowController : NSWindowController

// web_dir：web/settings 資料夾（通常在 app 的 Resources/web/settings）
+ (void)showWithWebDirectory:(NSString*)webDir hooks:(MacSettingsPlatform::Hooks)hooks;

@end
