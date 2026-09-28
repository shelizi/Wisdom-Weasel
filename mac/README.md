# macOS（鼠鬚管）版

小狼毫之外的輸入法功能（LLM 預測、整句校正、推薦、中英混打、注音逐字選字、個人詞庫、選字統計、
網頁版設定）都在 `core/`，Windows 與 macOS 共用。這個資料夾是 macOS 專用的部分，要接到鼠鬚管（Squirrel）使用。

> **狀態：核心、橋接層、設定視窗與推理程式都已在 macOS 上編譯，測試通過；還沒接進鼠鬚管實際使用。**
> `.github/workflows/macos-core.yml` 會在 macOS 上編譯並跑測試。

## 內容

| 位置 | 內容 |
|---|---|
| `core/platform/posix/process_posix.cpp` | 推理程式的子行程與管道（posix_spawn） |
| `core/platform/mac/key_store_mac.cpp` | 個人資料的金鑰存在登入鑰匙圈 |
| `core/net/curl/http_curl.cpp` | HTTP（libcurl，macOS 內建） |
| `mac/WisdomLLMHost/main.cpp` | LLM 推理程式（放在 `Squirrel.app/Contents/MacOS`） |
| `mac/squirrel/WisdomBridge.{h,mm}` | 給鼠鬚管呼叫的 Objective-C 介面（Swift 用 bridging header 匯入） |
| `mac/settings/` | 網頁版設定視窗（WKWebView）與設定後端的平台功能 |
| `mac/CMakeLists.txt` | 建置：`wisdom_core`、`wisdom_squirrel` 兩個靜態庫、推理程式與測試 |

## 建置

```sh
brew install cmake librime   # 或自己建的 librime：-DRIME_ROOT=<前綴>
mac/get-llama-runtime.sh      # llama.cpp（與 Windows 同版本）下載到 output/llama-mac；可省略
cmake -S mac -B build -DCMAKE_BUILD_TYPE=Release \
      -DLLAMA_ROOT=$PWD/output/llama-mac        # 可省略：省略時不建推理程式
cmake --build build
ctest --test-dir build --output-on-failure
```

測試：`TestIme`（規則、預測引擎、推薦、統計、加密紀錄）、`TestLLMHost`（推理行程的管道與重新啟動，對 `FakeHost`）、
`TestLLMHttp`（libcurl 對 `mock_server.py`）。
`TestController` 需要注音方案資料：加上 `-DRIME_SHARED=<含 bopomofo.schema.yaml、terra_pinyin.dict.yaml 的資料夾>`。

本機模型的端到端測試（需要 `LLAMA_ROOT`；啟動真的推理程式載入 GGUF 模型、預測並比較句子分數）：
`build/LocalModelSmoke <model.gguf> [n_gpu_layers]`。放進 `Squirrel.app/Contents/MacOS` 執行，就是測打包後的推理程式。

`LLAMA_ROOT` 也可以用 Homebrew 的 llama.cpp（`$(brew --prefix llama.cpp)`），但它只能在本機執行，不能打包。

## 建置鼠鬚管（含以上功能）

一次完成建置、安裝與本機 LLM 設定（還沒建過就建置；下載測試用的小模型到 `~/Library/Rime/models`；
把 `llm/*` 合併進 `squirrel.custom.yaml`，原檔先備份）：

```sh
mac/install-squirrel.sh              # --no-model：只安裝輸入法；--rebuild：重新建置
```

手動的步驟：

