#!/usr/bin/env bash
# 一次完成鼠鬚管（含 Wisdom-Weasel 功能）的建置、安裝與本機 LLM 設定：
#   1. 還沒建過 Squirrel.app 就建置（librime、Sparkle、llama.cpp 用預先建好的發行檔）
#   2. 本機 LLM 模型（Gemma 4 E2B）放到 ~/Library/Rime/models（沒有就下載並核對 SHA-256）
#   3. 安裝到 /Library/Input Methods（這一步要 sudo），註冊並啟用輸入法
#   4. 把 llm/* 設定合併進 ~/Library/Rime/squirrel.custom.yaml（改之前先備份）；
#      還沒有 default.custom.yaml 時預設輸入方案為注音·快打模式；重新部署
#
#   mac/install-squirrel.sh [--rebuild] [--no-model]
#     --rebuild   已經建過也重新建置
#     --no-model  不下載模型、不改 LLM 設定（只安裝輸入法）
# 以自己的帳號執行，不要整個用 sudo：使用者設定要寫在自己的 ~/Library/Rime。
set -euo pipefail

rebuild=0
with_model=1
while [ $# -gt 0 ]; do
  case "$1" in
    --rebuild) rebuild=1; shift ;;
    --no-model) with_model=0; shift ;;
    *) echo "unknown option: $1" >&2; exit 1 ;;
  esac
done
if [ "$(id -u)" = 0 ]; then
  echo "請用自己的帳號執行（需要時腳本會自己用 sudo）" >&2
  exit 1
fi

root="$(cd "$(dirname "$0")/.." && pwd)"
squirrel="$root/squirrel"
app="$squirrel/build/Build/Products/Release/Squirrel.app"
install_dir="/Library/Input Methods"
rime_dir="${RIME_USER_DIR:-$HOME/Library/Rime}"

# Gemma 4 E2B 的官方 QAT 4-bit（3.3 GB）：M1 8GB 以 Metal 執行，預測約 0.45 秒；整個模型常駐記憶體（約 3.3 GB），
# 同時執行兩份會超過 GPU 可用的記憶體
model_name="gemma-4-E2B_q4_0-it.gguf"
model_url="https://huggingface.co/google/gemma-4-E2B-it-qat-q4_0-gguf/resolve/main/$model_name"
model_sha256="fa401b55b07ee70a54c6dae3903c783a6e65064312529ea57175cb5f8dec6634"
model_path="$rime_dir/models/$model_name"

step() { printf '\n==> %s\n' "$*"; }

# ---- 1. 建置
if [ ! -d "$squirrel/.git" ]; then
  echo "找不到 ${squirrel}（鼠鬚管的修改版）：先取得 squirrel/ 再執行" >&2
  exit 1
fi
if [ "$rebuild" = 1 ] || [ ! -x "$app/Contents/MacOS/Squirrel" ]; then
  step "建置 Squirrel.app"
  if ! xcodebuild -version > /dev/null 2>&1; then
    echo "需要完整的 Xcode（並執行過 sudo xcodebuild -license accept）" >&2
    exit 1
  fi
  command -v cmake > /dev/null || { echo "需要 cmake（brew install cmake）" >&2; exit 1; }
  (
    cd "$squirrel"
    # librime 的原始碼（標頭）要在 action-install.sh 之前取得：它會把預先建好的 librime 放進 librime/
    if [ ! -f librime/src/rime_api.h ]; then
      if [ -d librime/dist ]; then mv librime/dist .librime-dist; fi
      if [ -d librime/share/opencc ]; then mv librime/share/opencc .librime-opencc; fi
      rm -rf librime .git/modules/librime
      git submodule update --init --depth 1 librime
      if [ -d .librime-dist ]; then mv .librime-dist librime/dist; fi
      if [ -d .librime-opencc ]; then mkdir -p librime/share && mv .librime-opencc librime/share/opencc; fi
    fi
    if [ ! -f librime/dist/lib/librime.1.dylib ] || [ ! -d Frameworks/Sparkle.framework ]; then
      bash ./action-install.sh
    fi
  )
  if [ ! -d "$root/output/llama-mac/lib" ]; then
    "$root/mac/get-llama-runtime.sh"
  fi
  make -C "$squirrel" release
fi

# ---- 2. 模型
if [ "$with_model" = 1 ]; then
  step "本機 LLM 模型：$model_path"
  mkdir -p "$(dirname "$model_path")"
  check() { [ -f "$1" ] && [ "$(shasum -a 256 "$1" | cut -d' ' -f1)" = "$model_sha256" ]; }
  if check "$model_path"; then
    echo "已經有了"
  elif check "$root/build-mac/models/$model_name"; then
    cp -c "$root/build-mac/models/$model_name" "$model_path" 2> /dev/null ||
      cp "$root/build-mac/models/$model_name" "$model_path"
    echo "從 build-mac/models 複製"
  else
    curl -fL --progress-bar -o "$model_path.part" "$model_url"
    check "$model_path.part" || { echo "模型的 SHA-256 不符" >&2; rm -f "$model_path.part"; exit 1; }
    mv "$model_path.part" "$model_path"
  fi
