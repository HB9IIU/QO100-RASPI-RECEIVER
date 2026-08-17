# QO-100 DATV Receiver (Raspberry Pi 5)

A standalone QO-100 (Es'hail-2) digital amateur TV receiver. Runs on a
**Raspberry Pi 5**, driving a **MiniTiouner** tuner, with a fullscreen
touchscreen app that shows the spectrum, decodes the video, lets you tap to
tune, and connects to the QO-100 wideband chat.

## What you need

- Raspberry Pi 5, running Raspberry Pi OS (Bookworm or newer) with a desktop.
- A MiniTiouner tuner. Tested with the **Pro TS2** and the **S** — other
  variants (Express, original) likely work too but haven't been tried.
- A touchscreen — either a 1024x600 DSI panel or the official 800x480
  Raspberry Pi touchscreen (switchable any time from the app itself, see
  Step 5 below).
- Internet access on the Pi — needed for setup, and also ongoing during
  normal use (the spectrum display and chat both come from live internet
  feeds, separate from the tuner itself).

## Setup, step by step

### 1. Get the code

```bash
git clone https://github.com/HB9IIU/QO100-RASPI-RECEIVER.git DATVreceiver
cd DATVreceiver
```

This is a **private** repo, so `git` will ask you to log in to GitHub
(a personal access token, or an SSH key if you'd rather use that).

### 2. Run the setup script

```bash
scripts/initialSetup.sh
```

This one script does everything: installs all the software this app needs,
downloads one small third-party library it depends on, sets up the tuner so
it works without needing admin rights every time, and builds the app. It
will ask for your password (for the parts that need it). Safe to run more
than once if something goes wrong partway through.

### 3. Plug in the tuner

Plug the MiniTiouner into a USB port now, if it isn't already. Check it's
recognized:

```bash
lsusb | grep 0403:6010
```

You should see a line show up.

### 4. Try it out

```bash
cd qo100_sdl
./build_and_run.sh
```

A window should open showing the spectrum display, video, and status
panel. If the antenna is pointed at QO-100, it should lock onto the beacon
signal within a couple of seconds. Tap **EXIT** (or close the window) when
you're done checking.

If it doesn't lock: make sure the antenna/LNB is actually pointed at
QO-100, and check the **SET** page in the app for the LNB settings (LO
offset, bias voltage) match your hardware.

### 5. Pick your screen size

The app defaults to 1024x600. If your touchscreen is the smaller 800x480
official Raspberry Pi one instead: open **SET** in the app, choose
**800 x 480** under Display Resolution, and tap **SAVE** — the app restarts
itself in the new size automatically. You can change this again any time.

### 6. Make it start automatically

```bash
scripts/setup_autostart.sh
```

This makes the app start by itself every time the Pi boots up and you log
in, fullscreen, ready to go. To start it right now without rebooting:

```bash
systemctl --user start qo100datv.service
```

### 7. Reboot and check

```bash
sudo reboot
```

Once the Pi is back up and you're logged in, the receiver should appear on
its own, fullscreen, tuned to the QO-100 beacon. If it doesn't:

```bash
systemctl --user status qo100datv.service
```

This shows whether it's running, and if not, why.

## Using the app

- **Tap anywhere on the spectrum display** to tune to that signal.
- **CHAT** — opens the QO-100 wideband chat.
- **SET** — LNB settings (local oscillator offset, bias voltage) and screen
  resolution.
- **SCAN** — automatically hops between detected signals.
- **EXIT** — closes the app; it restarts itself a few seconds later.

## Optional: photo album on your LAN

A small gallery you can browse from any device on your home network,
including a **Take Screenshot** button on the page itself (useful when the
app is fullscreen with no touch access — it captures whatever's currently
on screen, not just this app). To turn it on:

```bash
scripts/setup_photo_album.sh
```

This prints a web address to open in a browser (e.g. `http://<pi's IP
address>:8090/`). Keeps the most recent 300 screenshots by default; delete
individual photos or select several for bulk delete right from the page.

## Making code changes

After editing the source, rebuild with:

```bash
cd qo100_sdl && ./build_and_run.sh
```

(Or `cd longmynd_ws && make` if you changed the tuner driver instead of the
app itself.)

## Repo layout

| Path | What it is |
|---|---|
| `longmynd_ws/` | The tuner driver (a modified version of an existing open-source project), talks to the MiniTiouner over USB. |
| `qo100_sdl/` | The actual app — the touchscreen UI you see and use. |
| `screenshots/` | Where the photo album keeps screenshots. |
| `scripts/` | The setup scripts described above. |

Default tuning at startup is the QO-100 beacon: 741474 kHz, 1500 ksps.
