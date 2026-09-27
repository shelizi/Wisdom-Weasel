#include "stdafx.h"
#include "WebSettings.h"
#include "SettingsBackend.h"
#include "resource.h"
#include <WeaselUtility.h>
#include <WebView2.h>
#include <dwmapi.h>
#include <shlwapi.h>
#include <wrl.h>
#include <condition_variable>
#include <atomic>
#include <deque>
#include <filesystem>
#include <thread>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shlwapi.lib")

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

const wchar_t kHost[] = L"settings.wisdom-weasel";
const wchar_t kClassName[] = L"WisdomWebSettings";
constexpr UINT WM_APP_POST = WM_APP + 100;    // lParam：要送給網頁的 JSON（std::string*）
constexpr UINT WM_APP_RUN_UI = WM_APP + 101;  // lParam：要在視窗執行緒執行的工作（std::function*）
constexpr UINT_PTR kTimerScreenshot = 1;

std::wstring Widen(const std::string& s) {
  return u8tow(s);
}

// Rime 的呼叫都在這個執行緒上依序執行
class RimeWorker {
 public:
  RimeWorker() : thread_([this] { Loop(); }) {}
  ~RimeWorker() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    cv_.notify_one();
    thread_.join();
  }
  void Post(std::function<void()> job) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      queue_.push_back(std::move(job));
    }
    cv_.notify_one();
  }

 private:
  void Loop() {
    while (true) {
      std::function<void()> job;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
        if (queue_.empty())
          return;
        job = std::move(queue_.front());
        queue_.pop_front();
      }
      job();
    }
  }
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> queue_;
  bool stop_ = false;
  std::thread thread_;
};

bool DarkTheme(int pref) {
  return pref == 2 || (pref == 0 && IsUserDarkMode());
}

class WebSettingsWindow {
 public:
  WebSettingsWindow(Configurator* configurator, const WebSettingsOptions& options)
      : configurator_(configurator), options_(options) {}

