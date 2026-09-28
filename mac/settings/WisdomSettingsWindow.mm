//
// 網頁從自訂網址 wisdom-settings://app/ 載入（WKURLSchemeHandler 讀 web/settings 的檔案），
// 對應 Windows 的 WebView2 虛擬主機；用 file:// 載入時 WebKit 不允許 ES module。
// 訊息：網頁 → window.webkit.messageHandlers.bridge.postMessage({id, method, params})
//       原生 → window.__bridgeReceive({id, result} / {id, error} / {event, data})
#import "WisdomSettingsWindow.h"

#import <WebKit/WebKit.h>

#include <memory>
#include <string>

#include "../../core/base/utf8.h"
#include "../../core/llm/LLMProvider.h"
#include "../../core/settings/backend.h"
#include "../../core/third_party/nlohmann/json.hpp"

using json = nlohmann::json;
using settings::Backend;

static NSString* const kScheme = @"wisdom-settings";

// 後端與它用的平台功能一起保存：背景工作可能比視窗晚結束，兩者要一起活著
struct BackendHolder {
  MacSettingsPlatform platform;
  std::unique_ptr<Backend> backend;
  explicit BackendHolder(MacSettingsPlatform::Hooks hooks) : platform(std::move(hooks)) {}
};

// ---------------------------------------------------------------------------
// 提供網頁檔案

@interface WisdomSchemeHandler : NSObject <WKURLSchemeHandler>
@property(nonatomic, copy) NSString* root;
@end

@implementation WisdomSchemeHandler

static NSString* MimeType(NSString* ext) {
  NSDictionary* types = @{
    @"html" : @"text/html",
    @"js" : @"text/javascript",
    @"mjs" : @"text/javascript",
    @"css" : @"text/css",
    @"json" : @"application/json",
    @"svg" : @"image/svg+xml",
    @"png" : @"image/png",
    @"woff2" : @"font/woff2",
  };
  return types[ext.lowercaseString] ?: @"application/octet-stream";
}

- (void)webView:(WKWebView*)webView startURLSchemeTask:(id<WKURLSchemeTask>)task {
  NSString* path = task.request.URL.path;
  if (path.length == 0 || [path isEqualToString:@"/"])
    path = @"/index.html";
  // 只提供 web/settings 底下的檔案
  NSString* full = [[self.root stringByAppendingPathComponent:path] stringByStandardizingPath];
  NSData* data = [full hasPrefix:[self.root stringByStandardizingPath]] ? [NSData dataWithContentsOfFile:full] : nil;
  if (!data) {
    [task didFailWithError:[NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorFileDoesNotExist userInfo:nil]];
    return;
  }
  NSString* mime = MimeType(full.pathExtension);
  NSHTTPURLResponse* response =
      [[NSHTTPURLResponse alloc] initWithURL:task.request.URL
                                  statusCode:200
                                 HTTPVersion:@"HTTP/1.1"
                                headerFields:@{
                                  @"Content-Type" : [mime stringByAppendingString:@"; charset=utf-8"],
                                  @"Content-Length" : [NSString stringWithFormat:@"%lu", (unsigned long)data.length],
                                  @"Cache-Control" : @"no-cache",
                                }];
  [task didReceiveResponse:response];
  [task didReceiveData:data];
  [task didFinish];
}

- (void)webView:(WKWebView*)webView stopURLSchemeTask:(id<WKURLSchemeTask>)task {
}

@end

// ---------------------------------------------------------------------------
// 視窗

@interface WisdomSettingsWindowController () <WKScriptMessageHandler, NSWindowDelegate>
@end

@implementation WisdomSettingsWindowController {
  WKWebView* _webView;
  std::shared_ptr<BackendHolder> _backend;
  dispatch_queue_t _rimeQueue;  // Rime 的呼叫依序在這裡執行（levers API 與部署不能同時呼叫）
  BOOL _allowClose;
}

static WisdomSettingsWindowController* g_current = nil;

// 鼠鬚管是背景 app（沒有 Dock 圖示），自己不在前景：showWindow 不會把視窗放到畫面上，
// macOS 14 起 activateIgnoringOtherApps 也不再搶前景。orderFrontRegardless 一定會顯示
static void BringToFront(NSWindow* window) {
  [window makeKeyAndOrderFront:nil];
  [window orderFrontRegardless];
  if (@available(macOS 14.0, *))
    [NSApp activate];
  else
    [NSApp activateIgnoringOtherApps:YES];
}

+ (void)showWithWebDirectory:(NSString*)webDir hooks:(MacSettingsPlatform::Hooks)hooks {
  if (g_current) {
    BringToFront(g_current.window);
    return;
  }
  WisdomSettingsWindowController* controller = [[WisdomSettingsWindowController alloc] initWithWebDirectory:webDir
                                                                                                        hooks:hooks];
  if (!controller)
    return;
  g_current = controller;
  [controller showWindow:nil];
  BringToFront(controller.window);
}

