# QO-100 SDL receiver

This is the current QO-100 DATV receiver application, superseding the earlier
LVGL-based `qo100_lvgl` (removed 2026-08-13).

The current milestone provides:

- the same 1024x600 panel geometry, colours, Montserrat text and controls;
- one SDL accelerated renderer with vertical sync;
- a streaming RGB565 video texture (only a selected video frame is uploaded);
- a timestamp clock, six-frame bounded queue and controlled stale-frame drops;
- the live BATC wideband FFT websocket feed, signal detection and selectable
  frequency marker;
- owned Longmynd startup/shutdown, live monitor/control websocket links,
  click-to-tune and receiver status/quality fields;
- direct FFmpeg UDP ingestion with an interruptible decoder, native YUV420
  handoff, timestamp scheduling and a persistent SDL YUV texture;
- audio decoded from the same transport stream and presented from an isolated,
  bounded SDL PCM ring buffer with controlled rebuffering;
- a synthetic 50 fps video source for exercising the handoff independently of
  UDP and FFmpeg;
- a deterministic screenshot mode for visual regression checks.

The Audio status shows the detected codec when the selected transport stream
contains a supported audio service.

## Build

```sh
cmake -S qo100_sdl -B qo100_sdl/build -DCMAKE_BUILD_TYPE=Release
cmake --build qo100_sdl/build -j2
```

## Run

```sh
QO100_WINDOWED=1 qo100_sdl/build/qo100sdl --demo
```

The spectrum is live by default. Add `--offline-spectrum` to use deterministic
test data without a network connection. Add `--no-tuner` for a renderer-only
test that does not start Longmynd or access the MiniTiouner.

For a headless visual check:

```sh
SDL_VIDEODRIVER=dummy qo100_sdl/build/qo100sdl \
  --demo --offline-spectrum --seconds 1 --screenshot /tmp/qo100-sdl.bmp
```
