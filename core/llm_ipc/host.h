#pragma once

// 推理行程端：依序處理輸入法送來的請求，直到輸入法關閉管道為止。
#include <functional>
#include <string>

#include "protocol.h"

namespace llm_ipc {

// 處理一個請求並填好回覆；cancelled() 在輸入法取消這個請求後回傳 true
using Handler =
    std::function<void(Reader& request, Writer& reply, const std::function<bool()>& cancelled)>;

void Serve(platform::Pipe& pipe, const Handler& handler);

// 送一行開發終端記錄給輸入法（任何執行緒都可以呼叫；Serve 以外的時間呼叫會被忽略）
void Log(const std::string& text);

}  // namespace llm_ipc
