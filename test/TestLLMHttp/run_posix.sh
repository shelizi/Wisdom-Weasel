#!/bin/sh
# macOS／Linux：啟動 mock_server.py、執行 TestLLMHttp，結束後關掉伺服器（CTest 用）
#   run_posix.sh <python3> <TestLLMHttp 執行檔> [port]
set -u
PYTHON="$1"
TEST="$2"
PORT="${3:-18765}"
DIR="$(cd "$(dirname "$0")" && pwd)"
"$PYTHON" "$DIR/mock_server.py" "$PORT" &
SERVER=$!
trap 'kill $SERVER 2>/dev/null' EXIT
# 等伺服器開始聽
for i in $(seq 1 50); do
  if "$PYTHON" -c "import socket,sys; s=socket.socket(); s.settimeout(0.2); sys.exit(s.connect_ex(('127.0.0.1', $PORT)))"; then
    break
  fi
  sleep 0.1
done
"$TEST" "$PORT"