- (instancetype)initWithWebDirectory:(NSString*)webDir hooks:(MacSettingsPlatform::Hooks)hooks {
  NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1080, 760)
                                                 styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                           NSWindowStyleMaskResizable |
                                                           NSWindowStyleMaskMiniaturizable
                                                   backing:NSBackingStoreBuffered
                                                     defer:NO];
  window.title = @"鼠鬚管設定";
  window.minSize = NSMakeSize(720, 520);
  [window center];
  self = [super initWithWindow:window];
  if (!self)
    return nil;
  window.delegate = self;
  window.releasedWhenClosed = NO;
  _rimeQueue = dispatch_queue_create("wisdom.settings.rime", DISPATCH_QUEUE_SERIAL);

  // 事件（下載進度等）可能在任何執行緒送出；視窗關了就不送
  __weak WisdomSettingsWindowController* weakSelf = self;
  try {
    _backend = std::make_shared<BackendHolder>(std::move(hooks));
    Backend::Options options;
    options.config_id = "squirrel";
    options.generator_id = "Squirrel::UIStyleSettings";
    options.correct_instruction = utf8::FromWide(kLLMCorrectInstruction);
    _backend->backend = std::make_unique<Backend>(_backend->platform, options,
                                                  [weakSelf](const std::string& event, const json& data) {
                                                    const json message = {{"event", event}, {"data", data}};
                                                    [weakSelf postToPage:message];
                                                  });
  } catch (const std::exception& e) {
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = @"無法開啟設定";
    alert.informativeText = [NSString stringWithUTF8String:e.what()] ?: @"";
    [alert runModal];
    return nil;
  }

  WKWebViewConfiguration* config = [[WKWebViewConfiguration alloc] init];
  WisdomSchemeHandler* handler = [[WisdomSchemeHandler alloc] init];
  handler.root = webDir;
  [config setURLSchemeHandler:handler forURLScheme:kScheme];
  [config.userContentController addScriptMessageHandler:self name:@"bridge"];
  _webView = [[WKWebView alloc] initWithFrame:window.contentView.bounds configuration:config];
  _webView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
  [window.contentView addSubview:_webView];
  [_webView loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:@"wisdom-settings://app/index.html"]]];
  return self;
}

// 送給網頁（任何執行緒都可以呼叫）
- (void)postToPage:(const json&)message {
  // 以 UTF-8 組字串（stringWithFormat 的 %s 會照系統編碼解讀）；不合法的 UTF-8 以替代字元取代
  const std::string text =
      "window.__bridgeReceive(" + message.dump(-1, ' ', false, json::error_handler_t::replace) + ")";
  NSString* script = [NSString stringWithUTF8String:text.c_str()];
  __weak WKWebView* webView = _webView;
  dispatch_async(dispatch_get_main_queue(), ^{
    [webView evaluateJavaScript:script completionHandler:nil];
  });
}

- (void)reply:(const json&)identifier result:(const json&)result {
  const json message = {{"id", identifier}, {"result", result}};
  [self postToPage:message];
}

- (void)userContentController:(WKUserContentController*)controller didReceiveScriptMessage:(WKScriptMessage*)message {
  if (![message.body isKindOfClass:[NSDictionary class]])
    return;
  NSData* data = [NSJSONSerialization dataWithJSONObject:message.body options:0 error:nil];
  if (!data)
    return;
  const json request =
      json::parse(std::string((const char*)data.bytes, data.length), nullptr, /*allow_exceptions=*/false);
  if (!request.is_object() || !request.contains("method"))
    return;
  const json identifier = request.value("id", json());
  const std::string method = request.value("method", std::string());
  const json params = request.value("params", json::object());

  // 視窗自己處理的
  if (method == "app.ready" || method == "app.selftestDone") {
    [self reply:identifier result:json()];
    return;
  }
  if (method == "app.close") {
    [self reply:identifier result:json()];
    _allowClose = YES;
    [self.window performClose:nil];
    return;
  }
  if (method == "app.titleBar") {
    self.window.appearance =
        [NSAppearance appearanceNamed:params.value("dark", false) ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua];
    [self reply:identifier result:json()];
    return;
  }

  std::shared_ptr<BackendHolder> backend = _backend;
  __weak WisdomSettingsWindowController* weakSelf = self;
  void (^run)(void) = ^{
    json reply = {{"id", identifier}};
    try {
      reply["result"] = backend->backend->Call(method, params, nullptr);
    } catch (const std::exception& e) {
      reply["error"] = e.what();
    }
    [weakSelf postToPage:reply];
  };
  switch (Backend::ThreadOf(method)) {
    case Backend::Thread::kUi:
      dispatch_async(dispatch_get_main_queue(), run);  // 不在 WebKit 的回呼裡開對話框
      break;
    case Backend::Thread::kRime:
      dispatch_async(_rimeQueue, run);
      break;
    case Backend::Thread::kTask:
      dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), run);
      break;
  }
}

// 關閉：先問網頁（有未套用的修改時會詢問），網頁同意後呼叫 app.close
- (BOOL)windowShouldClose:(NSWindow*)sender {
  if (_allowClose)
    return YES;
  const json message = {{"event", "closeRequested"}, {"data", nullptr}};
  [self postToPage:message];
  return NO;
}

- (void)windowWillClose:(NSNotification*)notification {
  [_webView.configuration.userContentController removeScriptMessageHandlerForName:@"bridge"];
  if (g_current == self)
    g_current = nil;
}

@end
