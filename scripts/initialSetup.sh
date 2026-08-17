#!/bin/bash
# First-time setup for the QO-100 DATV receiver.
#
# Installs build dependencies, fetches the vendored library (pinned to the
# version this repo actually expects), creates the longmynd status FIFO,
# installs the MiniTiouner udev rule, and builds both longmynd_ws and
# qo100_sdl.
#
# Safe to re-run - every step is skipped (or is a no-op) if already done.
# Run from anywhere; paths are resolved relative to this script.
set -e

# Colour only when actually printing to a terminal (not piped/redirected/
# logged to a file), so output stays clean either way.
if [ -t 1 ]; then
    BLUE='\033[1;34m'; GREEN='\033[1;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
else
    BLUE=''; GREEN=''; YELLOW=''; NC=''
fi

step() { printf "\n${BLUE}%s${NC}\n" "$1"; }
skip() { printf "${YELLOW}   ↷ %s${NC}\n" "$1"; }

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_DIR"

step "📦 Installing build dependencies..."
sudo apt update
sudo apt install -y \
    build-essential cmake pkg-config git \
    libusb-1.0-0-dev libasound2-dev libjson-c-dev libcap-dev \
    libsdl2-dev libsdl2-ttf-dev libwebsockets-dev \
    libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev \
    imagemagick grim python3

step "📚 Fetching vendored dependencies..."
if [ ! -d longmynd_ws/web/libwebsockets ]; then
    git clone --branch v4.3-stable https://github.com/warmcat/libwebsockets.git longmynd_ws/web/libwebsockets
else
    skip "longmynd_ws/web/libwebsockets already present"
fi

step "🧵 Creating the Longmynd status FIFO..."
if [ ! -p longmynd_ws/longmynd_main_status ]; then
    # longmynd_ws/fifo.c only ever open()s this path, it never mkfifo()s it -
    # so it has to exist before the first run. It's a real filesystem entry
    # and persists across reboots once created, hence this being a one-time
    # step rather than something the app or Makefile handles.
    mkfifo longmynd_ws/longmynd_main_status
else
    skip "longmynd_ws/longmynd_main_status already exists"
fi

step "🔨 Building longmynd_ws (the tuner driver)..."
(cd longmynd_ws && make clean && make)

step "🌐 Raising the UDP receive buffer limit..."
# Default net.core.rmem_max (~208KB on this Pi) caps the socket buffer
# FFmpeg's udp:// input requests (4MB) for the Longmynd transport-stream
# feed; a too-small buffer under bursty MPEG-TS arrival shows up as video
# stalls/late frames. Persists across reboots via a sysctl.d drop-in.
echo "net.core.rmem_max=8388608
net.core.rmem_default=8388608" | sudo tee /etc/sysctl.d/60-qo100-udp.conf > /dev/null
sudo sysctl --system > /dev/null

step "🔌 Installing the MiniTiouner udev rule (USB access without root)..."
sudo cp longmynd_ws/minitiouner.rules /etc/udev/rules.d/minitiouner.rules
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=usb --attr-match=idVendor=0403 --attr-match=idProduct=6010

step "🛠️  Building qo100_sdl (the receiver app)..."
cmake -S qo100_sdl -B qo100_sdl/build -DCMAKE_BUILD_TYPE=Release
cmake --build qo100_sdl/build --target qo100sdl -j"$(nproc)"

printf "\n${GREEN}✅ Setup complete!${NC}\n\n"
echo "▶️  Run the app with:            qo100_sdl/build_and_run.sh"
echo "🚀 Set up autostart (optional): scripts/setup_autostart.sh"
echo "🖼️  Screenshot album (optional): scripts/setup_photo_album.sh"
echo
echo "🔌 Plug in the MiniTiouner (USB 0403:6010) before or after this script -"
echo "   the udev rule picks it up on connect, no reboot needed."
