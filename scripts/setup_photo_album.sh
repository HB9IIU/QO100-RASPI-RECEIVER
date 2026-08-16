#!/bin/bash
# Installs and enables a systemd --user service that serves the screenshot
# album (screenshots/album/) as a static website on the LAN.
#
# Usage: scripts/setup_photo_album.sh [PORT]
#   No argument: default port 8090.
#
# Safe to re-run: the service unit is always rewritten to match this script.
set -e

PORT="${1:-8090}"
REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
ALBUM_DIR="$REPO_DIR/screenshots/album"
SERVICE_DIR="$HOME/.config/systemd/user"
SERVICE_FILE="$SERVICE_DIR/qo100album.service"

mkdir -p "$ALBUM_DIR"
# So the page isn't empty before the first screenshot is ever taken.
"$REPO_DIR/scripts/album_update.sh"

mkdir -p "$SERVICE_DIR"
cat > "$SERVICE_FILE" <<EOF
[Unit]
Description=QO-100 DATV screenshot album (LAN web gallery)
After=network.target

[Service]
Type=simple
ExecStart=/usr/bin/python3 -m http.server $PORT --bind 0.0.0.0 --directory $ALBUM_DIR
Restart=on-failure
RestartSec=3

[Install]
WantedBy=default.target
EOF

systemctl --user daemon-reload
systemctl --user enable qo100album.service
systemctl --user restart qo100album.service

IP="$(hostname -I | awk '{print $1}')"
echo "Photo album service installed and running."
echo "Browse it at: http://$IP:$PORT/"
echo "To check status/logs: systemctl --user status qo100album.service"
