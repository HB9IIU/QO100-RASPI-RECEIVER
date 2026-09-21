# Diagnostics

Tools for finding out why video is not smooth on somebody's Pi, without needing a
live DATV signal, a tuner, or a screen.

## playback_test.sh - can this Pi decode and present video smoothly?

```
cd qo100_sdl/diagnostics
./playback_test.sh              # all scenarios, about 2 minutes
./playback_test.sh --quick      # three scenarios
./playback_test.sh --only hevc_720p30_hw
./playback_test.sh --list
```

It generates short synthetic streams (test pattern with noise, video plus MP2 audio,
a keyframe every 2 s, 20 s long so nothing loops during the 16 s run) with `ffmpeg`, sends each one in real time over loopback
(`127.0.0.1:5610`, never the app's own multicast address) into the app's **real**
`VideoDecoder` and `VideoScheduler`, and polls the presenter at 60 Hz like the render
loop does. It then reports the frame rate actually shown against the stream's own.

Nothing is installed and the project is not modified. Generated streams and the built
harness are cached in `$TMPDIR/qo100-diagnostics` (override with `QO100_DIAG_DIR`).
Stop the app first if you can: it competes for the CPU and skews the numbers.

### Reading the summary

| column | meaning |
|---|---|
| stream / shown | the stream's frame rate / frames per second the presenter showed after the first 6 s |
| pct | shown as % of stream. **PASS >= 90, WARN 75-90, FAIL < 75** |
| first_ms | time to the first shown frame (about 2 s is normal: the decoder probes the stream first) |
| late | frames skipped because they were already late when shown |
| qdrop | frames dropped because the queue overflowed (should be 0) |
| rebs | times the presenter's clock jumped (a few during start-up is normal) |
| under | buffer underruns: the queue ran empty and was rebuilt (a visible freeze each) |
| cpu% | CPU used by decoder + harness, as % of one core |

The scenarios:

| name | what it exercises |
|---|---|
| `h264_720p50` | like the QO-100 beacon: H.264 1280x720 at 50 fps |
| `h264_1080p25` | 1080p H.264 in software, the heaviest common case |
| `h264_576p30` | typical amateur 30 fps stream |
| `hevc_720p30_hw` | HEVC with the Pi 5 hardware decoder |
| `hevc_720p30_sw` | the same stream forced to software (`QO100_HW_DECODE=0`) |
| `hevc_540p25_hw` | HEVC at 25 fps |

### What a result tells you

- **All PASS:** decoding and presenting are fine on this Pi. Look elsewhere: the
  signal itself (bad MER, packet loss: see `[FFMPEG]` and `late`/`under` in the app
  log), other programs using the CPU (`load1` and `app_cpu` in the app's `[SYS]`
  line), or the display path (VNC, X forwarding).
- **`hevc_*_hw` FAILs but `hevc_*_sw` passes:** the hardware decoder is not working
  here. The app falls back to software by itself, but check `/dev/video19`, the
  kernel/firmware version, and whether `ffmpeg -hwaccel drm` works.
- **Everything FAILs, cpu% high:** the Pi is too busy or too slow (thermal
  throttling: `vcgencmd get_throttled` should say `0x0`).
- **Only 1080p FAILs:** software H.264 1080p is at the edge of what a Pi 5 can do
  while other work is running.
- **`qdrop` or `under` non-zero:** a presenter/buffering problem; send both outputs.

### What to ask a user who reports choppy video

1. The output of `./playback_test.sh` (stop the app first).
2. The app log from the run in question: `qo100_sdl/logs/latest.log`. The lines that
   matter are `[VIDEO]` (fps, drops, late, underruns), `[SCHED]` (why frames were
   skipped), `[SYS]` (`app_cpu`, `load1`, temperature) and `[FFMPEG]` (stream errors).

## playback_harness.cpp

The program `playback_test.sh` builds. It links `src/video_decoder.cpp` and uses
`src/video_scheduler.h`, so it always tests the code the app really runs.

## ../tests/video_scheduler_test.cpp

A separate, deterministic simulation of the presenter alone (start-up bursts, bursty
and jittery arrival at 25/30/50/60 fps). Build line at the top of the file.
