#!/bin/bash
# First-time setup for the QO-100 DATV receiver.
#
# Installs build dependencies, fetches the vendored library (pinned to the
# version this repo actually expects), creates the longmynd status FIFO,
# installs the MiniTiouner and RTL-SDR udev rules, prevents Linux's DVB
# driver from claiming RTL-SDR sticks, builds both longmynd_ws and qo100_sdl,
# sets up autostart (scripts/setup_autostart.sh), and reboots at the end.
# A reboot is required both for new USB permissions and for the module
# blacklist to release a stick already claimed by the kernel.
#
# Safe to re-run - every step is skipped (or is a no-op) if already done.
# Run from anywhere; paths are resolved relative to this script.
set -e

# Colour only when actually printing to a terminal (not piped/redirected/
# logged to a file), so output stays clean either way.
if [ -t 1 ]; then
    BLUE='\033[1;34m'; GREEN='\033[1;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
else
    BLUE=''; GREEN=''; YELLOW=''; NC=''
fi

step() { printf "\n${BLUE}%s${NC}\n" "$1"; }
skip() { printf "${YELLOW}   ↷ %s${NC}\n" "$1"; }

# Set by the app itself when it runs this script in the background to apply
# an update (no keyboard on a touchscreen kiosk to press Enter with, and
# nobody's watching a terminal for this banner anyway - the app shows its
# own on-screen "Updating..." status instead).
if [ -z "$QO100_NONINTERACTIVE" ]; then
printf '%b' "$(cat <<BANNER

${BLUE}================================================================${NC}
${BLUE}  QO-100 DATV Receiver - first-time setup${NC}
${BLUE}================================================================${NC}

This script WILL:
  - Install some software packages via apt (build tools, SDL2,
    FFmpeg, and a few other libraries this app needs)
  - Download one small open-source library (libwebsockets) from
    GitHub
  - Build the receiver app and the tuner driver from source
  - Install USB access rules for the MiniTiouner and RTL-SDR
  - Prevent Linux's television driver from claiming the RTL-SDR
  - Adjust one system network setting (UDP receive buffer size) for
    smoother video
  - Set the app to start automatically when the Pi boots
  - Reboot the Pi at the end, so all of the above actually takes
    effect (needed for USB rules and the RTL-SDR driver blacklist)

${GREEN}This script will NOT:${NC}
  - Send any of your files, data, or settings anywhere
  - Install anything beyond what's listed above
  - Touch any other project or folder on this Pi
  - Ask you to sign up for or log in to anything
  Everything it downloads comes from Debian's own package repos and
  GitHub - ordinary, public open-source sources, nothing bundled or
  hidden.

Press ${GREEN}ENTER${NC} to continue, or ${YELLOW}Ctrl+C${NC} to cancel.
BANNER
)"
read -r _
fi

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_DIR"

step "🕐 Checking the Pi's clock..."
# A Pi has no battery-backed clock, so after a long power-off (or a broken
# time sync) it can start days in the past. apt and git-over-https then
# reject everything as "not valid yet" and the rest of this script fails
# with a cryptic error - so sort the clock out before anything else.
sudo timedatectl set-ntp true || true
for _ in $(seq 1 30); do
    [ "$(timedatectl show -p NTPSynchronized --value 2>/dev/null)" = "yes" ] && break
    sleep 2
done
if [ "$(timedatectl show -p NTPSynchronized --value 2>/dev/null)" = "yes" ]; then
    skip "clock is synchronised: $(date)"
else
    printf "${YELLOW}   ⚠ Could not synchronise the clock (is the Pi online?). It says: %s${NC}\n" "$(date)"
    echo "     If that date is wrong, package downloads will fail."
fi

step "⬇️  Pulling the latest version..."
# A no-op on a fresh clone (already at the latest commit); the actual point
# of this step is re-running this script later to pick up an update - see
# the "Updating" section in README.md, and the in-app update check.
git pull --ff-only

step "📦 Installing build dependencies..."
sudo apt update || {
    echo >&2
    echo "ERROR: apt update failed. Check the Pi is online and that its clock is correct" >&2
    echo "(it says: $(date)). Fix that, then run this script again." >&2
    exit 1
}
sudo apt install -y \
    build-essential cmake pkg-config git \
    libusb-1.0-0-dev libasound2-dev libjson-c-dev libcap-dev \
    libsdl2-dev libsdl2-ttf-dev libwebsockets-dev \
    libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev \
    imagemagick grim python3

step "📚 Fetching vendored dependencies..."
if [ ! -d longmynd_ws/web/libwebsockets ]; then
    git clone --branch v4.3-stable https://github.com/warmcat/libwebsockets.git longmynd_ws/web/libwebsockets
