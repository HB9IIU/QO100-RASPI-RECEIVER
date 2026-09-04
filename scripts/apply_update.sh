#!/bin/bash
# Applies an update: pulls the latest commit and rebuilds both binaries.
# Deliberately does NOT touch apt, udev, sysctl, or reboot - none of that
# is needed for a routine code update, and none of it needs sudo, which
# matters here: this is what the app itself runs in the background when
# you tap YES on its own "update available" popup, with no keyboard and no
# terminal for sudo to ever prompt through.
#
# If a future update ever needs a new apt dependency or a udev rule
# change, that still needs a real, manual run of scripts/initialSetup.sh
# (which does need sudo, and does need you present to type a password) -
# see the "Updating" section in README.md.
set -e

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_DIR"

echo "Pulling the latest version..."
git pull --ff-only

echo "Building longmynd_ws (the tuner driver)..."
(cd longmynd_ws && make clean && make)

echo "Building qo100_sdl (the receiver app)..."
cmake -S qo100_sdl -B qo100_sdl/build -DCMAKE_BUILD_TYPE=Release
cmake --build qo100_sdl/build --target qo100sdl -j"$(nproc)"

echo "Update applied."
