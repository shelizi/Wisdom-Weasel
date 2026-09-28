#!/usr/bin/env bash
# 下載 macOS 的 llama.cpp（與 Windows 的 get-llama-runtime.ps1 同一版本）到 output/llama-mac，
# 當作 mac/CMakeLists.txt 的 LLAMA_ROOT：
#   lib/      推理程式（WisdomLLMHost）需要的 dylib；安裝名稱是 @rpath，鼠鬚管打包時放進 Contents/Frameworks
#   include/  同一版本原始碼的 llama.h 與 ggml 標頭（發行檔不含標頭）
#
#   mac/get-llama-runtime.sh [--arch arm64|x64] [--version b11177] [--dest <資料夾>]
set -euo pipefail

arch=arm64
version=b11177
dest="$(cd "$(dirname "$0")/.." && pwd)/output/llama-mac"
while [ $# -gt 0 ]; do
  case "$1" in
    --arch) arch="$2"; shift 2 ;;
    --version) version="$2"; shift 2 ;;
    --dest) dest="$2"; shift 2 ;;
    *) echo "unknown option: $1" >&2; exit 1 ;;
  esac
done
case "$arch" in
  arm64|x64) ;;
  *) echo "--arch must be arm64 or x64" >&2; exit 1 ;;
esac

base="https://github.com/ggml-org/llama.cpp"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "Downloading llama-$version-bin-macos-$arch.tar.gz"
curl -fsSL -o "$tmp/bin.tar.gz" "$base/releases/download/$version/llama-$version-bin-macos-$arch.tar.gz"
echo "Downloading the $version headers"
curl -fsSL -o "$tmp/src.tar.gz" "$base/archive/refs/tags/$version.tar.gz"
mkdir -p "$tmp/bin" "$tmp/src"
tar -xzf "$tmp/bin.tar.gz" -C "$tmp/bin"
tar -xzf "$tmp/src.tar.gz" -C "$tmp/src" "llama.cpp-$version/include" "llama.cpp-$version/ggml/include"

rm -rf "$dest"
mkdir -p "$dest/lib" "$dest/include"
# 推理程式需要的 dylib（連同版本號的檔名與符號連結）；命令列工具與 mtmd、server 等不需要
for lib in llama ggml ggml-base ggml-cpu ggml-metal ggml-blas ggml-rpc; do
  found=0
  for f in "$tmp"/bin/*/lib"$lib".dylib "$tmp"/bin/*/lib"$lib".[0-9]*.dylib; do
    [ -e "$f" ] || continue
    cp -P "$f" "$dest/lib/"
    found=1
  done
  if [ "$found" = 0 ]; then
    echo "lib$lib.dylib is missing from the llama.cpp $version macOS $arch release" >&2
    exit 1
  fi
done
cp "$tmp/src/llama.cpp-$version/include/"*.h "$tmp/src/llama.cpp-$version/ggml/include/"*.h "$dest/include/"

echo "llama.cpp $version (macOS $arch): $(ls "$dest/lib" | wc -l | tr -d ' ') files in $dest/lib"
