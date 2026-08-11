#!/bin/bash
# Sets up the QO-100 DATV receiver desktop shortcut and boot-time autostart.
#
# 1. Creates ~/Desktop/qo100datv.desktop if it doesn't already exist
#    (builds via run.sh, so source changes are picked up).
# 2. Installs and enables a systemd --user service that launches the
#    prebuilt binary directly at login (no build step, no toolchain
#    dependency at boot). Waits for the X11 socket before launching, since
#    XWayland starts on demand and this service can otherwise race ahead
#    of it (app runs with audio but no visible window).
#
# Usage: scripts/setup_autostart.sh [WxH]
#   No argument: default 1024x600 kiosk panel (this Pi's original hardware).
#   e.g. "800x480": a second kiosk unit with a different real panel size -
#   see qo100datv.cpp's QO100_DISPLAY handling. Always fullscreen either way
#   (that flag only goes windowed if QO100_WINDOWED is also set, which this
#   script never does - autostart is always the real kiosk, never a preview).
#
# Safe to re-run: the desktop shortcut is left alone if present, the
# service unit is always rewritten to match this script.
set -e

DISPLAY_RES="$1"
REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
APP_BIN="$REPO_DIR/qo100_lvgl/build/qo100datv"
APP_ICON="$REPO_DIR/assets/qo100datv-icon.png"
DESKTOP_FILE="$HOME/Desktop/qo100datv.desktop"
SERVICE_DIR="$HOME/.config/systemd/user"
SERVICE_FILE="$SERVICE_DIR/qo100datv.service"

if [ -f "$DESKTOP_FILE" ]; then
    echo "Desktop shortcut already exists at $DESKTOP_FILE, leaving it alone."
else
    mkdir -p "$HOME/Desktop"
    cat > "$DESKTOP_FILE" <<EOF
[Desktop Entry]
Name=QO-100 DATV Receiver
Comment=Start the QO-100 DATV receiver UI
Exec=lxterminal -e $REPO_DIR/qo100_lvgl/run.sh
Path=$REPO_DIR/qo100_lvgl
Icon=$APP_ICON
Terminal=false
Type=Application
StartupNotify=false
Categories=AudioVideo;
EOF
    chmod +x "$DESKTOP_FILE"
    echo "Created desktop shortcut at $DESKTOP_FILE"
fi

if [ ! -x "$APP_BIN" ]; then
    echo "Warning: $APP_BIN doesn't exist yet - build it first with qo100_lvgl/run.sh," \
         "otherwise the autostart service will fail until you do."
fi

mkdir -p "$SERVICE_DIR"
cat > "$SERVICE_FILE" <<EOF
[Unit]
Description=QO-100 DATV Receiver UI
After=default.target

[Service]
Type=simple
Environment=DISPLAY=:0.0
$([ -n "$DISPLAY_RES" ] && echo "Environment=QO100_DISPLAY=$DISPLAY_RES")
WorkingDirectory=$REPO_DIR/qo100_lvgl
# labwc starts XWayland on demand; at boot this service can otherwise race
# ahead of it and connect to a DISPLAY that isn't ready yet. The app then
# runs (decoding/rendering internally, audio audible) with no visible
# window. Wait for the X11 socket before launching.
ExecStartPre=/bin/sh -c 'for i in \$(seq 1 60); do [ -S /tmp/.X11-unix/X0 ] && exit 0; sleep 0.5; done; echo "X11 socket never appeared"; exit 1'
ExecStart=$APP_BIN
Restart=on-failure
RestartSec=3

[Install]
WantedBy=default.target
EOF

systemctl --user daemon-reload
systemctl --user enable qo100datv.service
echo "Installed and enabled qo100datv.service - it will start automatically at next login."
echo "To start it right now: systemctl --user start qo100datv.service"
echo "To check status/logs:  systemctl --user status qo100datv.service"
