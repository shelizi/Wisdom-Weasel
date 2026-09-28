#import "WisdomBridge.h"

#import <AppKit/AppKit.h>

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "../../core/base/utf8.h"
#include "../../core/ime/controller.h"
#include "../../core/ime/prediction_engine.h"
#include "../../core/llm/ContextHistory.h"
#include "../settings/WisdomSettingsWindow.h"

namespace {

// 在主執行緒執行並等它完成（已經在主執行緒就直接執行，避免死結）
void RunOnMain(void (^block)(void)) {
  if ([NSThread isMainThread])
    block();
  else
    dispatch_sync(dispatch_get_main_queue(), block);
}

NSString* NS(const std::wstring& s) {
  return [NSString stringWithUTF8String:utf8::FromWide(s).c_str()] ?: @"";
}

std::string U8(NSString* s) {
  return s.UTF8String ? std::string(s.UTF8String) : std::string();
}

// 鼠鬚管端的 ime::Frontend
class MacFrontend : public ime::Frontend {
 public:
  struct Entry {
    ime::SessionState state;
    std::wstring client;
    void (^refresh)(void) = nil;
  };

  __weak WisdomBridge* bridge = nil;
  std::map<uint64_t, Entry> sessions;
  std::mutex mutex;  // 服務端的鎖：主執行緒與背景預測共用

  ime::SessionState* Session(uint64_t id) override {
    auto it = sessions.find(id);
    return it == sessions.end() ? nullptr : &it->second.state;
  }
  void Refresh(uint64_t id) override {
    auto it = sessions.find(id);
    if (it == sessions.end() || !it->second.refresh)
      return;
    // 可能在按鍵處理中或背景執行緒：一律排到主執行緒，避免在鎖內重入鼠鬚管
    void (^refresh)(void) = it->second.refresh;
    dispatch_async(dispatch_get_main_queue(), refresh);
  }
  void HideCandidates() override {
    void (^hide)(void) = bridge.hideCandidates;
    if (hide)
      dispatch_async(dispatch_get_main_queue(), hide);
  }
  std::mutex& ApiMutex() override { return mutex; }
  std::wstring ContextKey(uint64_t id) override {
    auto it = sessions.find(id);
    return it == sessions.end() ? std::wstring() : it->second.client;
  }
  void Redeploy() override {
    void (^redeploy)(void) = bridge.redeploy;
    if (redeploy)
      dispatch_async(dispatch_get_main_queue(), redeploy);
  }
  bool ReleaseSessions() override {
    // 呼叫端（精煉執行緒）持有鎖：不能等主執行緒，主執行緒可能正在等這把鎖
    BOOL (^release)(void) = bridge.releaseSessions;
    const bool ok = release ? release() : (rime_get_api()->cleanup_all_sessions(), true);
    if (ok)
      sessions.clear();
    return ok;
  }
};

}  // namespace

@implementation WisdomPreedit
@end

@implementation WisdomCandidate
@end

@implementation WisdomBridge {
  std::unique_ptr<MacFrontend> _frontend;
  std::unique_ptr<ContextHistory> _history;
  std::unique_ptr<ime::Controller> _controller;
}

+ (WisdomBridge*)shared {
  static WisdomBridge* bridge = nil;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    bridge = [[WisdomBridge alloc] init];
  });
  return bridge;
}

- (void)startWithUserDirectory:(NSString*)userDir sharedDirectory:(NSString*)sharedDir {
  if (_controller)
    return;
  _frontend = std::make_unique<MacFrontend>();
  _frontend->bridge = self;
  _history = std::make_unique<ContextHistory>();
  ime::Controller::Options options;
  options.user_dir = U8(userDir);
  options.shared_dir = U8(sharedDir);
  options.config_id = "squirrel";
  NSString* version = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
  options.version = U8(version ?: @"unknown");
  _controller = std::make_unique<ime::Controller>(rime_get_api(), _frontend.get(), options);
  _controller->SetContextHistory(_history.get());
  [self reloadConfig];
}

- (void)reloadConfig {
  if (!_controller)
    return;
  RimeApi* api = rime_get_api();
  RimeConfig config = {NULL};
  if (!api->config_open("squirrel", &config))
    return;
  std::lock_guard<std::mutex> lock(_frontend->mutex);
  _controller->LoadConfig(&config);
  api->config_close(&config);
}

- (void)stop {
  if (!_controller)
    return;
  std::lock_guard<std::mutex> lock(_frontend->mutex);
  _controller->Retire();
}

