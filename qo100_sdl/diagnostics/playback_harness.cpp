/* Diagnostic harness: the app's real decoder and presenter without a screen.
 *
 * Reads the MPEG-TS the decoder is pointed at (QO100_TS_ADDR / QO100_TS_PORT),
 * pushes every decoded frame into the VideoScheduler exactly as main() does,
 * and polls the presenter at 60Hz like the render loop. Prints one RESULT line.
 * Driven by playback_test.sh; see README.md.
 *
 *   playback_harness <seconds> <name> <stream_fps> */
#include "video_decoder.h"
#include "video_scheduler.h"

#include <sys/resource.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using namespace qo100;

int main(int argc, char ** argv)
{
    const int seconds = argc > 1 ? std::atoi(argv[1]) : 16;
    const char * name = argc > 2 ? argv[2] : "stream";
    const double stream_fps = argc > 3 ? std::atof(argv[3]) : 0.0;

    VideoScheduler scheduler;
    std::atomic<int> width{0}, height{0};
    VideoDecoder decoder([&](VideoFrame && frame) {
        width = frame.width;
        height = frame.height;
        scheduler.push(std::move(frame));
    });

    const auto start = Clock::now();
    decoder.start();
    std::vector<int> shown(static_cast<size_t>(seconds) + 1, 0);
    double first_frame_ms = -1.0;
    while(Clock::now() - start < std::chrono::seconds(seconds)) {
        const auto now = Clock::now();
        if(scheduler.take_due(now)) {
            if(first_frame_ms < 0)
                first_frame_ms = std::chrono::duration<double, std::milli>(now - start).count();
            const auto second = std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
            ++shown[static_cast<size_t>(second)];
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    decoder.stop();

    /* Steady state = after the first 6 seconds (probing, start-up burst). */
    const int from = 6;
    int steady = 0;
    for(int i = from; i < seconds; ++i) steady += shown[static_cast<size_t>(i)];
    const double steady_fps = seconds > from ? static_cast<double>(steady) / (seconds - from) : 0.0;

    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    const double cpu_seconds = usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6 +
                               usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6;
    const auto stats = scheduler.stats();

    std::printf("PER_SECOND %s:", name);
    for(int i = 0; i < seconds; ++i) std::printf(" %d", shown[static_cast<size_t>(i)]);
    std::printf("\n");
    std::printf("RESULT name=%s size=%dx%d stream_fps=%.1f steady_fps=%.1f first_frame_ms=%.0f "
                "decoded=%llu late_drops=%llu queue_drops=%llu rebases=%llu "
                "decode_errors=%llu cpu_percent=%.0f\n",
                name, width.load(), height.load(), stream_fps, steady_fps, first_frame_ms,
                static_cast<unsigned long long>(decoder.decoded_frames()),
                static_cast<unsigned long long>(stats.late_drops),
                static_cast<unsigned long long>(stats.queue_drops),
                static_cast<unsigned long long>(stats.rebases),
                static_cast<unsigned long long>(decoder.decode_errors()),
                cpu_seconds / seconds * 100.0);
    return 0;
}
