#!/bin/bash
# Captures just the running LVGL app window (not the whole desktop) to
# screenshots/latest.png, so it can be shared with Claude by file path.
set -e

DISPLAY="${DISPLAY:-:0.0}"
export DISPLAY

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT_DIR="$SCRIPT_DIR/../screenshots"
mkdir -p "$OUT_DIR"

WIDS=($(xdotool search --name "LVGL Simulator"))

if [ "${#WIDS[@]}" -eq 0 ]; then
    echo "No 'LVGL Simulator' window found - is qo100datv/spectrum_test/qo100_monitor running?"
    exit 1
fi

if [ "${#WIDS[@]}" -gt 1 ]; then
    echo "Warning: ${#WIDS[@]} LVGL windows found, capturing the last one (${WIDS[-1]})."
fi

WID="${WIDS[-1]}"
import -window "$WID" "$OUT_DIR/latest.png"
echo "Saved $OUT_DIR/latest.png"