- (void)addSession:(RimeSessionId)session client:(NSString*)client refresh:(void (^)(void))refresh {
  if (!_frontend)
    return;
  std::lock_guard<std::mutex> lock(_frontend->mutex);
  MacFrontend::Entry& entry = _frontend->sessions[(uint64_t)session];
  entry.state = ime::SessionState();
  entry.state.session_id = session;
  entry.client = utf8::ToWide(U8(client));
  entry.refresh = [refresh copy];
}

- (void)removeSession:(RimeSessionId)session {
  if (!_frontend)
    return;
  std::lock_guard<std::mutex> lock(_frontend->mutex);
  _frontend->sessions.erase((uint64_t)session);
}

- (void)setClient:(NSString*)client forSession:(RimeSessionId)session {
  if (!_frontend)
    return;
  std::lock_guard<std::mutex> lock(_frontend->mutex);
  auto it = _frontend->sessions.find((uint64_t)session);
  if (it != _frontend->sessions.end())
    it->second.client = utf8::ToWide(U8(client));
}

- (WisdomKeyResult)processKey:(int)keycode modifiers:(int)modifiers session:(RimeSessionId)session {
  if (!_controller)
    return rime_get_api()->process_key(session, keycode, modifiers) ? (WisdomKeyHandled | WisdomKeyRespond)
                                                                     : WisdomKeyRespond;
  std::lock_guard<std::mutex> lock(_frontend->mutex);
  const ime::KeyResult result = _controller->ProcessKey((uint64_t)session, keycode, modifiers);
  WisdomKeyResult flags = 0;
  if (result.handled)
    flags |= WisdomKeyHandled;
  if (result.respond)
    flags |= WisdomKeyRespond;
  return flags;
}

- (NSArray<NSString*>*)takeCommitsForSession:(RimeSessionId)session {
  NSMutableArray<NSString*>* commits = [NSMutableArray array];
  if (!_controller)
    return commits;
  std::lock_guard<std::mutex> lock(_frontend->mutex);
  for (const std::wstring& text : _controller->TakeCommits((uint64_t)session))
    [commits addObject:NS(text)];
  return commits;
}

- (nullable WisdomPreedit*)preeditForSession:(RimeSessionId)session zhuyinPreview:(BOOL)zhuyinPreview {
  if (!_controller)
    return nil;
  RimeApi* api = rime_get_api();
  std::lock_guard<std::mutex> lock(_frontend->mutex);
  ime::SessionState* ss = _frontend->Session((uint64_t)session);
  if (!ss)
    return nil;
  bool composing = false, ascii = false;
  RIME_STRUCT(RimeStatus, status);
  if (api->get_status(session, &status)) {
    composing = !!status.is_composing;
    ascii = !!status.is_ascii_mode;
    api->free_status(&status);
  }
  RIME_STRUCT(RimeContext, ctx);
  const bool has_ctx = !!api->get_context(session, &ctx);
  _controller->UpdateComposition((uint64_t)session, composing, has_ctx ? &ctx : nullptr);
  ime::Preedit preedit;
  bool has_preedit = false;
  if (has_ctx && composing) {
    if (zhuyinPreview)
      has_preedit = _controller->PreviewPreedit((uint64_t)session, ctx, ascii, &preedit);
    if (!has_preedit && ss->mixed_active() && ctx.composition.preedit) {
      // 混打時要把 Rime 的組字接在後面：換算成 wchar_t 位置
      const std::string raw = ctx.composition.preedit;
      auto wide_len = [&](int bytes) {
        return (int)utf8::ToWide(raw.substr(0, (size_t)std::max(0, std::min(bytes, (int)raw.size())))).size();
      };
      preedit.text = utf8::ToWide(raw);
      if (ctx.composition.sel_start <= ctx.composition.sel_end) {
        preedit.sel_start = wide_len(ctx.composition.sel_start);
        preedit.sel_end = wide_len(ctx.composition.sel_end);
        preedit.cursor = wide_len(ctx.composition.cursor_pos);
      }
      has_preedit = true;
    }
  }
  if (has_ctx)
    api->free_context(&ctx);
  if (ss->mixed_active()) {
    preedit = ime::Controller::WithMixedText(*ss, has_preedit ? &preedit : nullptr);
    has_preedit = true;
  }
  if (!has_preedit)
    return nil;
  // wchar_t（UTF-32）位置換成 NSString 的 UTF-16 位置
  auto utf16 = [&](int pos) -> NSInteger {
    if (pos < 0)
      return -1;
    return (NSInteger)NS(preedit.text.substr(0, std::min((size_t)pos, preedit.text.size()))).length;
  };
  WisdomPreedit* out = [[WisdomPreedit alloc] init];
  out.text = NS(preedit.text);
  out.selStart = utf16(preedit.sel_start);
  out.selEnd = utf16(preedit.sel_end);
  out.cursor = utf16(preedit.cursor);
  return out;
}

