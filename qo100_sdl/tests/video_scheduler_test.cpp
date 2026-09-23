/* Simulates the start-up burst that leaves the presenter crawling: the decoder
 * delivers ~2s of a 30fps stream at once, then live frames in real time, while
 * the presenter polls at 60Hz. Prints the shown fps per second and exits
 * non-zero if the steady state is not close to the stream rate.
 *   g++ -std=c++17 -O2 -Isrc tests/video_scheduler_test.cpp -o /tmp/sched_test -lpthread */
#include "../src/video_scheduler.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

using namespace qo100;

static int run(int fps)
{
    const int64_t kStepUs = 1000000 / fps;
    const int kBurstFrames = 2 * fps;
    constexpr int kSeconds = 12;
    VideoScheduler scheduler;
    std::atomic<bool> done{false};
    const auto start = Clock::now();

    std::thread decoder([&] {
        int64_t index = 0;
        auto push_frame = [&] {
            VideoFrame frame;
            frame.width = 2;
            frame.height = 2;
            frame.pts_us = index * kStepUs;
            ++index;
            scheduler.push(std::move(frame));
        };
        std::this_thread::sleep_until(start + std::chrono::seconds(2));   /* probing */
        for(int i = 0; i < kBurstFrames; ++i) push_frame();
        auto next = Clock::now();
        while(!done.load()) {
            next += Microseconds(kStepUs);
            std::this_thread::sleep_until(next);
            push_frame();
        }
    });

    std::vector<int> shown_per_second(kSeconds + 1, 0);
    uint64_t rebases_at_6s = 0;
    while(Clock::now() - start < std::chrono::seconds(kSeconds)) {
        const auto now = Clock::now();
        if(rebases_at_6s == 0 && now - start >= std::chrono::seconds(6))
            rebases_at_6s = scheduler.stats().rebases + 1;
        if(scheduler.take_due(now)) {
            const int second = static_cast<int>(
                std::chrono::duration_cast<std::chrono::seconds>(now - start).count());
            ++shown_per_second[second];
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    done = true;
    decoder.join();

    for(int i = 0; i < kSeconds; ++i) std::printf("second %2d: %d fps\n", i, shown_per_second[i]);
    const auto stats = scheduler.stats();
    std::printf("queue_drops=%llu late_drops=%llu rebases=%llu\n",
                static_cast<unsigned long long>(stats.queue_drops),
                static_cast<unsigned long long>(stats.late_drops),
                static_cast<unsigned long long>(stats.rebases));
    int steady = 0;
    for(int i = 8; i < 12; ++i) steady += shown_per_second[i];
    std::printf("stream %d fps: steady state (seconds 8-11): %.1f fps, rebases after second 6: %llu\n\n", fps,
                steady / 4.0, static_cast<unsigned long long>(stats.rebases + 1 - rebases_at_6s));
    return steady / 4.0 >= fps * 0.85 ? 0 : 1;
}

/* Lip sync: the picture must follow the audio clock however the audio buffer
 * misbehaves. The sound plays at real-time speed, except that every 3 s it
 * stalls for 400 ms (an underrun: the buffer is refilled before playing on) and
 * later skips 400 ms ahead (chunks dropped because the buffer overflowed), so the
 * delay between decoding and hearing swings by 400 ms, like the log of a bursty
 * station. Live frames carry pts = the wall time they were decoded at. Every
 * shown frame's pts is compared with the audio clock at that moment. */
static int run_audio_sync(int fps)
{
    const int64_t step_us = 1000000 / fps;
    VideoScheduler scheduler;
    std::atomic<bool> done{false};
    const auto start = Clock::now();
    const auto since_start_us = [&](Clock::time_point t) {
        return std::chrono::duration_cast<Microseconds>(t - start).count();
    };
    std::thread decoder([&] {
        auto next = start + std::chrono::seconds(1);   /* probing, then a burst */
        std::this_thread::sleep_until(next);
        while(!done.load()) {
            VideoFrame frame;
            frame.width = 2;
            frame.height = 2;
            frame.pts_us = since_start_us(Clock::now());   /* live: pts follows wall time */
            scheduler.push(std::move(frame));
            next += Microseconds(step_us);
            std::this_thread::sleep_until(next);
        }
    });
    std::vector<int64_t> errors_us;
    int shown = 0;
    while(Clock::now() - start < std::chrono::seconds(12)) {
        const auto now = Clock::now();
        const int64_t t_us = since_start_us(now);
        /* the audio clock: 200 ms behind real time to begin with; stalls for
         * 400 ms from 1.5 s into every 3 s cycle, and skips those 400 ms back at
         * the end of the cycle. */
        const int64_t cycle_us = t_us % 3000000;
        int64_t audio_clock_us = t_us - 200000;
        if(cycle_us >= 1500000 && cycle_us < 1900000) audio_clock_us -= cycle_us - 1500000;
        else if(cycle_us >= 1900000) audio_clock_us -= 400000;
        if(auto frame = scheduler.take_due(now, audio_clock_us)) {
            if(t_us > 4000000) {
                errors_us.push_back(std::llabs(audio_clock_us - frame->pts_us));
                ++shown;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    done = true;
    decoder.join();
    std::sort(errors_us.begin(), errors_us.end());
    const auto percentile = [&](double p) {
        return errors_us.empty() ? -1 : errors_us[static_cast<size_t>(p * (errors_us.size() - 1))];
    };
    const double shown_fps = shown / 8.0;
    std::printf("audio sync, %d fps, audio stalling 400ms and skipping 400ms: median error %.0fms, "
                "95th %.0fms, shown %.1f fps\n", fps, percentile(0.5) / 1000.0,
                percentile(0.95) / 1000.0, shown_fps);
    return (percentile(0.5) <= 60000 && percentile(0.95) <= 150000 && shown_fps >= fps * 0.7)
        ? 0 : 1;
}

int main()
{
    int failed = 0;
    for(int fps : {25, 30, 50, 60}) failed += run(fps);
    failed += run_audio_sync(30);
    failed += run_audio_sync(50);
    std::printf(failed == 0 ? "PASS\n" : "FAIL: %d scenario(s)\n", failed);
    return failed == 0 ? 0 : 1;
}
