#!/bin/bash
# Records the Pi's screen and current audio output to a timestamped MP4,
# using wf-recorder (install with: sudo apt install wf-recorder).
#
# Usage: scripts/record_demo.sh [seconds]
#   No argument: records for 300 seconds (5 minutes).
#
# Stop early any time with Ctrl+C. Important: that's the only safe way to
# stop it - wf-recorder needs a clean SIGINT to finalize the MP4 (write its
# index/moov atom); killing it abruptly (SIGKILL, closing the terminal)
# leaves a corrupted, unplayable file. This script uses `timeout --signal
# SIGINT` for the same reason, rather than the default SIGTERM.
set -e

DURATION="${1:-300}"
OUT_DIR="$HOME/DATVreceiver-recordings"
mkdir -p "$OUT_DIR"
OUT_FILE="$OUT_DIR/demo_$(date +%Y-%m-%d_%H-%M-%S).mp4"

# Whatever the current default audio output is (Bluetooth, HDMI, etc.) -
# resolved fresh each run rather than a hardcoded device name, since that
# can change if you switch outputs.
DEFAULT_SINK="$(pactl get-default-sink 2>/dev/null)"
MONITOR_SOURCE="${DEFAULT_SINK}.monitor"
AUDIO_ARGS=()
if [ -n "$DEFAULT_SINK" ] && pactl list short sources 2>/dev/null | grep -q "$MONITOR_SOURCE"; then
    AUDIO_ARGS=(-a "$MONITOR_SOURCE")
else
    echo "Warning: no audio monitor source found - recording video only." >&2
fi

echo "Recording to $OUT_FILE for ${DURATION}s (Ctrl+C to stop early)..."
DISPLAY="${DISPLAY:-:0.0}" timeout --signal=SIGINT "$DURATION" \
    wf-recorder "${AUDIO_ARGS[@]}" -f "$OUT_FILE"

echo "Saved: $OUT_FILE"
