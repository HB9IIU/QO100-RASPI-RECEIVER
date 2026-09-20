#!/usr/bin/env bash
# Run on the target Linux architecture, with dependencies installed in its venv.
set -euo pipefail
project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$project_root"
interpreter=${1:-"$project_root/.venv/bin/python"}
output_name=${2:-"rtl-sdr-server-linux-$(uname -m)"}
"$interpreter" -m PyInstaller --noconfirm --onefile \
  --name "$output_name" --distpath "$project_root/dist" \
  --workpath "$project_root/tools/build" --specpath "$project_root/tools/build" \
  --add-data "$project_root/app/spectrum_calibration.json:app" \
  --paths "$project_root/tools" --hidden-import calibrate_beacon \
  --collect-all pyrtlsdrlib --exclude-module matplotlib --exclude-module PySide6 \
  "$project_root/main.py"
"$project_root/dist/$output_name" --self-test
