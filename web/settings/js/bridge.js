// 與原生端的訊息通道。
// Windows：WebView2 的 chrome.webview；macOS：WKWebView 的 webkit.messageHandlers.bridge
// （原生端以 window.__bridgeReceive(訊息) 回傳）。訊息格式：
//   網頁 → 原生  { id, method, params }
//   原生 → 網頁  { id, result } / { id, error }，或事件 { event, data }

const pending = new Map();
const listeners = new Map();
let nextId = 1;

function send(message) {
  if (window.chrome && window.chrome.webview) {
    window.chrome.webview.postMessage(message);
  } else if (window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.bridge) {
    window.webkit.messageHandlers.bridge.postMessage(message);
  } else {
    throw new Error('找不到設定程式的訊息通道');
  }
}

function receive(message) {
  if (!message || typeof message !== 'object') return;
  if (message.event) {
    for (const fn of listeners.get(message.event) || []) fn(message.data);
    return;
  }
  const entry = pending.get(message.id);
  if (!entry) return;
  pending.delete(message.id);
  if ('error' in message) entry.reject(new Error(message.error));
  else entry.resolve(message.result);
}

if (window.chrome && window.chrome.webview) {
  window.chrome.webview.addEventListener('message', (e) => receive(e.data));
}
window.__bridgeReceive = receive;

// 呼叫原生端的方法；失敗時 reject，訊息可以直接顯示給使用者
export function call(method, params = {}) {
  return new Promise((resolve, reject) => {
    const id = nextId++;
    pending.set(id, { resolve, reject });
    try {
      send({ id, method, params });
    } catch (e) {
      pending.delete(id);
      reject(e);
    }
  });
}

export function on(event, fn) {
  if (!listeners.has(event)) listeners.set(event, []);
  listeners.get(event).push(fn);
}