`squirrel/` 是鼠鬚管的修改版（submodule：[shelizi/squirrel](https://github.com/shelizi/squirrel) 的 `wisdom` 分支，
下面「接到鼠鬚管」的改動都在裡面；`git submodule update --init squirrel` 取得）。需要完整的 Xcode。

```sh
cd squirrel
git submodule update --init librime   # 要在 action-install.sh 之前：它會把預先建好的 librime 放進 librime/
bash action-install.sh                # 下載預先建好的 librime 與 Sparkle
../mac/get-llama-runtime.sh           # 可省略：有 output/llama-mac 時一起打包本機 LLM 推理程式
make release                          # build/Build/Products/Release/Squirrel.app
```

安裝（輸入法要放在 `/Library/Input Methods`）：

```sh
sudo rm -rf "/Library/Input Methods/Squirrel.app"
sudo cp -R build/Build/Products/Release/Squirrel.app "/Library/Input Methods/"
sudo DSTROOT="/Library/Input Methods" bash scripts/postinstall
```

## 接到鼠鬚管

鼠鬚管 1.0 之後是 Swift。在 bridging header 加上 `#import "WisdomBridge.h"`，連結 `wisdom_squirrel`、
`wisdom_core`、libcurl、Security、WebKit、UniformTypeIdentifiers，然後改這幾個地方：

**1. 啟動與部署**（`SquirrelApplicationDelegate`）

```swift
// rimeAPI.setup / initialize 之後
let bridge = WisdomBridge.shared
bridge.hideCandidates = { [weak self] in self?.panel?.hide() }
bridge.redeploy = { [weak self] in self?.deploy() }
bridge.start(withUserDirectory: userDataDir, sharedDirectory: Bundle.main.sharedSupportPath!)

// 部署完成後（維護執行緒結束後；網頁版設定部署時也在等它，不要同時 join_maintenance_thread）
WisdomBridge.shared.reloadConfig()

// rimeAPI.finalize 之前
WisdomBridge.shared.stop()
```

選單加一項「網頁版設定」呼叫 `WisdomBridge.shared.showSettings()`。

**2. session**（`SquirrelInputController`）

```swift
// createSession 之後
WisdomBridge.shared.addSession(session, client: client.bundleIdentifier() ?? "", refresh: { [weak self] in
  self?.rimeUpdate()   // 背景預測完成時，在主執行緒呼叫
})
// destroySession 之前
WisdomBridge.shared.removeSession(session)
```

整理選字記憶時橋接層會關掉所有 session（`cleanup_all_sessions`）：之後處理按鍵前若 `find_session` 為 false，
要重建 session 並再 `addSession`。

**3. 按鍵**：原本呼叫 `rimeAPI.process_key(session, keycode, modifiers)` 的地方改成

```swift
let result = WisdomBridge.shared.processKey(Int32(keycode), modifiers: Int32(modifiers), session: session)
if result.contains(.respond) { rimeUpdate() }
return result.contains(.handled)
```

keycode 與 modifiers 就是原本傳給 `process_key` 的值（Rime 的 keysym 與修飾鍵，放開按鍵時帶 release）。
中英混打靠 Shift 的按下與放開，放開的事件也要傳進來。

**4. 更新畫面**（`rimeUpdate`）

- 送出的文字：原本 `rimeAPI.get_commit` 的地方改成 `takeCommits(forSession:)`，依序插入每一段。
- 組字區：`preedit(forSession:zhuyinPreview:)` 不是 nil 就顯示它（含選取範圍與游標），否則照 Rime 的 preedit。
  每次更新都要呼叫（它也負責記錄選字統計）。
- 候選：Rime 的候選後面接上 `predictionCandidates()`（標籤 Tab、⇧2…，註解「推薦」「校正」）。
  有 LLM 候選時即使 Rime 沒在組字也要顯示候選窗。
- 點選候選：先呼叫 `selectCandidate(UInt(index), session:)`，回傳 true 就 `rimeUpdate()`，否則照原本交給 Rime。

**5. 其他**：失去焦點時 `focusOut(_:)`；應用程式要求送出／清除組字時改呼叫 `commitComposition(_:)`、`clearComposition(_:)`。

**6. 打包**（`squirrel/` 的 Makefile 與 Xcode 建置步驟會做）

- `web/settings` 複製到 `Squirrel.app/Contents/Resources/web/settings`
- `WisdomLLMHost` 放到 `Squirrel.app/Contents/MacOS`（與鼠鬚管同一資料夾，輸入法從那裡啟動它）；
  llama.cpp 的 dylib 放 `Contents/Frameworks`，推理程式的 rpath 是 `@executable_path/../Frameworks`
- 設定寫在 `squirrel.custom.yaml` 的 `llm/*`（網頁版設定會寫）

## 注意

- **金鑰**：存在登入鑰匙圈（服務 `Wisdom-Weasel`）。個人詞庫與設定頁都在鼠鬚管行程裡，不會跳出詢問。
- **代理伺服器**：libcurl 讀環境變數（`https_proxy` 等），不讀系統偏好設定的代理。
- **輸入方案安裝**：鼠鬚管沒有附東風破，設定頁的「取得更多輸入方案」會提示用終端機安裝。
- **本機 LLM**：llama.cpp 的 macOS 發行檔需要 macOS 13.3；13.0～13.2 上推理程式無法啟動（遠端 LLM 不受影響）。
- **記憶壓縮**（`MemoryCompressor`）在 Windows 由服務設定；鼠鬚管端還沒接上，前文只保留最近的內容。
