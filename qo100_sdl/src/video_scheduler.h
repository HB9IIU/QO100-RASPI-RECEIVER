#pragma once

#include "video_decoder.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <optional>

namespace qo100 {

using Clock = std::chrono::steady_clock;
using Microseconds = std::chrono::microseconds;

/* Sized to cover the audio output's own buffer (up to ~1s, see AudioOutput's
 * "buffer=250-1000ms") at up to 60fps: while the audio clock is following its own
 * buffer, video frames simply wait here for their moment - not a jitter buffer of
 * their own, just enough room not to drop frames that are still legitimately
 * ahead of what's currently playing. */
constexpr size_t kVideoQueueCapacity = 64;
constexpr size_t kVideoPrebufferFrames = 3;
constexpr int64_t kVideoPrebufferMaxUs = 150000;
constexpr int64_t kClockDiscontinuityUs = 2000000;
constexpr int64_t kClockRebaseLateUs = 250000;
/* The newest queued frame may be at most kClockMaxLagFrames frame intervals
 * (clamped to the two Us bounds) ahead of the presenter's clock; beyond that
 * the clock jumps to kClockTargetLagFrames intervals behind the newest. */
constexpr int64_t kClockMaxLagFrames = 5;
constexpr int64_t kClockTargetLagFrames = 3;
constexpr int64_t kClockMinMaxLagUs = 150000;
constexpr int64_t kClockMaxMaxLagUs = 600000;

using VideoFrame = qo100::VideoFrame;

class VideoScheduler {
public:
    void push(VideoFrame frame)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if(queue_.empty()) first_queued_at_ = Clock::now();
        if(have_last_pushed_) {
            const int64_t step_us = frame.pts_us - last_pushed_pts_us_;
            if(step_us > 0 && step_us < 1000000)
                step_ema_us_ = step_ema_us_ == 0 ? step_us : (step_ema_us_ * 9 + step_us) / 10;
            const double step_ms = (frame.pts_us - last_pushed_pts_us_) / 1000.0;
            window_pts_step_sum_ms_ += step_ms;
            window_pts_step_max_ms_ = std::max(window_pts_step_max_ms_, step_ms);
            ++window_pushed_;
        }
        last_pushed_pts_us_ = frame.pts_us;
        have_last_pushed_ = true;
        if(!queue_.empty() && frame.pts_us <= queue_.back().pts_us) {
            frame.pts_us = queue_.back().pts_us + 1;
        }
        if(queue_.size() >= kVideoQueueCapacity) {
            const bool space_available = space_available_.wait_for(lock,
                std::chrono::milliseconds(25), [this] {
                    return queue_.size() < kVideoQueueCapacity;
                });
            if(!space_available) {
                /* Keep the newest frames, not the oldest: dropping the incoming
                 * frame leaves holes in the timeline, and a queue of frames
                 * spaced far apart then plays back at a crawl. */
                queue_.pop_front();
                ++queue_drops_;
            }
        }
        queue_.push_back(std::move(frame));
    }

    /* audio_clock_us: the stream time of the sound being heard right now, when the
     * audio is playing. Then the picture simply follows it (lip sync, whatever the
     * audio buffer does). Without it - no audio, or the audio has stalled - the
     * frames are paced against the wall clock as before. */
    std::optional<VideoFrame> take_due(Clock::time_point now,
                                       std::optional<int64_t> audio_clock_us = std::nullopt)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++window_take_calls_;
        if(audio_clock_us && !queue_.empty() &&
           std::llabs(queue_.front().pts_us - *audio_clock_us) <= kClockDiscontinuityUs) {
            ++window_audio_clock_;
            if(queue_.front().pts_us > *audio_clock_us + 2000) {
                ++window_future_;
                window_future_lead_sum_ms_ += (queue_.front().pts_us - *audio_clock_us) / 1000.0;
                return std::nullopt;
            }
            VideoFrame selected = std::move(queue_.front());
            queue_.pop_front();
            while(!queue_.empty() && queue_.front().pts_us <= *audio_clock_us + 2000) {
                selected = std::move(queue_.front());
                queue_.pop_front();
                ++late_drops_;
            }
            ++presented_;
            space_available_.notify_one();
            return selected;
        }
        if(queue_.empty()) {
            ++window_empty_;
            return std::nullopt;
        }

        if(!clock_started_) {
            const int64_t wait_us = std::chrono::duration_cast<Microseconds>(
                now - first_queued_at_).count();
            if(queue_.size() < kVideoPrebufferFrames && wait_us < kVideoPrebufferMaxUs)
                return std::nullopt;
            anchor_pts_us_ = queue_.front().pts_us;
            anchor_wall_ = now;
            clock_started_ = true;
        }

