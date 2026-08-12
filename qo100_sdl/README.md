# QO-100 SDL receiver

This is the non-LVGL successor to `qo100_lvgl`. It deliberately lives beside
the current application so the installed receiver remains available while the
new pipeline is developed and measured.

The first milestone provides:

- the same 1024x600 panel geometry, colours, Montserrat text and controls;
- one SDL accelerated renderer with vertical sync;
- a streaming RGB565 video texture (only a selected video frame is uploaded);
- a timestamp clock, six-frame bounded queue and controlled stale-frame drops;
- a synthetic 50 fps source for exercising the handoff independently of UDP
  and FFmpeg;
- a deterministic screenshot mode for visual regression checks.

It does not yet start Longmynd or receive/decode the live transport stream.
Those backends will be moved over after this presentation path is validated.

## Build

```sh
cmake -S qo100_sdl -B qo100_sdl/build -DCMAKE_BUILD_TYPE=Release
cmake --build qo100_sdl/build -j2
```

## Run

```sh
QO100_WINDOWED=1 qo100_sdl/build/qo100sdl --demo
```

For a headless visual check:

```sh
SDL_VIDEODRIVER=dummy qo100_sdl/build/qo100sdl \
  --demo --seconds 1 --screenshot /tmp/qo100-sdl.bmp
```
