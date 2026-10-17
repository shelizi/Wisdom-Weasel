#!/usr/bin/env bash
# Build-time preparation runs before Squirrel's resource copy and signing.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
python3 "$root/mac/prepare-squirrel.py"
if [ "$#" = 0 ]; then set -- release; fi
exec make -C "$root/squirrel" "$@"
