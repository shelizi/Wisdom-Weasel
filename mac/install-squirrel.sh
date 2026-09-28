#!/usr/bin/env bash
# 一次完成鼠鬚管（含 Wisdom-Weasel 功能）的建置、安裝與本機 LLM 設定：
#   1. 還沒建過 Squirrel.app 就建置（librime、Sparkle、llama.cpp 用預先建好的發行檔）
#   2. 本機 LLM 模型放到 ~/Library/Rime/models（沒有就下載並核對 SHA-256）
#   3. 安裝到 /Library/Input Methods（這一步要 sudo），註冊並啟用輸入法
#   4. 把 llm/* 設定合併進 ~/Library/Rime/squirrel.custom.yaml（改之前先備份），重新部署
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

model_name="qwen2.5-0.5b-instruct-q4_k_m.gguf"
model_url="https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF/resolve/main/$model_name"
model_sha256="74a4da8c9fdbcd15bd1f6d01d621410d31c6fc00986f5eb687824e7b93d7a9db"
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
install_cmd="$(printf 'rm -rf %q && cp -R %q %q && DSTROOT=%q /bin/bash %q' \
  "$install_dir/Squirrel.app" "$app" "$install_dir/" "$install_dir" "$squirrel/scripts/postinstall")"
if [ -t 0 ] || sudo -n true 2> /dev/null; then
  sudo /bin/bash -c "$install_cmd"
else
  # 沒有終端機可以輸入密碼（例如由其他程式執行）：用系統的密碼視窗
  echo "沒有終端機可以輸入密碼，改用系統的密碼視窗"
  applescript="${install_cmd//\\/\\\\}"
  applescript="${applescript//\"/\\\"}"
  osascript -e "do shell script \"$applescript\" with administrator privileges" > /dev/null
fi

# ---- 4. 設定
if [ "$with_model" = 1 ]; then
  step "LLM 設定：$rime_dir/squirrel.custom.yaml"
  mkdir -p "$rime_dir"
  custom="$rime_dir/squirrel.custom.yaml"
  if [ -f "$custom" ]; then
    backup="$custom.bak.$(date +%Y%m%d%H%M%S)"
    cp "$custom" "$backup"
    echo "原本的設定備份在 $backup"
  fi
  # 設定放在 patch: 底下、以標記框起來的一段；再執行時只替換這一段。
  # 其他地方已經有的同名設定會拿掉（同一個鍵出現兩次時 YAML 無效）
  MODEL_PATH="$model_path" /usr/bin/python3 - "$custom" <<'PYEOF'
import os, re, sys
path = sys.argv[1]
begin, end = "# >>> Wisdom-Weasel 本機 LLM（mac/install-squirrel.sh 產生）", "# <<< Wisdom-Weasel 本機 LLM"
settings = [
    ("llm/enabled", "true"),
    ("llm/provider_type", "llamacpp"),
    ("llm/llamacpp/model_path", '"%s"' % os.environ["MODEL_PATH"]),
    ("llm/llamacpp/model_type", "Instruct"),
    ("llm/llamacpp/n_ctx", "2048"),
    ("llm/llamacpp/n_gpu_layers", "-1"),
    ("llm/llamacpp/max_tokens", "8"),
]
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
keys = {k for k, _ in settings}
key_re = re.compile(r'^\s+["\']?([^"\':]+)["\']?\s*:')
lines = [l for l in lines if not (key_re.match(l) and key_re.match(l).group(1).strip() in keys)]
patch_at = next((i for i, l in enumerate(lines) if re.match(r"^patch:\s*(#.*)?$", l)), None)
if patch_at is None:
    if any(re.match(r"^patch:", l) for l in lines):
        sys.exit("squirrel.custom.yaml 的 patch: 不是一般的區塊寫法，請手動加入 llm/* 設定")
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
  echo "已寫入 llm/*（模型：${model_path}）"
fi

step "重新部署"
"$install_dir/Squirrel.app/Contents/MacOS/Squirrel" --reload || true

cat <<EOF

完成。
- 選單列沒有鼠鬚管時：系統設定 → 鍵盤 → 輸入方式，加入「鼠鬚管」；第一次可能要登出再登入。
- 鼠鬚管選單的「網頁版設定…」可以改 LLM 設定；「部署」（⌃⌥\`）讓手動修改的設定生效。
- 紀錄：\$TMPDIR/rime.squirrel/
EOF
