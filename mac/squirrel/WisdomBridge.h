#pragma once

// 鼠鬚管（Squirrel）與共用輸入法控制器（core/ime）之間的橋接。
// 純 Objective-C 介面：Swift 版的鼠鬚管以 bridging header 匯入即可使用。
// 所有方法都在主執行緒呼叫；背景預測完成時，以 addSession 時給的 refresh 在主執行緒通知。
// 接到鼠鬚管的方式見 mac/README.md。
#import <Foundation/Foundation.h>

#include <rime_api.h>

NS_ASSUME_NONNULL_BEGIN

typedef NS_OPTIONS(NSUInteger, WisdomKeyResult) {
  WisdomKeyHandled = 1 << 0,  // 按鍵被輸入法吃掉
  WisdomKeyRespond = 1 << 1,  // 要更新（取出送出的文字、組字區與候選）
};

// 組字區：選取範圍與游標是 UTF-16 位置（NSString 的索引）
@interface WisdomPreedit : NSObject
@property(nonatomic, copy) NSString* text;
@property(nonatomic) NSInteger selStart;
@property(nonatomic) NSInteger selEnd;
@property(nonatomic) NSInteger cursor;  // -1 表示沒有游標資訊
@end

// 一個 LLM 候選（接在 Rime 候選後面顯示）
@interface WisdomCandidate : NSObject
@property(nonatomic, copy) NSString* text;
@property(nonatomic, copy) NSString* label;    // Tab、⇧2 …
@property(nonatomic, copy) NSString* comment;  // 推薦、校正或空字串
@end

@interface WisdomBridge : NSObject

@property(class, readonly) WisdomBridge* shared;

// 鼠鬚管自己的功能（啟動時設定）
@property(nonatomic, copy, nullable) void (^hideCandidates)(void);  // 收起候選窗
@property(nonatomic, copy, nullable) void (^redeploy)(void);        // 重新部署（非同步；部署完成後要呼叫 reloadConfig）
// 關掉所有 session 放開使用者詞典（整理選字記憶時）。在精煉的背景執行緒上、持有橋接層的鎖時呼叫：
// 不可呼叫橋接層、也不可等主執行緒。沒設定時直接呼叫 rime_api->cleanup_all_sessions()。
// 之後鼠鬚管發現 session 不在（find_session 為 false）要重建並再 addSession
@property(nonatomic, copy, nullable) BOOL (^releaseSessions)(void);
@property(nonatomic, copy, nullable) void (^startMaintenance)(void);
@property(nonatomic, copy, nullable) void (^endMaintenance)(void);

// 在 Rime setup 與 initialize 之後呼叫；userDir 通常是 ~/Library/Rime，sharedDir 是 app 的 SharedSupport
- (void)startWithUserDirectory:(NSString*)userDir sharedDirectory:(NSString*)sharedDir;
// 部署完成後重新讀設定（squirrel.yaml 的 llm/*）
- (void)reloadConfig;
// Rime finalize 之前呼叫：停止精煉並存檔
- (void)stop;

// session：鼠鬚管建立的 Rime session。client 是輸入中的應用程式（bundle id），各自一份前文
- (void)addSession:(RimeSessionId)session client:(NSString*)client refresh:(void (^)(void))refresh;
- (void)removeSession:(RimeSessionId)session;
- (void)setClient:(NSString*)client forSession:(RimeSessionId)session;

// 按鍵：keycode 是 Rime 的 keysym，modifiers 是 Rime 的修飾鍵（含放開），與傳給 rime_api->process_key 的相同。
// 取代直接呼叫 process_key
- (WisdomKeyResult)processKey:(int)keycode modifiers:(int)modifiers session:(RimeSessionId)session;
// 取代 rime_api->get_commit：依序取出要插入的文字（LLM 候選、混打內容、Rime 送出的字）
- (NSArray<NSString*>*)takeCommitsForSession:(RimeSessionId)session;
// 更新組字狀態並取得要顯示的組字區；回傳 nil 表示照 Rime 的 preedit 顯示。
// zhuyinPreview：鼠鬚管的組字區顯示轉好的字（像新注音）
- (nullable WisdomPreedit*)preeditForSession:(RimeSessionId)session zhuyinPreview:(BOOL)zhuyinPreview;

// LLM 候選（沒有時為空）；接在 Rime 候選之後，Tab、Shift+1~5 選取
- (NSArray<WisdomCandidate*>*)predictionCandidates;
// 候選窗點選第 index 個候選（Rime 候選之後接著 LLM 候選）；處理了 LLM 候選時回傳 YES
- (BOOL)selectCandidate:(NSUInteger)index session:(RimeSessionId)session;
- (void)commitComposition:(RimeSessionId)session;
- (void)clearComposition:(RimeSessionId)session;
- (void)focusOut:(RimeSessionId)session;

// 開啟網頁版設定（web/settings 在 app 的 Resources/web/settings）
- (void)showSettings;

@end

NS_ASSUME_NONNULL_END
