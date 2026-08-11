#!/bin/bash
# First-time setup for the QO-100 DATV receiver.
#
# Installs build dependencies, fetches the two vendored libraries (pinned to
# the versions this repo actually expects), creates the longmynd status FIFO,
# installs the MiniTiouner udev rule, and builds both longmynd_ws and
# qo100_lvgl.
#
# Safe to re-run - every step is skipped (or is a no-op) if already done.
# Run from anywhere; paths are resolved relative to this script.
set -e

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_DIR"

echo "==> Installing build dependencies"
sudo apt update
sudo apt install -y \
    build-essential cmake pkg-config git \
    libusb-1.0-0-dev libasound2-dev libjson-c-dev libcap-dev \
    libsdl2-dev libwebsockets-dev \
    libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev

echo "==> Fetching vendored dependencies"
if [ ! -d qo100_lvgl/lvgl ]; then
    # Pinned to v9.5.0: qo100_lvgl/lv_conf.h is written for that exact
    # version, and LVGL has broken lv_conf.h compatibility across minor
    # versions before - an unpinned clone of master risks pulling ahead of
    # what this config expects.
    git clone --branch v9.5.0 https://github.com/lvgl/lvgl.git qo100_lvgl/lvgl
else
    echo "    qo100_lvgl/lvgl already present, skipping"
fi

if [ ! -d longmynd_ws/web/libwebsockets ]; then
    git clone --branch v4.3-stable https://github.com/warmcat/libwebsockets.git longmynd_ws/web/libwebsockets
else
    echo "    longmynd_ws/web/libwebsockets already present, skipping"
fi

echo "==> Creating longmynd status FIFO"
if [ ! -p longmynd_ws/longmynd_main_status ]; then
    # longmynd_ws/fifo.c only ever open()s this path, it never mkfifo()s it -
    # so it has to exist before the first run. It's a real filesystem entry
    # and persists across reboots once created, hence this being a one-time
    # step rather than something the app or Makefile handles.
    mkfifo longmynd_ws/longmynd_main_status
else
    echo "    longmynd_ws/longmynd_main_status already exists, skipping"
fi

echo "==> Building longmynd_ws"
(cd longmynd_ws && make clean && make)

echo "==> Installing MiniTiouner udev rule"
sudo cp longmynd_ws/minitiouner.rules /etc/udev/rules.d/minitiouner.rules
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=usb --attr-match=idVendor=0403 --attr-match=idProduct=6010

echo "==> Building qo100_lvgl"
cmake -S qo100_lvgl -B qo100_lvgl/build -DCMAKE_BUILD_TYPE=Release
cmake --build qo100_lvgl/build --target qo100datv -j"$(nproc)"

echo
echo "Setup complete."
echo "Run the app with:            qo100_lvgl/run.sh"
echo "Set up autostart (optional): scripts/setup_autostart.sh"
echo "Plug in the MiniTiouner (USB 0403:6010) before or after this script -" \
     "the udev rule picks it up on connect, no reboot needed."
