// 推理行程裡的開發終端：不開自己的主控台，把文字原樣轉給輸入法的開發終端顯示
#include "stdafx.h"
#include "../WeaselServer/DevConsole.h"
#include "../core/llm_ipc/host.h"
#include <WeaselUtility.h>

DevConsole::DevConsole()
    : m_enabled(false),
      m_console_allocated(false),
      m_hConsoleOutput(nullptr),
      m_hConsoleInput(nullptr) {}

DevConsole::~DevConsole() = default;

bool DevConsole::Initialize() {
  m_enabled = true;
  return true;
}

void DevConsole::Write(const std::string& message) {
  if (m_enabled && !message.empty())
    llm_ipc::Log(message);
}

void DevConsole::WriteLine(const std::string& message) {
  Write(message + "\r\n");
}

void DevConsole::Write(const std::wstring& message) {
  Write(wtou8(message));
}

void DevConsole::WriteLine(const std::wstring& message) {
  Write(wtou8(message) + "\r\n");
}

void DevConsole::Close() {
  m_enabled = false;
}
