# QO-100 DATV Receiver (Raspberry Pi 5)

A standalone QO-100 (Es'hail-2) digital amateur TV receiver: a **Raspberry Pi 5** driving a **MiniTiouner Pro TS2** tuner, with a custom fullscreen touchscreen UI built on SDL2.

## Hardware

- Raspberry Pi 5
- A MiniTiouner (FTDI FT2232H, USB `0403:6010`). Tested with the **Pro TS2** (reports as `MiniTiouner_Pro_TS2`) and the **S** (reports as `MiniTiouner`) — detection is by USB VID:PID, not model, so other variants (Express, original) likely work too but haven't been tried.
- A touchscreen — either a 1024x600 DSI panel or the official 800x480 Raspberry Pi touchscreen. Switch between them from the app's **SET** page (Display Resolution), or see [HOWTO.md](HOWTO.md#5-choose-your-screen-resolution).

## Repo layout

| Path | What it is |
|---|---|
| `longmynd_ws/` | Vendored [philcrump/longmynd](https://github.com/philcrump/longmynd) fork — the tuner driver, extended with a websocket server (`-W <port>`) for JSON status and retune commands. |
| `qo100_sdl/` | The actual application (`src/main.cpp`). Custom SDL2 touchscreen UI; forks `longmynd_ws/longmynd` itself at startup and talks to it locally over websocket. |
| `screenshots/` | UI screenshots, captured via the app's on-screen **SNAP** button (or `scripts/screenshot.sh`). |
| `scripts/` | Standalone helper scripts. |

Default tuning at startup is the QO-100 beacon: **741474 kHz, 1500 ksps**.

## Setup (fresh Pi)

See **[HOWTO.md](HOWTO.md)** for the full step-by-step walkthrough. The short version:

```bash
scripts/initialSetup.sh
```

This one script installs everything needed (build tools, `SDL2`/`SDL2_ttf`,
FFmpeg, `libwebsockets`, `json-c`, `grim`, `imagemagick`...), fetches the one
vendored dependency not included in this repo (`libwebsockets`), installs
the MiniTiouner udev rule, and builds both `longmynd_ws` and `qo100_sdl`.
Safe to re-run.

## Rebuilding after a code change

```bash
cd qo100_sdl && ./build_and_run.sh
```

Or `cd longmynd_ws && make` if you touched the tuner driver instead.

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
