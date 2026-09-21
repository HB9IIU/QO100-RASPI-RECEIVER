/* Simulates the start-up burst that leaves the presenter crawling: the decoder
 * delivers ~2s of a 30fps stream at once, then live frames in real time, while
 * the presenter polls at 60Hz. Prints the shown fps per second and exits
 * non-zero if the steady state is not close to the stream rate.
 *   g++ -std=c++17 -O2 -Isrc tests/video_scheduler_test.cpp -o /tmp/sched_test -lpthread */
#include "../src/video_scheduler.h"

#include <atomic>
#include <random>
#include <cstdio>
#include <thread>
#include <vector>

using namespace qo100;

static int run(int fps, int burst_size = 1, int jitter_ms = 0)
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
        std::mt19937 random(1234);
        std::uniform_int_distribution<int> jitter(0, std::max(jitter_ms, 1));
        auto last_arrival = Clock::now();
        while(!done.load() && jitter_ms > 0) {
            /* Network-like jitter: each frame is late by up to jitter_ms, but
             * frames still arrive in order. */
            next += Microseconds(kStepUs);
            auto arrival = std::max(next + std::chrono::milliseconds(jitter(random)), last_arrival);
            last_arrival = arrival;
            std::this_thread::sleep_until(arrival);
            push_frame();
        }
        while(!done.load()) {
            /* burst_size > 1: frames arrive in groups, like a decoder that
             * delivers several at once after a hiccup. */
            next += Microseconds(kStepUs * burst_size);
            std::this_thread::sleep_until(next);
            for(int i = 0; i < burst_size; ++i) push_frame();
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
    std::printf("stream %d fps, bursts of %d, jitter %dms: steady state (seconds 8-11): %.1f fps, rebases after second 6: %llu, late_drops=%llu underruns=%llu\n\n", fps, burst_size, jitter_ms,
                steady / 4.0, static_cast<unsigned long long>(stats.rebases + 1 - rebases_at_6s),
                static_cast<unsigned long long>(stats.late_drops), static_cast<unsigned long long>(stats.underruns));
    return steady / 4.0 >= fps * 0.85 ? 0 : 1;
}

int main()
{
    int failed = 0;
    for(int fps : {25, 30, 50, 60}) failed += run(fps);
    failed += run(30, 4);   /* bursty delivery */
    failed += run(25, 3);
    failed += run(50, 1, 120);   /* jittery arrival */
    failed += run(30, 1, 150);
    std::printf(failed == 0 ? "PASS\n" : "FAIL: %d scenario(s)\n", failed);
    return failed == 0 ? 0 : 1;
}
