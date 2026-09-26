#pragma once

#include <Windows.h>
#include <string>
#include <memory>
#include <thread>

// 开发终端类，用于实时显示调试日志
// 与现有日志功能独立，通过配置文件控制是否启用
class DevConsole {
 public:
  DevConsole();
  ~DevConsole();

  // 初始化开发终端（从配置文件读取是否启用）
  // 返回true表示成功启用，false表示未启用或初始化失败
  bool Initialize();

  // 检查是否已启用
  bool IsEnabled() const { return m_enabled; }

  // 输出日志到控制台
  void Write(const std::string& message);
  void WriteLine(const std::string& message);
  void Write(const std::wstring& message);
  void WriteLine(const std::wstring& message);

  // 关闭控制台
  void Close();

 private:
  bool m_enabled;
  bool m_console_allocated;
  HANDLE m_hConsoleOutput;
  HANDLE m_hConsoleInput;

  // 非同步寫入：Write 只把文字放進佇列，由背景執行緒寫到主控台。
  // 主控台在選取模式（QuickEdit）或輸出來不及時會暫停寫入，同步寫會卡住呼叫端，
  // 而按鍵處理握著服務端的鎖，其他應用程式就跟著凍結
  struct Queue;
  std::shared_ptr<Queue> m_queue;
  void StartWriter();
  void StopWriter();

  // 从配置文件读取是否启用开发终端
  bool LoadConfig();

  // 分配控制台窗口
  bool AllocateConsole();

  // 释放控制台窗口
  void FreeConsole();

  // 重定向标准输出到控制台
  bool RedirectStdout();

  // 恢复标准输出
  void RestoreStdout();
};

