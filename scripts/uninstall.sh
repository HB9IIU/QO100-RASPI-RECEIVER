#!/bin/bash
# Reverses everything scripts/initialSetup.sh, scripts/setup_autostart.sh,
# and scripts/setup_photo_album.sh set up: stops and removes both systemd
# --user services, the desktop shortcut, the MiniTiouner udev rule, and the
# UDP-buffer sysctl tweak - then deletes the repo folder itself as the last
# step. This is the real, final step - there's no confirmation prompt and
# nothing is recoverable afterward (any uncommitted local changes, saved
# settings, and the whole screenshot album go with it). If you want to keep
# any of that, back it up before running this.
#
# What this does NOT do, on purpose:
#   - Remove apt packages (build-essential, SDL2, FFmpeg, etc.) - other
#     software on the system may depend on them, not safe to guess.
#
# Safe to re-run up until the final deletion, which is obviously a one-shot.
set -e

if [ -t 1 ]; then
    BLUE='\033[1;34m'; GREEN='\033[1;32m'; RED='\033[1;31m'; NC='\033[0m'
else
    BLUE=''; GREEN=''; RED=''; NC=''
fi

step() { printf "\n${BLUE}%s${NC}\n" "$1"; }

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"

step "🛑 Stopping and removing systemd --user services..."
for unit in qo100datv.service qo100album.service; do
    systemctl --user stop "$unit" 2>/dev/null || true
    systemctl --user disable "$unit" 2>/dev/null || true
    UNIT_FILE="$HOME/.config/systemd/user/$unit"
    if [ -f "$UNIT_FILE" ]; then
        rm -f "$UNIT_FILE"
        echo "   removed $UNIT_FILE"
    else
        echo "   $unit not present, skipping"
    fi
done
systemctl --user daemon-reload 2>/dev/null || true

step "🖥️  Removing desktop shortcut..."
DESKTOP_FILE="$HOME/Desktop/qo100datv.desktop"
if [ -f "$DESKTOP_FILE" ]; then
    rm -f "$DESKTOP_FILE"
    echo "   removed $DESKTOP_FILE"
else
    echo "   not present, skipping"
fi

step "🔌 Removing MiniTiouner udev rule (needs sudo)..."
if [ -f /etc/udev/rules.d/minitiouner.rules ]; then
    sudo rm -f /etc/udev/rules.d/minitiouner.rules
    sudo udevadm control --reload-rules
    echo "   removed and reloaded udev rules"
else
    echo "   not present, skipping"
fi

step "🌐 Removing UDP receive buffer sysctl tweak (needs sudo)..."
if [ -f /etc/sysctl.d/60-qo100-udp.conf ]; then
    sudo rm -f /etc/sysctl.d/60-qo100-udp.conf
    sudo sysctl --system > /dev/null
    echo "   removed and reapplied sysctl settings"
else
    echo "   not present, skipping"
fi

printf "\n${RED}🗑️  Deleting %s ...${NC}\n" "$REPO_DIR"
cd /
rm -rf "$REPO_DIR"

printf "\n${GREEN}✅ Uninstall complete. %s is gone.${NC}\n" "$REPO_DIR"
echo "apt packages were left installed - see this script's header comment"
echo "if you want to remove those by hand too."