fi

# ---- 3. 安裝
step "安裝到 ${install_dir}（需要管理者密碼）"
# postinstall 會把預先部署的方案寫進 app 的 SharedSupport，本機建置的 ad-hoc 簽章因此失效，
# 系統（imklaunchagent）就啟動不了輸入法：部署完重新簽一次
install_cmd="$(printf 'rm -rf %q && cp -R %q %q && DSTROOT=%q /bin/bash %q && /usr/bin/codesign --force --deep --sign - %q' \
  "$install_dir/Squirrel.app" "$app" "$install_dir/" "$install_dir" "$squirrel/scripts/postinstall" \
  "$install_dir/Squirrel.app")"
if [ -t 0 ] || sudo -n true 2> /dev/null; then
  sudo /bin/bash -c "$install_cmd"
else
  # 沒有終端機可以輸入密碼（例如由其他程式執行）：用系統的密碼視窗
  echo "沒有終端機可以輸入密碼，改用系統的密碼視窗"
  applescript="${install_cmd//\\/\\\\}"
  applescript="${applescript//\"/\\\"}"
  osascript -e "do shell script \"$applescript\" with administrator privileges" > /dev/null
fi
codesign --verify --deep --strict "$install_dir/Squirrel.app" ||
  { echo "安裝後的簽章無效，系統會啟動不了輸入法" >&2; exit 1; }
# 建置資料夾裡的 Squirrel.app 也登記了同一個 bundle id：拿掉，系統才會啟動安裝的這一份。
# imklaunchagent 會記住上次啟動失敗的結果，重新啟動它（launchd 會自動再開）
lsregister=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
"$lsregister" -u "$app" 2> /dev/null || true
"$lsregister" -f -R "$install_dir/Squirrel.app" 2> /dev/null || true
killall imklaunchagent 2> /dev/null || true

# ---- 4. 設定
# 把設定合併進 <檔案> 的 patch: 底下，以標記框起來；再執行時只替換這一段。
# 其他地方已經有的同名設定（連同底下的子項目）會拿掉：同一個鍵出現兩次時 YAML 無效。
# 改之前先備份。回傳 3：<檔案> 已經有網頁版設定寫的整個 <map> 區塊（會蓋過單一設定），沒有修改
#   merge_patch <檔案> <標記名稱> <map 或空字串> <JSON：[[鍵, YAML 值], …]>
merge_patch() {
  mkdir -p "$(dirname "$1")"
  if [ -f "$1" ]; then
    cp "$1" "$1.bak.$(date +%Y%m%d%H%M%S)"
  fi
  /usr/bin/python3 - "$@" <<'PYEOF'
import json, os, re, sys
path, label, map_key, settings = sys.argv[1], sys.argv[2], sys.argv[3], json.loads(sys.argv[4])
begin, end = "# >>> Wisdom-Weasel %s（mac/install-squirrel.sh 產生）" % label, "# <<< Wisdom-Weasel %s" % label
lines = open(path, encoding="utf-8").read().splitlines() if os.path.exists(path) else []
# 拿掉上次產生的一段
out, skipping = [], False
for line in lines:
    if line.strip() == begin:
        skipping = True
        continue
    if skipping:
        if line.strip() == end:
            skipping = False
        continue
    out.append(line)
lines = out
if map_key and any(re.match(r"^\s+%s:\s*(#.*)?$" % re.escape(map_key), l) for l in lines):
    open(path, "w", encoding="utf-8").write("\n".join(lines) + "\n")
    sys.exit(3)
keys = {k for k, _ in settings}
key_re = re.compile(r'^(\s+)["\']?([^"\':]+)["\']?\s*:')
indent_of = lambda l: len(l) - len(l.lstrip())
out, drop_deeper = [], None
for l in lines:
    if drop_deeper is not None:
        if not l.strip() or indent_of(l) > drop_deeper:
            continue
        drop_deeper = None
    m = key_re.match(l)
    if m and m.group(2).strip() in keys:
        drop_deeper = len(m.group(1))
        continue
    out.append(l)
lines = out
patch_at = next((i for i, l in enumerate(lines) if re.match(r"^patch:\s*(#.*)?$", l)), None)
if patch_at is None:
    if any(re.match(r"^patch:", l) for l in lines):
        sys.exit("%s 的 patch: 不是一般的區塊寫法，請手動加入設定" % path)
    lines += ([""] if lines and lines[-1].strip() else []) + ["patch:"]
    patch_at = len(lines) - 1
# 沿用 patch: 底下原本的縮排
indent = "  "
for l in lines[patch_at + 1:]:
    if l.strip() and not l.lstrip().startswith("#"):
        m = re.match(r"^(\s+)", l)
        if m:
            indent = m.group(1)
        break
block = [indent + begin] + ['%s"%s": %s' % (indent, k, v) for k, v in settings] + [indent + end]
lines[patch_at + 1:patch_at + 1] = block
open(path, "w", encoding="utf-8").write("\n".join(lines) + "\n")
PYEOF
}

