#include "stdafx.h"
#include <StringAlgorithm.hpp>
#include <WeaselIPC.h>
#include <FixedWMemStreamBuf.h>
#include "Deserializer.h"

using namespace weasel;

ResponseParser::ResponseParser(std::wstring* commit,
                               Context* context,
                               Status* status,
                               Config* config,
                               UIStyle* style)
    : p_commit(commit),
      p_context(context),
      p_status(status),
      p_config(config),
      p_style(style) {
  Deserializer::Initialize(this);
}

bool ResponseParser::operator()(LPWSTR buffer, UINT length) {
  try {
    WMemStream bs((wchar_t*)buffer, length);
    std::wstring line;
    while (bs.good()) {
      std::getline(bs, line);
      if (!bs.good())
        return false;

      // file ends
      if (line == L".")
        break;

      Feed(line);
    }
    return bs.good();
  } catch (...) {
    // 运行在宿主应用进程内，绝不让异常逃逸
    OutputDebugStringA("[weasel] IPC response dropped: exception while parsing\n");
    return false;
  }
}

void ResponseParser::Feed(const std::wstring& line) {
  // ignore blank lines and comments
  if (line.empty() || line.find_first_of(L'#') == 0)
    return;

  Deserializer::KeyType key;
  std::wstring value;

  // extract key (split by L'.') and value
  std::wstring::size_type sep_pos = line.find_first_of(L'=');
  if (sep_pos == std::wstring::npos)
    return;
  split(key, line.substr(0, sep_pos), L".");
  if (key.empty())
    return;
  value = line.substr(sep_pos + 1);

  // first part of the key serve as action type
  std::wstring const& action = key[0];

  // get required action deserializer instance
  std::map<std::wstring, Deserializer::Ptr>::iterator i =
      deserializers.find(action);
  if (i == deserializers.end()) {
    // line ignored... since corresponding deserializer is not active
    return;
  }

  // dispatch
  // 本代码运行在宿主应用进程内（TSF/IME），任何异常都不能逃逸，否则会拖垮宿主应用；
  // 解析失败时丢弃该行即可
  Deserializer::Ptr p = i->second;
  try {
    p->Store(key, value);
  } catch (const std::exception& e) {
    OutputDebugStringA("[weasel] IPC line dropped: ");
    OutputDebugStringA(e.what());
    OutputDebugStringA("\n");
  } catch (...) {
    OutputDebugStringA("[weasel] IPC line dropped: unknown exception\n");
  }
}