- (NSArray<WisdomCandidate*>*)predictionCandidates {
  NSMutableArray<WisdomCandidate*>* list = [NSMutableArray array];
  if (!_controller)
    return list;
  std::lock_guard<std::mutex> lock(_frontend->mutex);
  if (!_controller->ShowingPredictions())
    return list;
  const ime::PredictionSet set = _controller->Predictions();
  for (size_t i = 0; i < set.candidates.size(); ++i) {
    WisdomCandidate* c = [[WisdomCandidate alloc] init];
    c.text = NS(set.candidates[i]);
    // Tab 選第一個，Shift+2~5 選其餘（數字鍵在注音中是注音符號）
    c.label = i == 0 ? @"Tab" : [NSString stringWithFormat:@"⇧%zu", i + 1];
    c.comment = set.IsRecommend(i) ? @"推薦" : set.IsCorrection(i) ? @"校正" : @"";
    [list addObject:c];
  }
  return list;
}

- (BOOL)selectCandidate:(NSUInteger)index session:(RimeSessionId)session {
  if (!_controller)
    return NO;
  std::lock_guard<std::mutex> lock(_frontend->mutex);
  return _controller->SelectCandidate((uint64_t)session, index) ? YES : NO;
}

- (void)commitComposition:(RimeSessionId)session {
  if (!_controller) {
    rime_get_api()->commit_composition(session);
    return;
  }
  std::lock_guard<std::mutex> lock(_frontend->mutex);
  _controller->CommitComposition((uint64_t)session);
}

- (void)clearComposition:(RimeSessionId)session {
  if (!_controller) {
    rime_get_api()->clear_composition(session);
    return;
  }
  std::lock_guard<std::mutex> lock(_frontend->mutex);
  _controller->ClearComposition((uint64_t)session);
}

- (void)focusOut:(RimeSessionId)session {
  if (!_controller)
    return;
  std::lock_guard<std::mutex> lock(_frontend->mutex);
  _controller->FocusOut((uint64_t)session);
}

- (void)showSettings {
  NSString* webDir = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"web/settings"];
  __weak WisdomBridge* weakSelf = self;
  MacSettingsPlatform::Hooks hooks;
  // 設定後端在背景執行緒呼叫這些：排到主執行緒，在鎖內交給控制器
  hooks.personal_command = [weakSelf](unsigned command) {
    dispatch_async(dispatch_get_main_queue(), ^{
      WisdomBridge* bridge = weakSelf;
      if (!bridge || !bridge->_controller)
        return;
      std::lock_guard<std::mutex> lock(bridge->_frontend->mutex);
      bridge->_controller->PersonalCommand((int)command);
    });
  };
  hooks.llm_test = [weakSelf]() {
    dispatch_async(dispatch_get_main_queue(), ^{
      WisdomBridge* bridge = weakSelf;
      if (!bridge || !bridge->_controller)
        return;
      std::lock_guard<std::mutex> lock(bridge->_frontend->mutex);
      bridge->_controller->LLMTest();
    });
  };
  hooks.start_maintenance = [weakSelf]() {
    RunOnMain(^{
      void (^start)(void) = weakSelf.startMaintenance;
      if (start)
        start();
    });
  };
  hooks.end_maintenance = [weakSelf]() {
    RunOnMain(^{
      void (^end)(void) = weakSelf.endMaintenance;
      if (end)
        end();
    });
  };
  hooks.deploy = [weakSelf]() {
    // 鼠鬚管的重新部署是非同步的：等維護執行緒結束再回報。llm/* 由鼠鬚管在部署完成後重新讀（reloadConfig）
    RunOnMain(^{
      void (^redeploy)(void) = weakSelf.redeploy;
      if (redeploy)
        redeploy();
    });
    rime_get_api()->join_maintenance_thread();
  };
  [WisdomSettingsWindowController showWithWebDirectory:webDir hooks:hooks];
}

@end
