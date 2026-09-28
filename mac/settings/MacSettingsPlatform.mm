#import "MacSettingsPlatform.h"

#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <algorithm>
#include <cstdlib>

namespace fs = std::filesystem;

namespace {

NSString* NS(const std::string& s) {
  return [NSString stringWithUTF8String:s.c_str()] ?: @"";
}

fs::path Home() {
  const char* home = getenv("HOME");
  return fs::path(home ? home : "");
}

// 「*.txt;*.yaml」→ 副檔名；「*.*」或空的表示不限
NSArray<UTType*>* ContentTypes(const std::vector<settings::FileFilter>& filters) {
  NSMutableArray<UTType*>* types = [NSMutableArray array];
  for (const auto& f : filters) {
    std::string spec = f.spec;
    size_t start = 0;
    while (start <= spec.size()) {
      size_t end = spec.find(';', start);
      if (end == std::string::npos)
        end = spec.size();
      std::string pattern = spec.substr(start, end - start);
      const size_t dot = pattern.rfind('.');
      const std::string ext = dot == std::string::npos ? std::string() : pattern.substr(dot + 1);
      if (ext.empty() || ext == "*")
        return @[];  // 不限檔案類型
      if (UTType* type = [UTType typeWithFilenameExtension:NS(ext)])
        [types addObject:type];
      start = end + 1;
    }
  }
  return types;
}

}  // namespace

fs::path MacSettingsPlatform::UserDataDir() {
  return Home() / "Library" / "Rime";
}

fs::path MacSettingsPlatform::ModelsDir() {
  return Home() / "models";  // 與 Windows（%USERPROFILE%\models）相同
}

fs::path MacSettingsPlatform::FromRimePath(const char* path) {
  return fs::path(path ? path : "");  // macOS 上 librime 回傳 UTF-8
}

bool MacSettingsPlatform::SendPersonalCommand(unsigned command) {
  if (!hooks_.personal_command)
    return false;
  hooks_.personal_command(command);
  return true;
}

bool MacSettingsPlatform::NotifyLLMTest() {
  if (!hooks_.llm_test)
    return false;
  hooks_.llm_test();
  return true;
}

void MacSettingsPlatform::StartMaintenance() {
  if (hooks_.start_maintenance)
    hooks_.start_maintenance();
}

void MacSettingsPlatform::EndMaintenance() {
  if (hooks_.end_maintenance)
    hooks_.end_maintenance();
}

void MacSettingsPlatform::Deploy() {
  if (hooks_.deploy)
    hooks_.deploy();
}

bool MacSettingsPlatform::InstallSchemas(std::string* error) {
  // 鼠鬚管沒有附東風破；請使用者在終端機安裝
  *error =
      "請在終端機用東風破（plum）安裝輸入方案，例如：\n"
      "curl -fsSL https://raw.githubusercontent.com/rime/plum/master/rime-install | bash -s -- :preset\n"
      "安裝後回到這裡按「套用」重新部署。";
  return false;
}

fs::path MacSettingsPlatform::OpenFileDialog(void*, const std::string& title,
                                             const std::vector<settings::FileFilter>& filters,
                                             const std::string& file_name, const std::string&) {
  __block fs::path result;
  void (^run)(void) = ^{
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.title = NS(title);
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = NO;
    NSArray<UTType*>* types = ContentTypes(filters);
    if (types.count > 0)
      panel.allowedContentTypes = types;
    if (!file_name.empty())
      panel.nameFieldStringValue = NS(file_name);
    if ([panel runModal] == NSModalResponseOK && panel.URL)
      result = fs::path(panel.URL.fileSystemRepresentation);
  };
  if ([NSThread isMainThread])
    run();
  else
    dispatch_sync(dispatch_get_main_queue(), run);
  return result;
}

fs::path MacSettingsPlatform::SaveFileDialog(void*, const std::string& title,
                                             const std::vector<settings::FileFilter>& filters,
                                             const std::string& file_name, const std::string&) {
  __block fs::path result;
  void (^run)(void) = ^{
    NSSavePanel* panel = [NSSavePanel savePanel];
    panel.title = NS(title);
    NSArray<UTType*>* types = ContentTypes(filters);
    if (types.count > 0)
      panel.allowedContentTypes = types;
    if (!file_name.empty())
      panel.nameFieldStringValue = NS(file_name);
    if ([panel runModal] == NSModalResponseOK && panel.URL)
      result = fs::path(panel.URL.fileSystemRepresentation);
  };
  if ([NSThread isMainThread])
    run();
  else
    dispatch_sync(dispatch_get_main_queue(), run);
  return result;
}

void MacSettingsPlatform::Reveal(const fs::path& file) {
  NSURL* url = [NSURL fileURLWithPath:NS(file.string())];
  dispatch_async(dispatch_get_main_queue(), ^{
    [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[ url ]];
  });
}

void MacSettingsPlatform::OpenFolder(const fs::path& dir) {
  NSURL* url = [NSURL fileURLWithPath:NS(dir.string()) isDirectory:YES];
  dispatch_async(dispatch_get_main_queue(), ^{
    [[NSWorkspace sharedWorkspace] openURL:url];
  });
}

bool MacSettingsPlatform::MoveToTrash(const fs::path& file) {
  NSURL* url = [NSURL fileURLWithPath:NS(file.string())];
  return [[NSFileManager defaultManager] trashItemAtURL:url resultingItemURL:nil error:nil];
}

bool MacSettingsPlatform::FileInUse(const fs::path&) {
  return false;  // macOS 可以刪除開啟中的檔案
}

std::vector<std::string> MacSettingsPlatform::SystemFonts() {
  std::vector<std::string> fonts;
  for (NSString* family in [[NSFontManager sharedFontManager] availableFontFamilies]) {
    if (const char* name = family.UTF8String)
      fonts.push_back(name);
  }
  std::sort(fonts.begin(), fonts.end());
  fonts.erase(std::unique(fonts.begin(), fonts.end()), fonts.end());
  return fonts;
}

int MacSettingsPlatform::GetPreference(const std::string& key, int fallback) {
  NSString* k = NS("WisdomSettings." + key);
  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  return [defaults objectForKey:k] ? (int)[defaults integerForKey:k] : fallback;
}

void MacSettingsPlatform::SetPreference(const std::string& key, int value) {
  [[NSUserDefaults standardUserDefaults] setInteger:value forKey:NS("WisdomSettings." + key)];
}
