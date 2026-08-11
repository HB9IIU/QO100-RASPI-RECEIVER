# QO-100 DATV Receiver (Raspberry Pi 5)

A standalone QO-100 (Es'hail-2) digital amateur TV receiver: a **Raspberry Pi 5** driving a **MiniTiouner Pro TS2** tuner, with a custom fullscreen touchscreen UI built on LVGL.

## Hardware

- Raspberry Pi 5
- MiniTiouner Pro TS2 (FTDI FT2232H, USB `0403:6010`, reports as `MiniTiouner_Pro_TS2`)
- 1024x600 DSI touchscreen

## Repo layout

| Path | What it is |
|---|---|
| `longmynd_ws/` | Vendored [philcrump/longmynd](https://github.com/philcrump/longmynd) fork — the tuner driver, extended with a websocket server (`-W <port>`) for JSON status and retune commands. |
| `qo100_lvgl/` | The actual application (`src/qo100datv.cpp`). Custom LVGL touchscreen UI; forks `longmynd_ws/longmynd` itself at startup and talks to it locally over websocket. |
| `screenshots/` | UI screenshots, captured via the app's on-screen **SNAP** button (or `scripts/screenshot.sh`). |
| `scripts/` | Standalone helper scripts. |

Default tuning at startup is the QO-100 beacon: **741474 kHz, 1500 ksps**.

## Quick setup (fresh Pi)

```bash
scripts/initialSetup.sh
```

Installs build dependencies, fetches the two vendored libraries below (pinned
to the versions this repo expects), creates the longmynd status FIFO,
installs the MiniTiouner udev rule, and builds both `longmynd_ws` and
`qo100_lvgl`. Safe to re-run - every step is skipped if already done. See the
sections below for what it's doing and why, or to run any step by hand.

## Getting the vendored dependencies

Two third-party libraries are required to build but are not vendored in this repo (kept out to keep it small) — clone them into place before building:

```bash
git clone https://github.com/lvgl/lvgl.git qo100_lvgl/lvgl
git clone --branch v4.3-stable https://github.com/warmcat/libwebsockets.git longmynd_ws/web/libwebsockets
```

## Building

**`longmynd_ws`** (tuner driver + websocket server):

```bash
cd longmynd_ws
make clean && make
```

Build locally on the Pi — a binary copied from another machine can fail to start with a GLIBC version mismatch.

**`qo100_lvgl`** (the UI app):

```bash
cd qo100_lvgl
./run.sh
```

This configures and builds with CMake (`build/`, Release) and launches the app. Requires `SDL2`, `libwebsockets`, `libavformat`/`libavcodec`/`libavutil`/`libswscale`/`libswresample` (FFmpeg), and `json-c` dev packages.

## First-time device setup

The MiniTiouner needs a udev rule so it's readable without root:

```bash
sudo cp longmynd_ws/minitiouner.rules /etc/udev/rules.d/minitiouner.rules
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=usb --attr-match=idVendor=0403 --attr-match=idProduct=6010
```

Verify with `lsusb` + `ls -la /dev/bus/usb/<bus>/<dev>` — permissions should read `crw-rw-rw-`.

## Screenshots

See `screenshots/` — includes the live spectrum/alignment view and the BATC wideband chat panel.
