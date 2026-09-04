#!/bin/bash
# PicoTuner test build - QO-100 DATV Receiver
#
# Installs a SEPARATE copy of the app, built from the `picotuner-support`
# branch, side by side with any existing installation. It does not touch
# ~/DATVreceiver, its systemd service, its udev rule file, or its desktop
# shortcut - this is a completely independent install living in its own
# folder, with its own desktop icon, so a working existing setup is never
# at risk.
#
# This build does NOT autostart at boot and installs no systemd service -
# it's meant to be launched by hand from its desktop icon while someone is
# watching, since PicoTuner support is new and unverified against real
# hardware. The icon opens a terminal, runs the app, and saves everything
# printed to a timestamped log file for sending back.
#
# Not linked from anywhere in the README - shared by direct link only.
set -e

if [ -t 1 ]; then
    BLUE='\033[1;34m'; GREEN='\033[1;32m'; YELLOW='\033[1;33m'; CYAN='\033[1;36m'; NC='\033[0m'
else
    BLUE=''; GREEN=''; YELLOW=''; CYAN=''; NC=''
fi

step() { printf "\n${BLUE}%s${NC}\n" "$1"; }
skip() { printf "${YELLOW}   ↷ %s${NC}\n" "$1"; }

INSTALL_DIR="$HOME/DATVreceiver-picotuner-test"

printf '%b' "$(cat <<BANNER

${CYAN}================================================================${NC}
${CYAN}  QO-100 DATV Receiver - PicoTuner test build${NC}
${CYAN}================================================================${NC}

This installs a SEPARATE, side-by-side copy of the app into:
  ${GREEN}${INSTALL_DIR}${NC}

It will NOT touch any existing DATVreceiver install, its autostart,
or its desktop shortcut - you'll get a second, independent desktop
icon called "QO-100 DATV (PicoTuner TEST)".

Press ${GREEN}ENTER${NC} to continue, or ${YELLOW}Ctrl+C${NC} to cancel.
BANNER
)"
read -r _

step "📦 Installing build dependencies..."
sudo apt update
sudo apt install -y \
    build-essential cmake pkg-config git \
    libusb-1.0-0-dev libasound2-dev libjson-c-dev libcap-dev \
    libsdl2-dev libsdl2-ttf-dev libwebsockets-dev \
    libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev \
    imagemagick grim python3

step "⬇️  Getting the picotuner-support branch..."
if [ -d "$INSTALL_DIR/.git" ]; then
    skip "$INSTALL_DIR already exists, pulling latest instead"
    cd "$INSTALL_DIR"
    git fetch origin
    git checkout picotuner-support
    git pull --ff-only origin picotuner-support
else
    git clone --branch picotuner-support https://github.com/HB9IIU/QO100-RASPI-RECEIVER.git "$INSTALL_DIR"
    cd "$INSTALL_DIR"
fi

step "📚 Fetching vendored dependencies..."
if [ ! -d longmynd_ws/web/libwebsockets ]; then
    git clone --branch v4.3-stable https://github.com/warmcat/libwebsockets.git longmynd_ws/web/libwebsockets
else
    skip "longmynd_ws/web/libwebsockets already present"
fi

step "🧵 Creating the Longmynd status FIFO..."
if [ ! -p longmynd_ws/longmynd_main_status ]; then
    mkfifo longmynd_ws/longmynd_main_status
else
    skip "longmynd_ws/longmynd_main_status already exists"
fi

step "🔨 Building longmynd_ws (the tuner driver, with PicoTuner support)..."
(cd longmynd_ws && make clean && make)

step "🌐 Raising the UDP receive buffer limit..."
echo "net.core.rmem_max=8388608
net.core.rmem_default=8388608" | sudo tee /etc/sysctl.d/60-qo100-udp.conf > /dev/null
sudo sysctl --system > /dev/null

step "🔌 Installing the udev rule (USB access without root, MiniTiouner + PicoTuner)..."
# A separate rules FILE (not minitiouner.rules) so this never clobbers a
# rule file an existing stable install may already have in place - udev
# doesn't care which file a rule lives in.
sudo cp longmynd_ws/minitiouner.rules /etc/udev/rules.d/minitiouner-picotuner-test.rules
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=usb

step "🛠️  Building qo100_sdl (the receiver app)..."
cmake -S qo100_sdl -B qo100_sdl/build -DCMAKE_BUILD_TYPE=Release
cmake --build qo100_sdl/build --target qo100sdl -j"$(nproc)"

step "🖥️  Creating the desktop shortcut..."
mkdir -p "$HOME/Desktop"
RUN_SCRIPT="$INSTALL_DIR/scripts/run_picotuner_test.sh"
cat > "$RUN_SCRIPT" <<'RUNEOF'
#!/bin/bash
# Launches the PicoTuner test build and saves everything it prints to a
# timestamped log file, so a copy can be sent back after testing.
REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
LOG_DIR="$REPO_DIR/logs"
mkdir -p "$LOG_DIR"
LOG_FILE="$LOG_DIR/picotuner-test-$(date +%Y%m%d-%H%M%S).log"

BLUE='\033[1;34m'; GREEN='\033[1;32m'; YELLOW='\033[1;33m'; CYAN='\033[1;36m'; NC='\033[0m'

{
    printf "${CYAN}================================================================${NC}\n"
    printf "${CYAN}  QO-100 DATV Receiver - PicoTuner test session${NC}\n"
    printf "${CYAN}  Started: $(date)${NC}\n"
    printf "${CYAN}================================================================${NC}\n\n"

    cd "$REPO_DIR/qo100_sdl"
    QO100_WINDOWED=1 ./build/qo100sdl

    printf "\n${CYAN}================================================================${NC}\n"
    printf "${GREEN}Session ended: $(date)${NC}\n"
    printf "${CYAN}================================================================${NC}\n"
} 2>&1 | tee "$LOG_FILE"

printf "\n${YELLOW}This log was saved to:${NC}\n"
printf "  ${GREEN}%s${NC}\n" "$LOG_FILE"
printf "${YELLOW}Please attach this file when you report back how it went.${NC}\n\n"
read -p "Press ENTER to close this window..." _
RUNEOF
chmod +x "$RUN_SCRIPT"

DESKTOP_FILE="$HOME/Desktop/qo100datv-picotuner-test.desktop"
cat > "$DESKTOP_FILE" <<EOF
[Desktop Entry]
Name=QO-100 DATV (PicoTuner TEST)
Comment=Test build with PicoTuner support - not the normal receiver
Exec=lxterminal -e $RUN_SCRIPT
Path=$INSTALL_DIR/qo100_sdl
Icon=$INSTALL_DIR/assets/qo100datv-icon.png
Terminal=false
Type=Application
StartupNotify=false
Categories=AudioVideo;
EOF
chmod +x "$DESKTOP_FILE"

printf "\n${GREEN}✅ PicoTuner test build installed!${NC}\n\n"
echo "🔌 Plug the PicoTuner in now if it isn't already."
echo
echo "🖱️  Double-click the new \"QO-100 DATV (PicoTuner TEST)\" icon on the"
echo "   Desktop to run it. It opens in a terminal window so you can see"
echo "   what's happening, and saves a full log to:"
echo "     $INSTALL_DIR/logs/"
echo
echo "📧 After testing, please send that log file back - whatever happens"
echo "   (works, doesn't work, crashes), it's exactly what's needed to"
echo "   figure out the next step."
