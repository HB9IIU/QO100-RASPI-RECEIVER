# HOWTO: Set up this receiver on a brand new Pi

Step-by-step walkthrough from a freshly flashed Raspberry Pi OS to a working,
auto-starting QO-100 DATV kiosk. For background on what each piece does, see
[README.md](README.md) — this file is just the ordered checklist.

## What you need

- Raspberry Pi 5, running Raspberry Pi OS (Bookworm or newer) with a desktop
  — the kiosk needs a display session (labwc/Wayland) to run in.
- MiniTiouner Pro TS2 tuner (USB `0403:6010`).
- A touchscreen — 1024x600 or 800x480 are both supported (see Step 5).
- Internet access on the Pi (for `apt` packages and cloning the repo).

## 1. Clone the repo

```bash
git clone https://github.com/HB9IIU/QO100-RASPI-RECEIVER.git DATVreceiver
cd DATVreceiver
```

This is a **private** repo, so `git` will prompt for GitHub authentication
(a personal access token, or an SSH remote/key if you'd rather use that).

## 2. Run initial setup

```bash
scripts/initialSetup.sh
```

This one script does everything needed to get from a bare OS to a built app:
installs all build/runtime dependencies (SDL2, FFmpeg, libwebsockets,
json-c, ImageMagick, grim, etc.), fetches the one vendored library
(`libwebsockets`, pinned version), creates the Longmynd status FIFO,
installs the MiniTiouner udev rule, raises the UDP receive buffer limit
(needed for smooth video), and builds both `longmynd_ws` and `qo100_sdl`.

It's safe to re-run — every step is skipped if already done. It will ask
for your `sudo` password (package installs, udev rule, sysctl).

## 3. Plug in the MiniTiouner

Plug it in via USB now if you haven't already. The udev rule installed in
Step 2 makes the device usable without root — no reboot needed. Verify with:

```bash
lsusb | grep 0403:6010
```

## 4. Test it manually

```bash
cd qo100_sdl
./build_and_run.sh
```

This builds (picking up any local changes) and launches the app windowed on
your desktop. You should see the spectrum display, video panel, and status
panel come up, and — pointed at QO-100 — the beacon should lock within a
couple of seconds. Close the window (or tap **EXIT**) when done checking.

If it doesn't lock: check the antenna/LNB setup is actually pointed at
QO-100, and check **SET** in the app for the LNB LO offset and bias voltage
match your hardware.

## 5. Choose your screen resolution

Default is 1024x600. If your panel is the smaller 800x480 official
Raspberry Pi touchscreen instead, open **SET** in the app, choose
**800 x 480** under Display Resolution, and tap **SAVE** — the app restarts
itself into the new size automatically. (This also works after autostart is
set up in the next step — the choice is read fresh from `settings.json` on
every launch, so it always takes effect on the next restart.)

## 6. Set up autostart (kiosk boot)

```bash
scripts/setup_autostart.sh
```

Installs a systemd `--user` service (`qo100datv.service`) that launches the
app automatically at login, and adds a desktop shortcut as a fallback/manual
way to start it. Enabled but not started yet — start it now with:

```bash
systemctl --user start qo100datv.service
```

Check it came up cleanly:

```bash
systemctl --user status qo100datv.service
```

## 7. (Optional) LAN screenshot album

If you want a web page on your LAN showing every screenshot taken (via the
in-app **SNAP** button or `scripts/screenshot.sh`):

```bash
scripts/setup_photo_album.sh
```

Prints the URL to browse to (default port 8090) once it's running.

## 8. Reboot and confirm

```bash
sudo reboot
```

After the Pi comes back up and you're logged into the desktop, the receiver
should start automatically, fullscreen, tuned to the QO-100 beacon. If it
doesn't:

- `systemctl --user status qo100datv.service` — is it running? Check the
  logs shown there for the actual failure.
- `journalctl --user -u qo100datv.service -n 100` — fuller log history.
- Confirm the MiniTiouner is still detected: `lsusb | grep 0403:6010`.

## You're done

Day-to-day, the app runs itself — tap tx slots on the spectrum to tune,
**SCAN** to auto-hop between detected signals, **CHAT** for the wideband
chat, **SET** for LNB/resolution settings. See README.md's other sections
for anything not covered here (repo layout, manual building, the LAN photo
album in more detail).
