# QO-100 DATV Receiver (Raspberry Pi 5)

A standalone QO-100 (Es'hail-2) digital amateur TV receiver: a **Raspberry Pi 5** driving a **MiniTiouner Pro TS2** tuner, with a custom fullscreen touchscreen UI built on SDL2.

## Hardware

- Raspberry Pi 5
- MiniTiouner Pro TS2 (FTDI FT2232H, USB `0403:6010`, reports as `MiniTiouner_Pro_TS2`)
- A touchscreen — either a 1024x600 DSI panel or the official 800x480 Raspberry Pi touchscreen. Switch between them from the app's **SET** page (Display Resolution), or see [HOWTO.md](HOWTO.md#5-choose-your-screen-resolution).

## Repo layout

| Path | What it is |
|---|---|
| `longmynd_ws/` | Vendored [philcrump/longmynd](https://github.com/philcrump/longmynd) fork — the tuner driver, extended with a websocket server (`-W <port>`) for JSON status and retune commands. |
| `qo100_sdl/` | The actual application (`src/main.cpp`). Custom SDL2 touchscreen UI; forks `longmynd_ws/longmynd` itself at startup and talks to it locally over websocket. |
| `screenshots/` | UI screenshots, captured via the app's on-screen **SNAP** button (or `scripts/screenshot.sh`). |
| `scripts/` | Standalone helper scripts. |

Default tuning at startup is the QO-100 beacon: **741474 kHz, 1500 ksps**.

## Quick setup (fresh Pi)

```bash
scripts/initialSetup.sh
```

Installs build dependencies, fetches the vendored library below (pinned to
the version this repo expects), creates the longmynd status FIFO, installs
the MiniTiouner udev rule, and builds both `longmynd_ws` and `qo100_sdl`.
Safe to re-run - every step is skipped if already done. See the sections
below for what it's doing and why, or to run any step by hand.

## Getting the vendored dependency

One third-party library is required to build but is not vendored in this repo (kept out to keep it small) — clone it into place before building:

```bash
git clone --branch v4.3-stable https://github.com/warmcat/libwebsockets.git longmynd_ws/web/libwebsockets
```

## Building

**`longmynd_ws`** (tuner driver + websocket server):

```bash
cd longmynd_ws
make clean && make
```

Build locally on the Pi — a binary copied from another machine can fail to start with a GLIBC version mismatch.

**`qo100_sdl`** (the UI app):

```bash
cd qo100_sdl
./build_and_run.sh
```

This configures and builds with CMake (`build/`, Release) and launches the app. Requires `SDL2`, `SDL2_ttf`, `libwebsockets`, `libavformat`/`libavcodec`/`libavutil`/`libswscale`/`libswresample` (FFmpeg), and `json-c` dev packages, plus `grim` and `imagemagick` at runtime for screenshots (SNAP button, `scripts/screenshot.sh`).

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

## Photo album (LAN screenshot gallery)

Every screenshot — from the in-app **SNAP** button or `scripts/screenshot.sh` — is
also archived into `screenshots/album/` (timestamped PNG + thumbnail) with an
auto-regenerated `index.html` gallery. Requires `imagemagick` (installed by
`scripts/initialSetup.sh`). Keeps the most recent 300 by default; override
with `ALBUM_KEEP=<n>`.

To serve that gallery on your LAN as a systemd `--user` service:

```bash
scripts/setup_photo_album.sh [PORT]   # default port 8090
```

Installs `~/.config/systemd/user/qo100album.service` (Python's built-in
`http.server`, no extra dependency), enables it, and prints the URL to browse
from any device on the LAN. Safe to re-run — the unit is always rewritten to
match the script. Check status with `systemctl --user status qo100album.service`.