if [ "$with_model" = 1 ]; then
  step "LLM 設定：$rime_dir/squirrel.custom.yaml"
  # Gemma 常輸出簡體字。提示詞由預測與整句校正共用，只放兩者都適用的要求。
  # 快打模式以 Tab、Shift+數字選字：打字中不顯示 LLM 補全，才不會搶走選字鍵（送出後的預測照常）
  llm_settings="$(MODEL_PATH="$model_path" /usr/bin/python3 -c 'import json, os; print(json.dumps([
    ["llm/enabled", "true"],
    ["llm/provider_type", "llamacpp"],
    ["llm/llamacpp/model_path", json.dumps(os.environ["MODEL_PATH"])],
    ["llm/llamacpp/model_type", "Instruct"],
    ["llm/prompt", json.dumps("一律使用繁體中文（臺灣用字）輸出，不要使用簡體字。", ensure_ascii=False)],
    ["llm/llamacpp/n_ctx", "2048"],
    ["llm/llamacpp/n_gpu_layers", "-1"],
    ["llm/llamacpp/max_tokens", "8"],
    ["llm/predict_while_typing", "false"],
  ], ensure_ascii=False))')"
  merge_status=0
  merge_patch "$rime_dir/squirrel.custom.yaml" "本機 LLM" llm "$llm_settings" || merge_status=$?
  if [ "$merge_status" = 3 ]; then
    echo "LLM 設定由網頁版設定管理（squirrel.custom.yaml 的 llm: 區塊），沒有修改：模型在網頁版設定選 ${model_path}"
  elif [ "$merge_status" != 0 ]; then
    exit "$merge_status"
  else
    echo "已寫入 llm/*（模型：${model_path}）"
  fi
fi

# 輸入方案：還沒自訂過（沒有 default.custom.yaml）時預設為注音·快打模式
if [ ! -f "$rime_dir/default.custom.yaml" ]; then
  step "輸入方案：注音·快打模式（$rime_dir/default.custom.yaml）"
  mkdir -p "$rime_dir"
  cat > "$rime_dir/default.custom.yaml" <<'YAMLEOF'
# mac/install-squirrel.sh 產生：預設的輸入方案（可在網頁版設定或這裡修改）
patch:
  schema_list:
    - schema: bopomofo_express
YAMLEOF
fi

# macOS 用 Caps Lock 切換輸入法時，這一下也會送到鼠鬚管，把 Rime 切成西文：
# Caps Lock 不切換中英（中英用 Shift）
step "Caps Lock 不切換中英：$rime_dir/default.custom.yaml"
merge_patch "$rime_dir/default.custom.yaml" "Caps Lock" "" '[["ascii_composer/switch_key/Caps_Lock", "noop"]]'

# 注音·快打模式的說明寫「支持亂序輸入」，但它的拼寫規則漏了 zhuyin:/free_order：照一般注音的順序補上
step "快打模式的亂序輸入：$rime_dir/bopomofo_express.custom.yaml"
merge_patch "$rime_dir/bopomofo_express.custom.yaml" "亂序輸入" "" \
  '[["speller/algebra", "{__patch: [\"zhuyin:/pinyin_to_zhuyin\", \"zhuyin:/free_order\", \"zhuyin:/abbreviation\", \"zhuyin:/keymap_bopomofo\"]}"]]'

step "重新部署"
"$install_dir/Squirrel.app/Contents/MacOS/Squirrel" --reload || true

cat <<EOF

完成。
- 重新安裝前就開著的 App（包括執行這個腳本的終端機）還連著舊的鼠鬚管，會打不出字：⌘Q 結束後重新開啟。
- 選單列沒有鼠鬚管時：系統設定 → 鍵盤 → 輸入方式，加入「鼠鬚管」；第一次可能要登出再登入。
- 鼠鬚管選單的「網頁版設定…」可以改 LLM 設定；「部署」（⌃⌥\`）讓手動修改的設定生效。
- 紀錄：\$TMPDIR/rime.squirrel/
EOF
