#!/bin/bash
set -e
cd "$(dirname "$0")"

mkdir -p build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target qo100sdl -j"$(nproc)"

DISPLAY="${DISPLAY:-:0.0}" ./build/qo100sdl
