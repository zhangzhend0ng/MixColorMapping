#!/usr/bin/env bash
# Launch the color-mixer-batch web UI. Builds the CLI first if missing.
set -euo pipefail
cd "$(dirname "$0")/.."

if [ ! -f build/color_match_batch ] && [ ! -f build/color_match_batch.exe ]; then
    echo "[web] CLI not built — building now..."
    bash build.sh
fi

echo "[web] starting server..."
exec python3 web/app.py "$@"
