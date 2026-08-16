#!/bin/bash
# Captures the whole display (labwc/Wayland - there's no per-window capture
# without a compositor protocol most setups don't support, but the app runs
# fullscreen kiosk anyway so this is equivalent) to screenshots/latest.png,
# so it can be shared with Claude by file path.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT_DIR="$SCRIPT_DIR/../screenshots"
mkdir -p "$OUT_DIR"

grim "$OUT_DIR/latest.png"
echo "Saved $OUT_DIR/latest.png"
"$SCRIPT_DIR/album_update.sh" "$OUT_DIR/latest.png"