        int64_t stream_now_us = anchor_pts_us_ +
            std::chrono::duration_cast<Microseconds>(now - anchor_wall_).count();
        const int64_t front_delta = queue_.front().pts_us - stream_now_us;
        if(std::llabs(front_delta) > kClockDiscontinuityUs ||
           stream_now_us - queue_.front().pts_us > kClockRebaseLateUs) {
            anchor_pts_us_ = queue_.front().pts_us;
            anchor_wall_ = now;
            stream_now_us = anchor_pts_us_;
            ++rebases_;
        }

        /* The decoder has run ahead of the clock (a burst at start-up, or a
         * long stall on this side): the newest queued frame is further in the
         * future than the queue can hold, so each new frame would push the
         * oldest out before it ever became due and nothing would be shown.
         * Jump the clock forward to keep a short, fixed lag behind the newest. */
        const int64_t newest_lead_us = queue_.back().pts_us - stream_now_us;
        const int64_t max_lag_us = std::clamp<int64_t>(
            kClockMaxLagFrames * step_ema_us_, kClockMinMaxLagUs, kClockMaxMaxLagUs);
        if(newest_lead_us > max_lag_us) {
            stream_now_us = queue_.back().pts_us -
                std::clamp<int64_t>(kClockTargetLagFrames * step_ema_us_, 60000, 250000);
            anchor_pts_us_ = stream_now_us;
            anchor_wall_ = now;
            ++rebases_;
        }

        if(queue_.front().pts_us > stream_now_us + 2000) {
            ++window_future_;
            window_future_lead_sum_ms_ += (queue_.front().pts_us - stream_now_us) / 1000.0;
            return std::nullopt;
        }

        VideoFrame selected = std::move(queue_.front());
        queue_.pop_front();
        while(!queue_.empty() && queue_.front().pts_us <= stream_now_us + 2000) {
            selected = std::move(queue_.front());
            queue_.pop_front();
            ++late_drops_;
        }
        ++presented_;
        space_available_.notify_one();
        return selected;
    }

    /* What the presenter saw since the last call, to tell "no frame was ready"
     * (empty), "the next frame isn't due yet" (future) and irregular
     * timestamps (pts step) apart when frames get dropped. */
    struct WindowStats {
        uint64_t take_calls = 0, empty = 0, future = 0, pushed = 0, audio_clock = 0;
        double avg_future_lead_ms = 0, avg_pts_step_ms = 0, max_pts_step_ms = 0;
    };

    WindowStats take_window_stats()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        WindowStats out;
        out.take_calls = window_take_calls_;
        out.empty = window_empty_;
        out.future = window_future_;
        out.pushed = window_pushed_;
        out.audio_clock = window_audio_clock_;
        out.avg_future_lead_ms = window_future_ ? window_future_lead_sum_ms_ / window_future_ : 0;
        out.avg_pts_step_ms = window_pushed_ ? window_pts_step_sum_ms_ / window_pushed_ : 0;
        out.max_pts_step_ms = window_pts_step_max_ms_;
        window_take_calls_ = window_empty_ = window_future_ = window_pushed_ = window_audio_clock_ = 0;
        window_future_lead_sum_ms_ = window_pts_step_sum_ms_ = window_pts_step_max_ms_ = 0;
        return out;
    }

    void reset()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        have_last_pushed_ = false;
        queue_.clear();
        clock_started_ = false;
        space_available_.notify_all();
    }

    struct Stats {
        uint64_t queue_drops;
        uint64_t late_drops;
        uint64_t presented;
        uint64_t rebases;
        size_t depth;
    };

    Stats stats() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return {queue_drops_, late_drops_, presented_, rebases_, queue_.size()};
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable space_available_;
    std::deque<VideoFrame> queue_;
    Clock::time_point first_queued_at_{};
    Clock::time_point anchor_wall_{};
    int64_t anchor_pts_us_ = 0;
    bool clock_started_ = false;
    uint64_t queue_drops_ = 0;
    uint64_t late_drops_ = 0;
    uint64_t presented_ = 0;
    uint64_t rebases_ = 0;
    bool have_last_pushed_ = false;
    int64_t step_ema_us_ = 0;   /* typical frame interval of the stream, from the pts steps */
    int64_t last_pushed_pts_us_ = 0;
    uint64_t window_take_calls_ = 0, window_empty_ = 0, window_future_ = 0, window_pushed_ = 0,
             window_audio_clock_ = 0;
    double window_future_lead_sum_ms_ = 0, window_pts_step_sum_ms_ = 0, window_pts_step_max_ms_ = 0;
};

} // namespace qo100