  int Run() {
    wchar_t dir[MAX_PATH] = {0};
    if (GetEnvironmentVariableW(L"WISDOM_SETTINGS_WEB", dir, MAX_PATH)) {
      web_dir_ = dir;  // 開發時直接指向原始碼的 web\settings
    } else {
      GetModuleFileNameW(nullptr, dir, MAX_PATH);
      web_dir_ = (fs::path(dir).parent_path() / L"web" / L"settings").wstring();
    }
    std::error_code ec;
    if (!fs::exists(fs::path(web_dir_) / L"index.html", ec)) {
      MessageBoxW(nullptr, (L"找不到設定網頁：" + web_dir_).c_str(), L"小狼毫設定",
                  MB_OK | MB_ICONERROR);
      return -1;
    }
    try {
      // 背景工作可能比視窗晚結束：事件經由共用的視窗代碼送出，視窗關閉後就不送
      std::shared_ptr<std::atomic<HWND>> target = target_;
      backend_ = std::make_shared<SettingsBackend>(
          configurator_, [target](const std::string& event, const json& data) {
            PostToPage(target->load(), {{"event", event}, {"data", data}});
          });
    } catch (const std::exception& e) {
      MessageBoxW(nullptr, Widen(e.what()).c_str(), L"小狼毫設定", MB_OK | MB_ICONERROR);
      return 1;
    }
    DWORD pref = 0, size = sizeof(pref);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Rime\\Weasel", L"SettingsTheme", RRF_RT_REG_DWORD,
                 NULL, &pref, &size);
    theme_ = options_.theme ? options_.theme : (int)pref;

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(IDI_WEASELDEPLOYER));
    wc.hIconSm = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(IDI_SMALL));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(DarkTheme(theme_) ? RGB(32, 32, 32) : RGB(243, 243, 243));
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    // 依主螢幕 DPI 決定大小（之後跨螢幕由系統縮放）
    HDC screen = GetDC(nullptr);
    const int dpi = GetDeviceCaps(screen, LOGPIXELSX);
    ReleaseDC(nullptr, screen);
    const int width = MulDiv(options_.width ? options_.width : 1080, dpi, 96);
    const int height = MulDiv(options_.height ? options_.height : 760, dpi, 96);
    hwnd_ = CreateWindowExW(0, kClassName, L"小狼毫設定", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                            CW_USEDEFAULT, width, height, nullptr, nullptr, wc.hInstance, this);
    if (!hwnd_)
      return 1;
    *target_ = hwnd_;
    ApplyTitleBar(DarkTheme(theme_));
    if (options_.screenshot.empty()) {
      // 置中
      RECT rc, work;
      GetWindowRect(hwnd_, &rc);
      SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
      SetWindowPos(hwnd_, nullptr, work.left + (work.right - work.left - (rc.right - rc.left)) / 2,
                   work.top + (work.bottom - work.top - (rc.bottom - rc.top)) / 2, 0, 0,
                   SWP_NOSIZE | SWP_NOZORDER);
      ShowWindow(hwnd_, SW_SHOW);
    } else {
      // 截圖模式：放在畫面外，不打擾使用者
      SetWindowPos(hwnd_, nullptr, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
      ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    }
    if (!CreateWebView()) {
      DestroyWindow(hwnd_);
      return -1;
    }
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    // 還在進行中的背景工作握著 backend；取消下載，讓它們盡快結束
    backend_.reset();
    return result_;
  }

 private:
  static void PostToPage(HWND hwnd, const json& message) {
    auto* text = new std::string(message.dump());
    if (!hwnd || !PostMessageW(hwnd, WM_APP_POST, 0, (LPARAM)text))
      delete text;
  }

  void RunOnUi(std::function<void()> job) {
    auto* fn = new std::function<void()>(std::move(job));
    if (!PostMessageW(hwnd_, WM_APP_RUN_UI, 0, (LPARAM)fn))
      delete fn;
  }

  void ApplyTitleBar(bool dark) {
    BOOL value = dark;
    if (FAILED(DwmSetWindowAttribute(hwnd_, 20, &value, sizeof(value))))
      DwmSetWindowAttribute(hwnd_, 19, &value, sizeof(value));
  }

  bool CreateWebView() {
    wchar_t local[MAX_PATH] = {0};
    ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\Wisdom-Weasel\\WebView2", local, MAX_PATH);
    const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, local, nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
              if (FAILED(result) || !env) {
                FailWebView(result);
                return S_OK;
              }
              env_ = env;
              return env->CreateCoreWebView2Controller(
                  hwnd_, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                             [this](HRESULT result, ICoreWebView2Controller* controller) {
                               if (FAILED(result) || !controller) {
                                 FailWebView(result);
                                 return S_OK;
                               }
                               OnControllerCreated(controller);
                               return S_OK;
                             })
                             .Get());
            })
            .Get());
    if (FAILED(hr)) {
      FailWebView(hr);
      return false;
    }
    return true;
  }

  void FailWebView(HRESULT hr) {
    wchar_t text[256];
    swprintf_s(text,
               L"無法開啟網頁版設定（WebView2 錯誤 0x%08X）。\n\n"
               L"請安裝 Microsoft Edge WebView2 Runtime，或從托盤選單改用原本的「輸入法設定」。",
               (unsigned)hr);
    MessageBoxW(hwnd_, text, L"小狼毫設定", MB_OK | MB_ICONERROR);
    result_ = -1;
    allow_close_ = true;
    PostMessageW(hwnd_, WM_CLOSE, 0, 0);
  }

  void OnControllerCreated(ICoreWebView2Controller* controller) {
    controller_ = controller;
    controller_->get_CoreWebView2(&webview_);
    ComPtr<ICoreWebView2Controller2> controller2;
    if (SUCCEEDED(controller_.As(&controller2))) {
      const bool dark = DarkTheme(theme_);
      COREWEBVIEW2_COLOR bg = dark ? COREWEBVIEW2_COLOR{255, 32, 32, 32}
                                   : COREWEBVIEW2_COLOR{255, 243, 243, 243};
      controller2->put_DefaultBackgroundColor(bg);
    }
    ComPtr<ICoreWebView2Settings> settings;
    webview_->get_Settings(&settings);
    wchar_t devtools[8] = {0};
    const bool dev = GetEnvironmentVariableW(L"WISDOM_SETTINGS_DEVTOOLS", devtools, 8) > 0;
    settings->put_AreDevToolsEnabled(dev);
    settings->put_AreDefaultContextMenusEnabled(dev);
    settings->put_IsStatusBarEnabled(FALSE);
    settings->put_IsZoomControlEnabled(FALSE);
    ComPtr<ICoreWebView2Settings3> settings3;
    if (SUCCEEDED(settings.As(&settings3)))
      settings3->put_AreBrowserAcceleratorKeysEnabled(dev);

    ComPtr<ICoreWebView2_3> webview3;
    if (FAILED(webview_.As(&webview3))) {
      FailWebView(E_NOINTERFACE);
      return;
    }
    webview3->SetVirtualHostNameToFolderMapping(kHost, web_dir_.c_str(),
                                                COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);

    EventRegistrationToken token;
    webview_->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
              LPWSTR text = nullptr;
              if (SUCCEEDED(args->get_WebMessageAsJson(&text)) && text) {
                OnPageMessage(wtou8(text));
                CoTaskMemFree(text);
              }
              return S_OK;
            })
            .Get(),
        &token);
    // 設定網頁以外的網址（說明連結）交給預設瀏覽器
    webview_->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
              LPWSTR uri = nullptr;
              args->get_Uri(&uri);
              const std::wstring url = uri ? uri : L"";
              CoTaskMemFree(uri);
              if (url.rfind(std::wstring(L"https://") + kHost + L"/", 0) != 0) {
                args->put_Cancel(TRUE);
                if (url.rfind(L"https://", 0) == 0 || url.rfind(L"http://", 0) == 0)
                  ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
              }
              return S_OK;
            })
            .Get(),
        &token);
    webview_->add_NewWindowRequested(
        Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
              LPWSTR uri = nullptr;
              args->get_Uri(&uri);
              if (uri && (wcsncmp(uri, L"https://", 8) == 0 || wcsncmp(uri, L"http://", 7) == 0))
                ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL);
              CoTaskMemFree(uri);
              args->put_Handled(TRUE);
              return S_OK;
            })
            .Get(),
        &token);

    // 截圖模式：網頁出錯沒有回報就緒時，最多等幾秒也照樣截圖
    if (!options_.screenshot.empty()) {
      webview_->add_NavigationCompleted(
          Callback<ICoreWebView2NavigationCompletedEventHandler>(
              [this](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                SetTimer(hwnd_, kTimerScreenshot, 4000, nullptr);
                return S_OK;
              })
              .Get(),
          &token);
    }

    Resize();
    std::wstring url = std::wstring(L"https://") + kHost + L"/index.html?page=" +
                       std::to_wstring(options_.start_page);
    if (options_.theme)
      url += L"&theme=" + std::to_wstring(options_.theme);
    if (!options_.screenshot.empty())
      url += L"&screenshot=1";
    webview_->Navigate(url.c_str());
  }

  void Reply(const json& id, const json& result) {
    PostToPage(hwnd_, {{"id", id}, {"result", result}});
  }

  void OnPageMessage(const std::string& text) {
    const json message = json::parse(text, nullptr, false);
    if (!message.is_object() || !message.contains("method"))
      return;
    const json id = message.value("id", json());
    const std::string method = message.value("method", std::string());
    const json params = message.value("params", json::object());

    // 視窗自己處理的
    if (method == "app.ready") {
      Reply(id, nullptr);
      if (!options_.screenshot.empty())
        SetTimer(hwnd_, kTimerScreenshot, 900, nullptr);
      return;
    }
    if (method == "app.close") {
      Reply(id, nullptr);
      allow_close_ = true;
      PostMessageW(hwnd_, WM_CLOSE, 0, 0);
      return;
    }
    if (method == "app.titleBar") {
      ApplyTitleBar(params.value("dark", false));
      Reply(id, nullptr);
      return;
    }

    HWND hwnd = hwnd_;
    std::shared_ptr<SettingsBackend> backend = backend_;
    auto run = [hwnd, backend, id, method, params]() {
      json reply = {{"id", id}};
      try {
        reply["result"] = backend->Call(method, params, hwnd);
      } catch (const std::exception& e) {
        reply["error"] = e.what();
      }
      PostToPage(hwnd, reply);
    };
    switch (SettingsBackend::ThreadOf(method)) {
      case SettingsBackend::Thread::kUi:
        RunOnUi(run);  // 不在 WebView2 的事件裡開對話框
        break;
      case SettingsBackend::Thread::kRime:
        worker_.Post(run);
        break;
      case SettingsBackend::Thread::kTask:
        std::thread(run).detach();
        break;
    }
  }

  void Resize() {
    if (!controller_)
      return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    controller_->put_Bounds(rc);
  }

  void Screenshot() {
    KillTimer(hwnd_, kTimerScreenshot);
    ComPtr<IStream> stream;
    if (FAILED(SHCreateStreamOnFileEx(options_.screenshot.c_str(),
                                      STGM_READWRITE | STGM_CREATE | STGM_SHARE_EXCLUSIVE,
                                      FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, &stream))) {
      allow_close_ = true;
      PostMessageW(hwnd_, WM_CLOSE, 0, 0);
      return;
    }
    webview_->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, stream.Get(),
                             Callback<ICoreWebView2CapturePreviewCompletedHandler>(
                                 [this, stream](HRESULT) -> HRESULT {
                                   stream->Commit(STGC_DEFAULT);
                                   allow_close_ = true;
                                   PostMessageW(hwnd_, WM_CLOSE, 0, 0);
                                   return S_OK;
                                 })
                                 .Get());
  }

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    WebSettingsWindow* self;
    if (msg == WM_NCCREATE) {
      self = (WebSettingsWindow*)((CREATESTRUCTW*)lp)->lpCreateParams;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
      self->hwnd_ = hwnd;
    } else {
      self = (WebSettingsWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    }
    return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
  }

  LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
      case WM_SIZE:
        Resize();
        return 0;
      case WM_DPICHANGED: {
        const RECT* rc = (const RECT*)lp;
        SetWindowPos(hwnd_, nullptr, rc->left, rc->top, rc->right - rc->left,
                     rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
      }
      case WM_APP_POST: {
        std::unique_ptr<std::string> text((std::string*)lp);
        if (webview_)
          webview_->PostWebMessageAsJson(Widen(*text).c_str());
        return 0;
      }
      case WM_APP_RUN_UI: {
        std::unique_ptr<std::function<void()>> fn((std::function<void()>*)lp);
        (*fn)();
        return 0;
      }
      case WM_TIMER:
        if (wp == kTimerScreenshot)
          Screenshot();
        return 0;
      case WM_CLOSE:
        // 先問網頁（有沒有尚未套用的變更）；網頁同意後會呼叫 app.close
        if (!allow_close_ && webview_) {
          PostToPage(hwnd_, {{"event", "closeRequested"}, {"data", nullptr}});
          return 0;
        }
        DestroyWindow(hwnd_);
        return 0;
      case WM_DESTROY:
        *target_ = nullptr;
        if (controller_)
          controller_->Close();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
  }

  Configurator* configurator_;
  WebSettingsOptions options_;
  HWND hwnd_ = nullptr;
  std::shared_ptr<std::atomic<HWND>> target_ = std::make_shared<std::atomic<HWND>>(nullptr);
  std::wstring web_dir_;
  int theme_ = 0;
  int result_ = 0;
  bool allow_close_ = false;
  std::shared_ptr<SettingsBackend> backend_;
  RimeWorker worker_;
  ComPtr<ICoreWebView2Environment> env_;
  ComPtr<ICoreWebView2Controller> controller_;
  ComPtr<ICoreWebView2> webview_;
};

}  // namespace

int RunWebSettings(Configurator* configurator, const WebSettingsOptions& options) {
  WebSettingsWindow window(configurator, options);
  return window.Run();
}
