#!/bin/bash
# Reverses everything scripts/initialSetup.sh, scripts/setup_autostart.sh,
# and scripts/setup_photo_album.sh set up: stops and removes both systemd
# --user services, the desktop shortcut, the MiniTiouner udev rule, and the
# UDP-buffer sysctl tweak. Also cleans build output so the repo folder is
# back to a freshly-cloned state.
#
# What this does NOT do, on purpose:
#   - Remove apt packages (build-essential, SDL2, FFmpeg, etc.) - other
#     software on the system may depend on them, not safe to guess.
#   - Delete the repo folder itself (~/DATVreceiver) - remove that by hand
#     if you're done with it entirely: rm -rf ~/DATVreceiver
#   - Delete your screenshots/album photos or qo100_sdl/settings.json - see
#     --purge-data below if you want those gone too.
#
# Usage: scripts/uninstall.sh [--purge-data]
#   --purge-data  also deletes screenshots/ (all photos, not just the
#                 gallery) and qo100_sdl/settings.json (LNB/volume/
#                 resolution preferences). Off by default since these are
#                 the one thing here that isn't trivially regenerated.
#
# Safe to re-run - every step is skipped (or is a harmless no-op) if
# already undone.
set -e

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PURGE_DATA=0
[ "$1" = "--purge-data" ] && PURGE_DATA=1

echo "==> Stopping and removing systemd --user services"
for unit in qo100datv.service qo100album.service; do
    systemctl --user stop "$unit" 2>/dev/null || true
    systemctl --user disable "$unit" 2>/dev/null || true
    UNIT_FILE="$HOME/.config/systemd/user/$unit"
    if [ -f "$UNIT_FILE" ]; then
        rm -f "$UNIT_FILE"
        echo "    removed $UNIT_FILE"
    else
        echo "    $unit not present, skipping"
    fi
done
systemctl --user daemon-reload 2>/dev/null || true

echo "==> Removing desktop shortcut"
DESKTOP_FILE="$HOME/Desktop/qo100datv.desktop"
if [ -f "$DESKTOP_FILE" ]; then
    rm -f "$DESKTOP_FILE"
    echo "    removed $DESKTOP_FILE"
else
    echo "    not present, skipping"
fi

echo "==> Removing MiniTiouner udev rule (needs sudo)"
if [ -f /etc/udev/rules.d/minitiouner.rules ]; then
    sudo rm -f /etc/udev/rules.d/minitiouner.rules
    sudo udevadm control --reload-rules
    echo "    removed and reloaded udev rules"
else
    echo "    not present, skipping"
fi

echo "==> Removing UDP receive buffer sysctl tweak (needs sudo)"
if [ -f /etc/sysctl.d/60-qo100-udp.conf ]; then
    sudo rm -f /etc/sysctl.d/60-qo100-udp.conf
    sudo sysctl --system > /dev/null
    echo "    removed and reapplied sysctl settings"
else
    echo "    not present, skipping"
fi

echo "==> Cleaning build output and the vendored library"
rm -rf "$REPO_DIR/qo100_sdl/build" "$REPO_DIR/qo100_sdl/build-debug" \
       "$REPO_DIR/qo100_sdl/build-release"
rm -f "$REPO_DIR"/longmynd_ws/*.o "$REPO_DIR"/longmynd_ws/*.d \
      "$REPO_DIR/longmynd_ws/longmynd" "$REPO_DIR/longmynd_ws/fake_read" \
      "$REPO_DIR/longmynd_ws/ts_analyse"
rm -rf "$REPO_DIR/longmynd_ws/web/libwebsockets"
rm -f "$REPO_DIR/longmynd_ws/longmynd_main_status"

if [ "$PURGE_DATA" = "1" ]; then
    echo "==> --purge-data: removing screenshots and saved settings"
    rm -rf "$REPO_DIR/screenshots"
    rm -f "$REPO_DIR/qo100_sdl/settings.json"
else
    echo "==> Leaving screenshots/ and qo100_sdl/settings.json alone"
    echo "    (re-run with --purge-data to remove those too)"
fi

echo
echo "Uninstall complete."
echo "The repo itself is still at $REPO_DIR - delete it manually if you're"
echo "done with it entirely: rm -rf $REPO_DIR"
echo "apt packages were left installed - see this script's header comment"
echo "if you want to remove those by hand too."
