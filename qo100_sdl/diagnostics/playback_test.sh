#!/usr/bin/env bash
# Playback diagnostic: does this Pi decode and present video smoothly?
#
# Generates short synthetic DATV-like streams, sends them over loopback into the
# app's real decoder and presenter (no screen, no tuner, no network), and reports
# the frame rate actually shown against the stream's own. See README.md.
#
#   ./playback_test.sh              all scenarios (about 2 minutes)
#   ./playback_test.sh --quick      three scenarios
#   ./playback_test.sh --only NAME  one scenario (see the list below)
#   ./playback_test.sh --list
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../src"
WORK="${QO100_DIAG_DIR:-${TMPDIR:-/tmp}/qo100-diagnostics}"
PORT="${QO100_DIAG_PORT:-5610}"
SECONDS_PER_RUN=16

# name | ffmpeg encoder | size | fps | hardware HEVC decode (1/0)
SCENARIOS=(
  "h264_720p50|libx264|1280x720|50|1"
  "h264_1080p25|libx264|1920x1080|25|1"
  "h264_576p30|libx264|1024x576|30|1"
  "hevc_720p30_hw|libx265|1280x720|30|1"
  "hevc_720p30_sw|libx265|1280x720|30|0"
  "hevc_540p25_hw|libx265|960x540|25|1"
)
QUICK=("h264_720p50" "h264_1080p25" "hevc_720p30_hw")

only=""; quick=0
while [ $# -gt 0 ]; do
  case "$1" in
    --quick) quick=1 ;;
    --only) only="${2:-}"; shift ;;
    --list) for s in "${SCENARIOS[@]}"; do echo "${s%%|*}"; done; exit 0 ;;
    -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done

need() { command -v "$1" >/dev/null 2>&1 || { echo "missing: $1 ($2)" >&2; exit 2; }; }
need ffmpeg "sudo apt install ffmpeg"
need g++ "sudo apt install build-essential"
need pkg-config "sudo apt install pkg-config"
mkdir -p "$WORK"

echo "== System"
echo "date:     $(date '+%Y-%m-%d %H:%M:%S')"
echo "model:    $(tr -d '\0' < /proc/device-tree/model 2>/dev/null || uname -m)"
echo "kernel:   $(uname -r)"
echo "load:     $(cut -d' ' -f1-3 /proc/loadavg)   temp: $(awk '{printf "%.1fC", $1/1000}' /sys/class/thermal/thermal_zone0/temp 2>/dev/null)"
echo "ffmpeg:   $(ffmpeg -version | head -1 | cut -d' ' -f1-3)"
echo "app:      $(git -C "$HERE" rev-parse --short HEAD 2>/dev/null || echo unknown)"
if pgrep -x qo100sdl >/dev/null; then
  echo "NOTE: qo100sdl is running - it uses CPU too; for a clean result stop it first."
fi
if [ -e /dev/video19 ] || ls /dev/video* >/dev/null 2>&1; then
  echo "V4L2:     video devices present (hardware HEVC decode can be used)"
else
  echo "V4L2:     no /dev/video* (HEVC will fall back to software)"
fi
echo

BIN="$WORK/playback_harness"
if [ ! -x "$BIN" ] || [ "$HERE/playback_harness.cpp" -nt "$BIN" ] || \
   [ -n "$(find "$SRC" -name '*.h' -newer "$BIN" -o -name 'video_decoder.cpp' -newer "$BIN" 2>/dev/null)" ]; then
  echo "== Building the harness"
  g++ -std=c++17 -O2 -I"$SRC" "$HERE/playback_harness.cpp" "$SRC/video_decoder.cpp" -o "$BIN" \
      $(pkg-config --cflags --libs libavcodec libavformat libavutil libswscale libswresample) \
      -lpthread || { echo "build failed" >&2; exit 2; }
  echo
fi

