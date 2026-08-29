#!/bin/bash
# Sets up the QO-100 DATV receiver desktop shortcut and boot-time autostart.
#
# 1. Creates ~/Desktop/qo100datv.desktop if it doesn't already exist
#    (builds via build_and_run.sh, so source changes are picked up).
# 2. Installs (but does not itself enable-at-boot) a systemd --user service
#    that launches the prebuilt binary directly at login (no build step, no
#    toolchain dependency at boot) - kept for process supervision: it's what
#    gives us Restart=always, `systemctl --user status`, and journal logs.
# 3. Installs an XDG autostart entry (~/.config/autostart/qo100datv.desktop)
#    that starts that service. This - not any systemd target - is the actual
#    "desktop is ready" hook on this labwc-based image: /etc/xdg/labwc/
#    autostart runs pcmanfm-pi, wf-panel-pi, kanshi, then as its LAST step
#    invokes lxsession-xdg-autostart, which is what processes .desktop files
#    under ~/.config/autostart. By the time that runs, labwc's own startup
#    (compositor, input backend, touchscreen wiring) is already done, since
#    labwc doesn't get to running its autostart script until it is.
#    graphical-session.target was tried first and turned out to be dead
#    weight on this image (never activated - not even wf-panel-pi, the
#    desktop's own panel, uses it), so it's left in the unit as a harmless,
#    inert fallback rather than relied upon.
#
# Usage: scripts/setup_autostart.sh [WxH]
#   No argument (the normal case): resolution comes from settings.json's
#   display_800x480 (defaults to 1024x600), read fresh on every launch by
#   qo100_sdl/service_launch.sh - this is what lets the SET page's resolution
#   choice take effect on its own restart, no need to re-run this script.
#   e.g. "800x480": pins the unit to that resolution regardless of what's
#   saved in settings.json - for a genuinely different physical panel, not
#   the normal way to switch resolution. See main.cpp's QO100_DISPLAY
#   handling. Always fullscreen either way (that flag only goes windowed if
#   QO100_WINDOWED is also set, which this script never does - autostart is
#   always the real kiosk, never a preview).
#
# Safe to re-run: the desktop shortcut is left alone if present, the
# service unit is always rewritten to match this script.
set -e

DISPLAY_RES="$1"
REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
APP_BIN="$REPO_DIR/qo100_sdl/build/qo100sdl"
APP_LAUNCHER="$REPO_DIR/qo100_sdl/service_launch.sh"
APP_ICON="$REPO_DIR/assets/qo100datv-icon.png"
DESKTOP_FILE="$HOME/Desktop/qo100datv.desktop"
SERVICE_DIR="$HOME/.config/systemd/user"
SERVICE_FILE="$SERVICE_DIR/qo100datv.service"
AUTOSTART_DIR="$HOME/.config/autostart"
AUTOSTART_FILE="$AUTOSTART_DIR/qo100datv.desktop"

if [ -f "$DESKTOP_FILE" ]; then
    echo "Desktop shortcut already exists at $DESKTOP_FILE, leaving it alone."
else
    mkdir -p "$HOME/Desktop"
    cat > "$DESKTOP_FILE" <<EOF
[Desktop Entry]
Name=QO-100 DATV Receiver
Comment=Start the QO-100 DATV receiver UI
Exec=lxterminal -e $REPO_DIR/qo100_sdl/build_and_run.sh
Path=$REPO_DIR/qo100_sdl
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
    echo "Warning: $APP_BIN doesn't exist yet - build it first with qo100_sdl/build_and_run.sh," \
         "otherwise the autostart service will fail until you do."
fi

mkdir -p "$SERVICE_DIR"
if [ -f "$SERVICE_FILE" ]; then
    # Disable against whatever [Install] section is currently on disk before
    # overwriting it below - otherwise a service previously installed with
    # WantedBy=default.target would leave a stale default.target.wants
    # symlink behind, which would keep pulling the service in early (racing
    # ahead of the desktop session again) even after this script switches it
    # to graphical-session.target.
    systemctl --user disable qo100datv.service 2>/dev/null || true
fi
cat > "$SERVICE_FILE" <<EOF
[Unit]
Description=QO-100 DATV Receiver UI
# graphical-session.target is what labwc activates once the desktop session
# has actually finished starting (compositor up, touchscreen wired in).
# PartOf ties our lifecycle to it: if the graphical session ever restarts,
# this service restarts with it instead of surviving as an orphan pointed
# at a dead session.
PartOf=graphical-session.target
After=graphical-session.target

[Service]
Type=simple
Environment=DISPLAY=:0.0
$([ -n "$DISPLAY_RES" ] && echo "Environment=QO100_DISPLAY=$DISPLAY_RES")
WorkingDirectory=$REPO_DIR/qo100_sdl
# Secondary safety net on top of the graphical-session.target ordering
# above: XWayland itself starts on demand, so wait for its socket too
# before launching, in case there's still a hair of a race.
ExecStartPre=/bin/sh -c 'for i in \$(seq 1 60); do [ -S /tmp/.X11-unix/X0 ] && exit 0; sleep 0.5; done; echo "X11 socket never appeared"; exit 1'
ExecStart=$APP_LAUNCHER
# always, not on-failure: EXIT (running=false) exits cleanly with status 0,
# which on-failure would treat as "stopped on purpose" and leave dead. Tap
# EXIT to restart the app (e.g. to pick up a settings.json edit) is the
# intended behaviour here.
Restart=always
RestartSec=3

[Install]
WantedBy=graphical-session.target
EOF

mkdir -p "$AUTOSTART_DIR"
cat > "$AUTOSTART_FILE" <<EOF
[Desktop Entry]
Type=Application
Name=QO-100 DATV Receiver (autostart)
Exec=systemctl --user start qo100datv.service
Hidden=false
NoDisplay=true
X-GNOME-Autostart-enabled=true
EOF
echo "Installed XDG autostart entry at $AUTOSTART_FILE"

systemctl --user daemon-reload
systemctl --user enable qo100datv.service
if [ -z "$QO100_SKIP_SERVICE_START" ]; then
    systemctl --user restart qo100datv.service
    echo "Installed, enabled, and started qo100datv.service - it will also start"
    echo "automatically at next login."
else
    # initialSetup.sh sets this: starting the app here would pop up
    # fullscreen mid-setup, hiding the terminal (and its own "setup
    # complete" message and reboot countdown) right as it's needed - and
    # udev hasn't actually been triggered for real yet at this point in a
    # first-time setup, so it could flash a misleading "No MiniTiouner
    # detected" too. enable is enough; the imminent reboot starts it for
    # real.
    echo "Installed and enabled qo100datv.service - it will start automatically"
    echo "at next boot."
fi
echo "To check status/logs: systemctl --user status qo100datv.service"
