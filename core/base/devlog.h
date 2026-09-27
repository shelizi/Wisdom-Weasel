#pragma once

// 開發終端的日誌介面：共用程式碼透過 g_dev_console 寫日誌，
// 由各平台實作（Windows 輸入法的 DevConsole、推理行程轉給輸入法的 relay、…）
#include <string>

#include "utf8.h"

class DevLog {
 public:
  virtual ~DevLog() = default;
  virtual bool IsEnabled() const = 0;
  virtual void Write(const std::string& utf8) = 0;
  void WriteLine(const std::string& utf8) { Write(utf8 + "\r\n"); }
  void Write(const std::wstring& text) { Write(utf8::FromWide(text)); }
  void WriteLine(const std::wstring& text) { Write(utf8::FromWide(text) + "\r\n"); }
};

// 沒有開發終端時為 nullptr
inline DevLog* g_dev_console = nullptr;