make_stream() {  # name encoder size fps
  local file="$WORK/$1.20s.ts" enc="$2" size="$3" fps="$4" opts
  [ -s "$file" ] && return 0
  case "$enc" in
    libx265) opts="-preset ultrafast -x265-params log-level=error:keyint=$((fps*2))" ;;
    *)       opts="-preset veryfast -g $((fps*2))" ;;
  esac
  ffmpeg -hide_banner -loglevel error -y \
    -f lavfi -i "testsrc2=size=$size:rate=$fps,noise=alls=15:allf=t" \
    -f lavfi -i "sine=frequency=440:sample_rate=48000" -t 20 \
    -c:v "$enc" $opts -b:v 1500k -c:a mp2 -b:a 128k -shortest -f mpegts "$file" 2>/dev/null
  [ -s "$file" ]
}

results=()
sender=""
cleanup() { [ -n "$sender" ] && kill "$sender" 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT

echo "== Running (${SECONDS_PER_RUN}s per scenario)"
for spec in "${SCENARIOS[@]}"; do
  IFS='|' read -r name enc size fps hw <<<"$spec"
  if [ -n "$only" ] && [ "$only" != "$name" ]; then continue; fi
  if [ -z "$only" ] && [ "$quick" = 1 ]; then
    match=0; for q in "${QUICK[@]}"; do [ "$q" = "$name" ] && match=1; done
    [ "$match" = 1 ] || continue
  fi
  if ! ffmpeg -hide_banner -encoders 2>/dev/null | grep -q " $enc "; then
    echo "$name: skipped (ffmpeg has no $enc encoder)"; continue
  fi
  make_stream "$name" "$enc" "$size" "$fps" || { echo "$name: could not generate the stream"; continue; }

  ffmpeg -hide_banner -loglevel error -re -i "$WORK/$name.20s.ts" -c copy \
    -f mpegts "udp://127.0.0.1:$PORT?pkt_size=1316" &
  sender=$!
  sleep 0.5
  out="$(QO100_TS_ADDR=127.0.0.1 QO100_TS_PORT="$PORT" QO100_HW_DECODE="$hw" \
         "$BIN" "$SECONDS_PER_RUN" "$name" "$fps" 2>&1)"
  kill "$sender" 2>/dev/null; wait "$sender" 2>/dev/null; sender=""
  line="$(printf '%s\n' "$out" | grep '^RESULT ')"
  per_second="$(printf '%s\n' "$out" | grep '^PER_SECOND ')"
  if [ -z "$line" ]; then echo "$name: no result"; printf '%s\n' "$out" | tail -5; continue; fi
  hwmsg="$(printf '%s\n' "$out" | grep -o 'hardware HEVC decode[^.]*' | head -1)"
  results+=("$line|$hwmsg")
  echo "$per_second"
  sleep 1
done

echo
echo "== Summary (steady state = after the first 6 s)"
printf '%-16s %-10s %7s %8s %6s %9s %5s %5s %5s %5s %5s %s\n' \
  scenario size stream shown pct first_ms late qdrop rebs under cpu% verdict
fail=0
for entry in "${results[@]}"; do
  line="${entry%%|*}"
  declare -A f=()
  for kv in $line; do [[ "$kv" == *=* ]] && f["${kv%%=*}"]="${kv#*=}"; done
  pct=$(awk -v s="${f[steady_fps]}" -v t="${f[stream_fps]}" 'BEGIN{ printf "%d", (t > 0 ? s / t * 100 : 0) }')
  if   [ "$pct" -ge 90 ]; then v=PASS
  elif [ "$pct" -ge 75 ]; then v=WARN
  else v=FAIL; fail=1; fi
  printf '%-16s %-10s %7s %8s %5s%% %9s %5s %5s %5s %5s %4s%% %s\n' \
    "${f[name]}" "${f[size]}" "${f[stream_fps]}" "${f[steady_fps]}" "$pct" "${f[first_frame_ms]}" \
    "${f[late_drops]}" "${f[queue_drops]}" "${f[rebases]}" "${f[underruns]}" "${f[cpu_percent]}" "$v"
  unset f
done
echo
echo "PASS >= 90% of the stream's frame rate, WARN 75-90%, FAIL below. Details: diagnostics/README.md"
exit $fail