else
    skip "longmynd_ws/web/libwebsockets already present"
fi

step "🧵 Creating the Longmynd status FIFO..."
if [ ! -p longmynd_ws/longmynd_main_status ]; then
    # longmynd_ws/fifo.c only ever open()s this path, it never mkfifo()s it -
    # so it has to exist before the first run. It's a real filesystem entry
    # and persists across reboots once created, hence this being a one-time
    # step rather than something the app or Makefile handles.
    mkfifo longmynd_ws/longmynd_main_status
else
    skip "longmynd_ws/longmynd_main_status already exists"
fi

step "🔨 Building longmynd_ws (the tuner driver)..."
(cd longmynd_ws && make clean && make)

step "🌐 Raising the UDP receive buffer limit..."
# Default net.core.rmem_max (~208KB on this Pi) caps the socket buffer
# FFmpeg's udp:// input requests (4MB) for the Longmynd transport-stream
# feed; a too-small buffer under bursty MPEG-TS arrival shows up as video
# stalls/late frames. Persists across reboots via a sysctl.d drop-in.
echo "net.core.rmem_max=8388608
net.core.rmem_default=8388608" | sudo tee /etc/sysctl.d/60-qo100-udp.conf > /dev/null
sudo sysctl --system > /dev/null

step "🔌 Installing the MiniTiouner udev rule (USB access without root)..."
sudo cp longmynd_ws/minitiouner.rules /etc/udev/rules.d/minitiouner.rules

step "📡 Configuring RTL-SDR access..."
# Keep this rule project-owned instead of depending on the distribution's
# rtl-sdr package being installed. These are the two Realtek identities the
# app currently recognises at startup. The reboot below refreshes the user's
# plugdev group membership as well as applying the rule to connected sticks.
sudo tee /etc/udev/rules.d/60-qo100-rtlsdr.rules > /dev/null <<'EOF'
# QO-100 DATV Receiver: permit non-root access to RTL2832U RTL-SDR sticks.
SUBSYSTEMS=="usb", ATTRS{idVendor}=="0bda", ATTRS{idProduct}=="2832", MODE="0660", GROUP="plugdev", TAG+="uaccess"
SUBSYSTEMS=="usb", ATTRS{idVendor}=="0bda", ATTRS{idProduct}=="2838", MODE="0660", GROUP="plugdev", TAG+="uaccess"
EOF
sudo usermod -a -G plugdev "$USER"

# These kernel modules treat an RTL2832U as a television receiver and claim
# its USB interface before librtlsdr can open it. A blacklist is preferable
# to unloading them here: unloading could disrupt another active device and
# would not survive the next boot or unplug/replug cycle.
sudo tee /etc/modprobe.d/blacklist-qo100-rtlsdr.conf > /dev/null <<'EOF'
# QO-100 DATV Receiver: reserve RTL2832U devices for librtlsdr.
blacklist dvb_usb_rtl28xxu
blacklist rtl2832
blacklist rtl2832_sdr
EOF

sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=usb --attr-match=idVendor=0403 --attr-match=idProduct=6010
sudo udevadm trigger --subsystem-match=usb --attr-match=idVendor=0bda || true

step "📻 Checking the bundled RTL-SDR spectrum server..."
if [ ! -f qo100_sdl/tools/rtl-sdr-server ]; then
    echo "ERROR: qo100_sdl/tools/rtl-sdr-server is missing after git pull." >&2
    exit 1
fi
chmod 755 qo100_sdl/tools/rtl-sdr-server

step "🛠️  Building qo100_sdl (the receiver app)..."
cmake -S qo100_sdl -B qo100_sdl/build -DCMAKE_BUILD_TYPE=Release
cmake --build qo100_sdl/build --target qo100sdl -j"$(nproc)"

step "🚀 Setting up autostart..."
# Don't launch the app here - it would pop up fullscreen mid-setup, hiding
# this terminal (and the reboot notice/countdown coming up next) right when
# it matters. The reboot a few steps down starts it for real.
QO100_SKIP_SERVICE_START=1 "$REPO_DIR/scripts/setup_autostart.sh"

printf "\n${GREEN}✅ Setup complete!${NC}\n\n"
echo "🖼️  Screenshot album (optional): $REPO_DIR/scripts/setup_photo_album.sh"
echo
echo "🔌 Plug in the MiniTiouner and RTL-SDR now if they aren't already."
echo
printf "${YELLOW}🔁 Rebooting now to make sure everything actually takes effect...${NC}\n"
sleep 3
sudo reboot
