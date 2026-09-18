#!/bin/bash
# Reverses everything scripts/initialSetup.sh, scripts/setup_autostart.sh,
# and scripts/setup_photo_album.sh set up: stops and removes both systemd
# --user services, the XDG autostart entry, the desktop shortcut, the
# MiniTiouner/RTL-SDR rules, RTL-SDR module blacklist, and the UDP-buffer
# sysctl tweak - then deletes the repo folder itself as the last step. This
# is the real, final step - there's
# no confirmation prompt and nothing is recoverable afterward (any
# uncommitted local changes, saved settings, and the whole screenshot album
# go with it). If you want to keep any of that, back it up before running
# this.
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

step "🚀 Removing XDG autostart entry..."
AUTOSTART_FILE="$HOME/.config/autostart/qo100datv.desktop"
if [ -f "$AUTOSTART_FILE" ]; then
    rm -f "$AUTOSTART_FILE"
    echo "   removed $AUTOSTART_FILE"
else
    echo "   not present, skipping"
fi

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

step "📡 Removing RTL-SDR system configuration (needs sudo)..."
RTLSDR_CONFIG_REMOVED=0
for RTLSDR_CONFIG in \
    /etc/udev/rules.d/60-qo100-rtlsdr.rules \
    /etc/modprobe.d/blacklist-qo100-rtlsdr.conf; do
    if [ -f "$RTLSDR_CONFIG" ]; then
        sudo rm -f "$RTLSDR_CONFIG"
        echo "   removed $RTLSDR_CONFIG"
        RTLSDR_CONFIG_REMOVED=1
    else
        echo "   $RTLSDR_CONFIG not present, skipping"
    fi
done
if [ "$RTLSDR_CONFIG_REMOVED" -eq 1 ]; then
    sudo udevadm control --reload-rules
    echo "   reboot to make the kernel-driver change take effect"
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
