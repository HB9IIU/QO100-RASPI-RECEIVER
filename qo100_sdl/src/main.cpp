#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <libwebsockets.h>
#ifndef LWS_PROTOCOL_LIST_TERM
#define LWS_PROTOCOL_LIST_TERM {nullptr, nullptr, 0, 0, 0, nullptr, 0}
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <ctime>
#include <fstream>

#include <limits.h>
#include <dirent.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include "receiver.h"
#include "app_log.h"
#include "chat_client.h"
#include "video_decoder.h"

namespace {

using Clock = std::chrono::steady_clock;
using Microseconds = std::chrono::microseconds;

constexpr int kReferenceWidth = 1024;
constexpr int kReferenceHeight = 600;
constexpr size_t kVideoQueueCapacity = 6;
constexpr size_t kVideoPrebufferFrames = 3;
constexpr int64_t kVideoPrebufferMaxUs = 150000;
constexpr int64_t kClockDiscontinuityUs = 2000000;
constexpr int64_t kClockRebaseLateUs = 250000;

struct Colour {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a = 255;
};

constexpr Colour kBackground{0x10, 0x14, 0x1c};
constexpr Colour kPanel{0x18, 0x1e, 0x2a};
constexpr Colour kBorder{0x2a, 0x33, 0x44};
constexpr Colour kText{0xd8, 0xde, 0xe9};
constexpr Colour kTextDim{0x70, 0x78, 0x88};
constexpr Colour kCyan{0x39, 0xd6, 0xff};
constexpr Colour kGreen{0x35, 0xd4, 0x6a};
constexpr Colour kYellow{0xff, 0xd1, 0x66};
constexpr Colour kRed{0xff, 0x4d, 0x4d};
constexpr Colour kPurple{0xb3, 0x66, 0xff};

void set_colour(SDL_Renderer * renderer, Colour colour)
{
    SDL_SetRenderDrawColor(renderer, colour.r, colour.g, colour.b, colour.a);
}

/* Tracks where a touch/click is currently held down, so buttons can draw a
 * pressed highlight the instant a finger lands on them rather than only
 * reacting on release. */
struct TouchState {
    bool active = false;
    int x = 0;
    int y = 0;
};

bool point_in_rect(int x, int y, const SDL_Rect & rect)
{
    return x >= rect.x && x < rect.x + rect.w &&
           y >= rect.y && y < rect.y + rect.h;
}

bool is_pressed(const TouchState & touch, const SDL_Rect & rect)
{
    return touch.active && point_in_rect(touch.x, touch.y, rect);
}

uint16_t rgb565(uint8_t red, uint8_t green, uint8_t blue)
{
    return static_cast<uint16_t>(((red & 0xf8U) << 8U) |
                                 ((green & 0xfcU) << 3U) |
                                 (blue >> 3U));
}

std::string executable_directory()
{
    char path[PATH_MAX]{};
    const ssize_t length = readlink("/proc/self/exe", path, sizeof(path) - 1U);
    if(length <= 0) return ".";
    path[length] = '\0';
    std::string result(path);
    const size_t slash = result.find_last_of('/');
    return slash == std::string::npos ? "." : result.substr(0, slash);
}

std::string repository_directory()
{
    const std::string candidate = executable_directory() + "/../..";
    char resolved[PATH_MAX]{};
    return realpath(candidate.c_str(), resolved) != nullptr ? resolved : candidate;
}

/* ~/.config/autostart/qo100datv.desktop is the actual boot-time trigger
 * (see scripts/setup_autostart.sh) - its mere presence/absence on disk *is*
 * the "auto start at boot" setting, so there's nothing to load from
 * settings.json or keep in sync: just check the file. */
std::string autostart_desktop_file_path()
{
    const char * home = std::getenv("HOME");
    return std::string(home ? home : "") + "/.config/autostart/qo100datv.desktop";
}

bool autostart_enabled()
{
    return access(autostart_desktop_file_path().c_str(), F_OK) == 0;
}

std::string detect_tuner_product_string()
{
    DIR * directory = opendir("/sys/bus/usb/devices");
    if(directory == nullptr) return {};

    std::string result;
    while(const dirent * entry = readdir(directory)) {
        const std::string path =
            std::string("/sys/bus/usb/devices/") + entry->d_name;
        char vendor[8]{};
        FILE * vendor_file = std::fopen((path + "/idVendor").c_str(), "r");
        if(vendor_file == nullptr) continue;
        const bool have_vendor =
            std::fgets(vendor, sizeof(vendor), vendor_file) != nullptr;
        std::fclose(vendor_file);
        if(!have_vendor || std::strncmp(vendor, "0403", 4) != 0) continue;

        char product_id[8]{};
        FILE * product_id_file =
            std::fopen((path + "/idProduct").c_str(), "r");
        if(product_id_file == nullptr) continue;
        const bool have_product_id =
            std::fgets(product_id, sizeof(product_id), product_id_file) != nullptr;
        std::fclose(product_id_file);
        if(!have_product_id || std::strncmp(product_id, "6010", 4) != 0) continue;

        FILE * product_file = std::fopen((path + "/product").c_str(), "r");
        if(product_file == nullptr) continue;
        char product[128]{};
        if(std::fgets(product, sizeof(product), product_file) != nullptr) {
            result = product;
            while(!result.empty() &&
                  (result.back() == '\n' || result.back() == '\r'))
                result.pop_back();
        }
        std::fclose(product_file);
        break;
    }
    closedir(directory);
    return result;
}

struct DisplayConfig {
    int width = kReferenceWidth;
    int height = kReferenceHeight;
    bool fullscreen = true;
    /* The real physical screen size, independent of what was actually
     * chosen above - a hard ceiling nothing should ever render past. Only
     * meaningful when QO100_DISPLAY isn't overriding things (see below);
     * defaults to the 1024x600 reference on the assumption that whatever
     * SDL can't measure is at least that big, matching this struct's
     * existing width/height defaults. */
    int native_width = kReferenceWidth;
    int native_height = kReferenceHeight;
};

/* Requires SDL video already initialised (SDL_Init(SDL_INIT_VIDEO) must run
 * before this). Resolution priority:
 *   1. QO100_DISPLAY env var, if set (e.g. a systemd unit pinned to a
 *      genuinely different physical panel via setup_autostart.sh WxH) -
 *      an explicit installer override, trusted completely and exempted
 *      from the native-size ceiling below.
 *   2. The saved Display Resolution choice from settings.json, if that file
 *      already exists - this is what makes the SET page's toggle actually
 *      stick regardless of how the app is launched (systemd service via
 *      service_launch.sh, the desktop icon via build_and_run.sh, or by
 *      hand); relying on service_launch.sh alone to set QO100_DISPLAY left
 *      the other launch paths silently ignoring the saved choice and
 *      falling back to auto-detect, which could render a layout too big
 *      for the real screen. Clamped to the smaller preset (800x480) if the
 *      saved choice doesn't actually fit the real screen - e.g. a stale or
 *      hand-edited settings.json requesting 1024x600 on a genuine 800x480
 *      panel, which otherwise renders the SET button itself off-screen and
 *      unreachable (this happened in practice). The SET page also disables
 *      picking a size that doesn't fit in the first place; this is the
 *      backstop for however a bad value ends up saved anyway.
 *   3. Auto-detected physical screen size, when settings.json doesn't exist
 *      yet (genuine first run) - otherwise a Pi with the smaller 800x480
 *      panel would open a too-big window before the user ever gets a
 *      chance to fix it from SET. Falls back to the 1024x600 reference
 *      size only if SDL can't report a desktop mode at all. */
DisplayConfig resolve_display_config(bool screenshot_mode, bool settings_file_exists,
                                      bool saved_800x480)
{
    DisplayConfig config;
    if(const char * value = std::getenv("QO100_DISPLAY")) {
        int width = 0;
        int height = 0;
        if(std::sscanf(value, "%dx%d", &width, &height) == 2 && width > 0 && height > 0) {
            config.width = width;
            config.height = height;
            config.native_width = width;
            config.native_height = height;
        }
    }
    else {
        SDL_DisplayMode mode{};
        const bool detected =
            SDL_GetDesktopDisplayMode(0, &mode) == 0 && mode.w > 0 && mode.h > 0;
        if(detected) {
            config.native_width = mode.w;
            config.native_height = mode.h;
        }
        if(settings_file_exists) {
            const int desired_width = saved_800x480 ? 800 : kReferenceWidth;
            const int desired_height = saved_800x480 ? 480 : kReferenceHeight;
            const bool fits = desired_width <= config.native_width &&
                              desired_height <= config.native_height;
            config.width = fits ? desired_width : 800;
            config.height = fits ? desired_height : 480;
        }
        else if(detected) {
            config.width = mode.w;
            config.height = mode.h;
        }
    }
    if(std::getenv("QO100_WINDOWED") != nullptr || screenshot_mode) config.fullscreen = false;
    return config;
}

class AudioOutput {
public:
    ~AudioOutput() { close(); }

    bool open(int volume_percent)
    {
        volume_ = std::clamp(volume_percent, 0, 100);
        SDL_AudioSpec requested{};
        requested.freq = kSampleRate;
        requested.format = AUDIO_S16SYS;
        requested.channels = kChannels;
        requested.samples = 1024;
        requested.callback = &AudioOutput::callback;
        requested.userdata = this;
        SDL_AudioSpec obtained{};
        device_ = SDL_OpenAudioDevice(nullptr, 0, &requested, &obtained, 0);
        if(device_ == 0) {
            qo100::log("[AUDIO] output unavailable: %s\n", SDL_GetError());
            return false;
        }
        bytes_per_second_ = obtained.freq * obtained.channels *
                            static_cast<int>(sizeof(int16_t));
        capacity_ = static_cast<size_t>(bytes_per_second_);
        prebuffer_bytes_ = static_cast<size_t>(bytes_per_second_) / 4U;
        ring_.resize(capacity_);
        SDL_PauseAudioDevice(device_, 0);
        qo100::log(
            "[AUDIO] output driver=%s rate=%dHz channels=%u buffer=250-1000ms\n",
            SDL_GetCurrentAudioDriver() != nullptr
                ? SDL_GetCurrentAudioDriver() : "unknown",
            obtained.freq, obtained.channels);
        return true;
    }

    /* Returns false if SDL_CloseAudioDevice had to be abandoned still
     * running - the caller must not call anything that touches SDL's audio
     * subsystem again in that case (notably SDL_Quit()), since the
     * abandoned close is still in there, forever, holding a lock that would
     * be needed - see the comment below. */
    bool close()
    {
        if(device_ == 0) return true;
        const SDL_AudioDeviceID device = device_;
        device_ = 0;
        SDL_PauseAudioDevice(device, 1);

        /* SDL_CloseAudioDevice can hang indefinitely against some
         * PulseAudio/PipeWire-pulse servers (seen on Raspberry Pi OS
         * Trixie's pipewire-pulse) waiting for a teardown ack that never
         * arrives. EXIT must never freeze the whole app on that - and it
         * did, taking the systemd service down with it since a hung stop
         * gets SIGKILLed and isn't restarted. Give it a bounded window on
         * its own thread and abandon it if it doesn't return.
         *
         * Abandoning it is NOT fully harmless, though: the detached thread
         * stays stuck inside SDL's audio code forever, holding SDL's
         * internal audio-subsystem lock. If the process later calls
         * SDL_Quit() (which tears down every SDL subsystem, including
         * audio) on the main thread as normal, that call blocks forever
         * trying to acquire the very same lock - trading the original
         * EXIT-hangs-forever bug for an equally permanent hang a few lines
         * later, just without systemd's stop-timeout around to eventually
         * SIGKILL it (this happened in practice: EXIT with "Restart"
         * selected doesn't call `systemctl stop` at all, so nothing was
         * left to force-kill the now-deadlocked process, and it silently
         * never restarted). The caller is responsible for skipping
         * SDL_Quit() and exiting immediately instead when this returns
         * false. */
        auto closed = std::make_shared<std::atomic<bool>>(false);
        std::thread closer([device, closed] {
            SDL_CloseAudioDevice(device);
            *closed = true;
        });
        closer.detach();
        for(int attempt = 0; attempt < 30 && !closed->load(); ++attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if(closed->load()) return true;
        qo100::log("[AUDIO] SDL_CloseAudioDevice did not return in time; abandoning\n");
        return false;
    }

    void reset()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        read_ = 0;
        write_ = 0;
        size_ = 0;
        buffering_ = true;
        ++resets_;
    }

    void set_volume(int percent) { volume_ = std::clamp(percent, 0, 100); }

    void push(qo100::AudioChunk && chunk)
    {
        if(device_ == 0 || chunk.pcm_s16.empty() ||
           chunk.sample_rate != kSampleRate || chunk.channels != kChannels) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if(chunk.pcm_s16.size() > capacity_ - size_) {
            ++dropped_chunks_;
            return;
        }
        const size_t first = std::min(chunk.pcm_s16.size(), capacity_ - write_);
        std::memcpy(ring_.data() + write_, chunk.pcm_s16.data(), first);
        const size_t remaining = chunk.pcm_s16.size() - first;
        if(remaining > 0)
            std::memcpy(ring_.data(), chunk.pcm_s16.data() + first, remaining);
        write_ = (write_ + chunk.pcm_s16.size()) % capacity_;
        size_ += chunk.pcm_s16.size();
    }

    uint32_t queued_ms() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return bytes_per_second_ > 0
            ? static_cast<uint32_t>(size_ * 1000U /
                                    static_cast<size_t>(bytes_per_second_))
            : 0;
    }
    uint64_t dropped_chunks() const { return dropped_chunks_.load(); }
    uint64_t underruns() const { return underruns_.load(); }
    uint64_t rebuffers() const { return rebuffers_.load(); }
    uint64_t resets() const { return resets_.load(); }
    /* Peak of the last decoded audio buffer, measured before volume scaling
     * (silence during underrun/buffering) - 0-100. */
    int peak_percent() const { return peak_percent_.load(std::memory_order_relaxed); }

private:
    static constexpr int kSampleRate = 48000;
    static constexpr int kChannels = 2;

    static void callback(void * userdata, Uint8 * stream, int length)
    {
        static_cast<AudioOutput *>(userdata)->fill(stream, length);
    }

    void update_peak(const Uint8 * stream, int length)
    {
        const auto * samples = reinterpret_cast<const int16_t *>(stream);
        const size_t count = static_cast<size_t>(length) / sizeof(int16_t);
        int peak = 0;
        for(size_t i = 0; i < count; ++i)
            peak = std::max(peak, std::abs(static_cast<int>(samples[i])));
        peak_percent_.store(std::min(100, peak * 100 / 32767), std::memory_order_relaxed);
    }

    void fill(Uint8 * stream, int length)
    {
        std::memset(stream, 0, static_cast<size_t>(length));
        if(length <= 0) return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(buffering_) {
                if(size_ < prebuffer_bytes_) { update_peak(stream, length); return; }
                buffering_ = false;
                ++rebuffers_;
            }
            const size_t requested = static_cast<size_t>(length);
            if(size_ < requested) {
                read_ = write_;
                size_ = 0;
                buffering_ = true;
                ++underruns_;
                update_peak(stream, length);
                return;
            }
            const size_t first = std::min(requested, capacity_ - read_);
            std::memcpy(stream, ring_.data() + read_, first);
            if(requested > first)
                std::memcpy(stream + first, ring_.data(), requested - first);
            read_ = (read_ + requested) % capacity_;
            size_ -= requested;
        }

        /* Measured before volume scaling - reflects the actual decoded
         * signal, not "how loud you've chosen to listen". */
        update_peak(stream, length);

        const int volume = volume_.load(std::memory_order_relaxed);
        if(volume < 100) {
            auto * samples = reinterpret_cast<int16_t *>(stream);
            const size_t count = static_cast<size_t>(length) / sizeof(int16_t);
            for(size_t i = 0; i < count; ++i)
                samples[i] = static_cast<int16_t>(static_cast<int>(samples[i]) * volume / 100);
        }
    }

    SDL_AudioDeviceID device_ = 0;
    int bytes_per_second_ = 0;
    size_t capacity_ = 0;
    size_t prebuffer_bytes_ = 0;
    mutable std::mutex mutex_;
    std::vector<uint8_t> ring_;
    size_t read_ = 0;
    size_t write_ = 0;
    size_t size_ = 0;
    bool buffering_ = true;
    std::atomic<int> volume_{100};
    std::atomic<uint64_t> dropped_chunks_{0};
    std::atomic<uint64_t> underruns_{0};
    std::atomic<uint64_t> rebuffers_{0};
    std::atomic<uint64_t> resets_{0};
    std::atomic<int> peak_percent_{0};
};

class TextCache {
public:
    explicit TextCache(SDL_Renderer * renderer) : renderer_(renderer) {}

    ~TextCache()
    {
        clear();
    }

    void clear()
    {
        for(auto & entry : textures_) SDL_DestroyTexture(entry.second.texture);
        for(auto & entry : fonts_) TTF_CloseFont(entry.second);
        textures_.clear();
        lru_.clear();
        fonts_.clear();
    }

    bool load_font(const std::string & path, int size)
    {
        if(fonts_.count(size) != 0) return true;
        TTF_Font * font = TTF_OpenFont(path.c_str(), size);
        if(font == nullptr) {
            qo100::log( "[FONT] cannot open %s at %dpx: %s\n",
                         path.c_str(), size, TTF_GetError());
            return false;
        }
        fonts_[size] = font;
        return true;
    }

    void draw(const std::string & text, int x, int y, Colour colour, int size = 14,
              bool centred = false)
    {
        const std::string key = std::to_string(size) + ":" +
            std::to_string(colour.r) + ":" + std::to_string(colour.g) + ":" +
            std::to_string(colour.b) + ":" + text;
        auto found = textures_.find(key);
        if(found == textures_.end()) {
            auto font = fonts_.find(size);
            if(font == fonts_.end()) return;
            const SDL_Color sdl_colour{colour.r, colour.g, colour.b, colour.a};
            SDL_Surface * surface = TTF_RenderUTF8_Blended(font->second, text.c_str(), sdl_colour);
            if(surface == nullptr) return;
            SDL_Texture * texture = SDL_CreateTextureFromSurface(renderer_, surface);
            const int width = surface->w;
            const int height = surface->h;
            SDL_FreeSurface(surface);
            if(texture == nullptr) return;
            lru_.push_front(key);
            CachedText cached{texture, width, height, lru_.begin()};
            found = textures_.emplace(key, cached).first;
            evict_if_needed();
        }
        else if(found->second.lru_it != lru_.begin()) {
            lru_.erase(found->second.lru_it);
            lru_.push_front(key);
            found->second.lru_it = lru_.begin();
        }

        SDL_Rect destination{x, y, found->second.width, found->second.height};
        if(centred) {
            destination.x -= destination.w / 2;
            destination.y -= destination.h / 2;
        }
        SDL_RenderCopy(renderer_, found->second.texture, nullptr, &destination);
    }

    std::pair<int, int> measure(const std::string & text, int size = 14) const
    {
        const auto font = fonts_.find(size);
        if(font == fonts_.end()) return {0, 0};
        int width = 0;
        int height = 0;
        if(TTF_SizeUTF8(font->second, text.c_str(), &width, &height) != 0) return {0, 0};
        return {width, height};
    }

    size_t texture_count() const { return textures_.size(); }

private:
    struct CachedText {
        SDL_Texture * texture = nullptr;
        int width = 0;
        int height = 0;
        std::list<std::string>::iterator lru_it;
    };

    /* Most (text, colour, size) combinations recur every frame (labels,
     * status values, chat timestamps), but some are one-off or slowly
     * drift (elapsed-time counters, frequency readouts while tuning) - an
     * unbounded cache leaks a texture per unique string forever on those.
     * Cap it and evict the least-recently-used entry once full. */
    static constexpr size_t kMaxCachedTextures = 512;

    void evict_if_needed()
    {
        while(textures_.size() > kMaxCachedTextures) {
            const auto found = textures_.find(lru_.back());
            if(found != textures_.end()) {
                SDL_DestroyTexture(found->second.texture);
                textures_.erase(found);
            }
            lru_.pop_back();
        }
    }

    SDL_Renderer * renderer_ = nullptr;
    std::unordered_map<int, TTF_Font *> fonts_;
    std::unordered_map<std::string, CachedText> textures_;
    std::list<std::string> lru_;
};

using VideoFrame = qo100::VideoFrame;

class VideoScheduler {
public:
    void push(VideoFrame frame)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if(queue_.empty()) first_queued_at_ = Clock::now();
        if(!queue_.empty() && frame.pts_us <= queue_.back().pts_us) {
            frame.pts_us = queue_.back().pts_us + 1;
        }
        if(queue_.size() >= kVideoQueueCapacity) {
            const bool space_available = space_available_.wait_for(lock,
                std::chrono::milliseconds(25), [this] {
                    return queue_.size() < kVideoQueueCapacity;
                });
            if(!space_available) {
                ++queue_drops_;
                return;
            }
        }
        queue_.push_back(std::move(frame));
    }

    std::optional<VideoFrame> take_due(Clock::time_point now)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(queue_.empty()) return std::nullopt;

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

        if(queue_.front().pts_us > stream_now_us + 2000) return std::nullopt;

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

    void reset()
    {
        std::lock_guard<std::mutex> lock(mutex_);
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
};

/* The dB range (0..kDisplayMaxDb) the spectrum plot's colour/height mapping
 * is tuned against - referenced by Layout below to compute extra headroom
 * for the compact (800x480) plot without changing this base scale. */
constexpr float kDisplayMaxDb = 10.0F;

struct Layout {
    int width;
    int height;
    int spectrum_height;
    int bottom_y;
    int bottom_height;
    int video_width;
    /* 800x480 has much less spare vertical space than 1024x600 - the
     * frequency axis labels below the spectrum plot (see draw_spectrum())
     * are skipped at this size and their reserved strip handed to the plot
     * itself instead. spectrum_max_db raises the ceiling by exactly enough
     * to keep the existing dB-per-pixel density unchanged (so today's
     * signals render pixel-identical, not stretched) - the extra pixel rows
     * become new headroom above the old 0..kDisplayMaxDb ceiling, mainly so
     * a strong signal's label has somewhere to sit without being clipped
     * or shoved aside. */
    bool compact;
    float spectrum_max_db;
    SDL_Rect spectrum_panel;
    SDL_Rect spectrum_plot;
    SDL_Rect video_panel;
    SDL_Rect video_content;
    SDL_Rect status_panel;

    explicit Layout(int w, int h)
        : width(w), height(h), spectrum_height(h * 230 / 480),
          bottom_y(4 + spectrum_height + 4), bottom_height(h - bottom_y - 4),
          video_width(w * 420 / 800),
          compact(w <= 800),
          spectrum_max_db(compact
              ? kDisplayMaxDb * static_cast<float>(spectrum_height - 22 - 1) /
                    static_cast<float>(spectrum_height - 36 - 1)
              : kDisplayMaxDb),
          spectrum_panel{4, 4, w - 8, spectrum_height},
          spectrum_plot{18, 18, w - 36,
                       spectrum_height - (compact ? 22 : 36)},
          video_panel{4, bottom_y, video_width, bottom_height},
          video_content{18, bottom_y + 14, video_width - 28, bottom_height - 28},
          status_panel{4 + video_width + 4, bottom_y,
                       w - (4 + video_width + 4) - 4, bottom_height}
    {}
};

void fill_panel(SDL_Renderer * renderer, const SDL_Rect & rect)
{
    set_colour(renderer, kPanel);
    SDL_RenderFillRect(renderer, &rect);
    set_colour(renderer, kBorder);
    SDL_RenderDrawRect(renderer, &rect);
}

constexpr double kSpectrumStartMhz = 10490.5;
constexpr double kSpectrumSpanMhz = 9.0;
constexpr float kServerUnitsPerDb = 3276.8F;
constexpr float kDisplayZeroOffsetDb = 20.0F / 6.0F;

uint8_t mix_channel(uint8_t start, uint8_t end, float amount)
{
    return static_cast<uint8_t>(start + (static_cast<float>(end) - start) * amount);
}

Colour mix_colour(Colour start, Colour end, float amount)
{
    return {mix_channel(start.r, end.r, amount),
            mix_channel(start.g, end.g, amount),
            mix_channel(start.b, end.b, amount)};
}

Colour spectrum_gradient(float db)
{
    db = std::clamp(db, 0.0F, kDisplayMaxDb);
    if(db < 2.0F) return mix_colour({0, 18, 105}, {0, 145, 220}, db / 2.0F);
    if(db < 4.0F) return mix_colour({0, 145, 220}, {0, 190, 55}, (db - 2.0F) / 2.0F);
    if(db < 5.5F) return mix_colour({0, 190, 55}, {245, 225, 0}, (db - 4.0F) / 1.5F);
    if(db < 7.0F) return mix_colour({245, 225, 0}, {255, 82, 0}, (db - 5.5F) / 1.5F);
    if(db < 8.5F) return mix_colour({255, 82, 0}, {245, 0, 35}, (db - 7.0F) / 1.5F);
    return mix_colour({245, 0, 35}, {255, 35, 155}, (db - 8.5F) / 1.5F);
}

struct DetectedSignal {
    size_t start_bin = 0;
    size_t end_bin = 0;
    float middle_bin = 0.0F;
    float strength = 0.0F;
    float measured_width_mhz = 0.0F;
    float symbol_rate_ms = 0.0F;
    double frequency_mhz = 0.0;
};

float align_symbol_rate(float width_mhz)
{
    if(width_mhz < 0.022F) return 0.0F;
    if(width_mhz < 0.060F) return 0.035F;
    if(width_mhz < 0.086F) return 0.066F;
    if(width_mhz < 0.185F) return 0.125F;
    if(width_mhz < 0.277F) return 0.250F;
    if(width_mhz < 0.388F) return 0.333F;
    if(width_mhz < 0.700F) return 0.500F;
    if(width_mhz < 1.200F) return 1.000F;
    if(width_mhz < 1.600F) return 1.500F;
    if(width_mhz < 2.200F) return 2.000F;
    return std::round(width_mhz * 5.0F) / 5.0F;
}

std::vector<DetectedSignal> detect_signals(const std::vector<uint16_t> & bins)
{
    constexpr float noise_level = 11000.0F;
    constexpr float signal_threshold = 16000.0F;
    std::vector<DetectedSignal> detected;
    if(bins.size() < 3) return detected;

    bool in_signal = false;
    size_t initial_start = 0;
    for(size_t index = 2; index < bins.size(); ++index) {
        const float average = (bins[index] + bins[index - 1] + bins[index - 2]) / 3.0F;
        if(!in_signal && average > signal_threshold) {
            in_signal = true;
            initial_start = index;
            continue;
        }
        if(!in_signal || average >= signal_threshold) continue;
        in_signal = false;
        const size_t initial_end = index;
        if(initial_end <= initial_start + 2) continue;

        const size_t middle_start = initial_start +
            static_cast<size_t>(0.3F * (initial_end - initial_start));
        const size_t middle_end = initial_start +
            static_cast<size_t>(0.7F * (initial_end - initial_start));
        if(middle_end <= middle_start) continue;
        uint64_t sum = 0;
        for(size_t bin = middle_start; bin < middle_end; ++bin) sum += bins[bin];
        const float strength = static_cast<float>(sum) / (middle_end - middle_start);
        const float edge_level = noise_level + 0.75F * (strength - noise_level);

        size_t refined_start = initial_start;
        while(refined_start < initial_end && bins[refined_start] < edge_level) ++refined_start;
        size_t refined_end = std::min(initial_end, bins.size() - 1);
        while(refined_end > refined_start && bins[refined_end] < edge_level) --refined_end;
        if(refined_end <= refined_start) continue;

        const float middle_bin = refined_start + (refined_end - refined_start) / 2.0F;
        const float width_mhz = (refined_end - refined_start) *
            static_cast<float>(kSpectrumSpanMhz / bins.size());
        const float symbol_rate_ms = align_symbol_rate(width_mhz);
        if(symbol_rate_ms == 0.0F) continue;
        const double frequency_mhz = kSpectrumStartMhz +
            ((middle_bin + 1.0) / bins.size()) * kSpectrumSpanMhz;
        detected.push_back({refined_start, refined_end, middle_bin, strength,
                            width_mhz, symbol_rate_ms, frequency_mhz});
    }
    return detected;
}

enum class SpectrumStatus { Connecting, Waiting, Live, ConnectionError, Disconnected };

const char * spectrum_status_text(SpectrumStatus status)
{
    switch(status) {
        case SpectrumStatus::Connecting: return "CONNECTING...";
        case SpectrumStatus::Waiting: return "WAITING FOR FFT...";
        case SpectrumStatus::Live: return "";
        case SpectrumStatus::ConnectionError: return "CONNECTION ERROR - RETRYING...";
        case SpectrumStatus::Disconnected: return "DISCONNECTED - RETRYING...";
    }
    return "";
}

class SpectrumFeed {
public:
    ~SpectrumFeed() { stop(); }

    void start()
    {
        if(running_.exchange(true)) return;
        thread_ = std::thread([this] { run(); });
    }

    void stop()
    {
        if(!running_.exchange(false)) return;
        if(thread_.joinable()) thread_.join();
    }

    bool consume(std::vector<uint16_t> & bins, SpectrumStatus & status)
    {
        std::vector<uint8_t> message;
        {
            std::lock_guard<std::mutex> lock(handoff_mutex_);
            message.swap(pending_message_);
            status = pending_status_;
        }
        if(message.empty() || message.size() % 2U != 0U) return false;
        bins.resize(message.size() / 2U);
        for(size_t index = 0; index < bins.size(); ++index) {
            bins[index] = static_cast<uint16_t>(message[index * 2U]) |
                static_cast<uint16_t>(message[index * 2U + 1U] << 8U);
        }
        return true;
    }

    uint64_t received_frames() const { return received_frames_.load(); }
    uint64_t replaced_frames() const { return replaced_frames_.load(); }

private:
    static int callback(lws * websocket, lws_callback_reasons reason,
                        void *, void * data, size_t length)
    {
        auto * self = static_cast<SpectrumFeed *>(
            lws_context_user(lws_get_context(websocket)));
        return self != nullptr ? self->on_event(websocket, reason, data, length) : 0;
    }

    int on_event(lws * websocket, lws_callback_reasons reason, void * data, size_t length)
    {
        switch(reason) {
            case LWS_CALLBACK_CLIENT_ESTABLISHED:
                websocket_ = websocket;
                set_status(SpectrumStatus::Waiting);
                qo100::log( "[SPECTRUM] connected to BATC\n");
                break;
            case LWS_CALLBACK_CLIENT_RECEIVE: {
                if(lws_is_first_fragment(websocket)) {
                    message_buffer_.clear();
                    message_is_binary_ = lws_frame_is_binary(websocket) != 0;
                }
                const auto * bytes = static_cast<const uint8_t *>(data);
                message_buffer_.insert(message_buffer_.end(), bytes, bytes + length);
                if(lws_is_final_fragment(websocket) &&
                   lws_remaining_packet_payload(websocket) == 0U) {
                    if(message_is_binary_) {
                        std::lock_guard<std::mutex> lock(handoff_mutex_);
                        if(!pending_message_.empty()) ++replaced_frames_;
                        pending_message_ = message_buffer_;
                        pending_status_ = SpectrumStatus::Live;
                        ++received_frames_;
                    }
                    message_buffer_.clear();
                }
                break;
            }
            case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
                websocket_ = nullptr;
                set_status(SpectrumStatus::ConnectionError);
                qo100::log( "[SPECTRUM] connection error: %.*s\n",
                    static_cast<int>(length), data != nullptr ? static_cast<const char *>(data) : "");
                break;
            case LWS_CALLBACK_CLIENT_CLOSED:
                websocket_ = nullptr;
                set_status(SpectrumStatus::Disconnected);
                qo100::log( "[SPECTRUM] disconnected\n");
                break;
            default:
                break;
        }
        return 0;
    }

    void set_status(SpectrumStatus status)
    {
        std::lock_guard<std::mutex> lock(handoff_mutex_);
        pending_status_ = status;
    }

    void connect(lws_context * context, const char * protocol_name)
    {
        lws_client_connect_info info{};
        info.context = context;
        info.address = "eshail.batc.org.uk";
        info.port = 443;
        info.path = "/wb/fft";
        info.host = info.address;
        info.origin = "https://eshail.batc.org.uk";
        info.ssl_connection = LCCSCF_USE_SSL;
        info.local_protocol_name = protocol_name;
        info.protocol = protocol_name;
        info.alpn = "http/1.1";
        websocket_ = lws_client_connect_via_info(&info);
    }

    void run()
    {
        lws_set_log_level(LLL_ERR | LLL_WARN, nullptr);
        static const lws_protocols protocols[] = {
            {"fft_m0dtslivetune", &SpectrumFeed::callback, 0, 64 * 1024, 0, nullptr, 0},
            LWS_PROTOCOL_LIST_TERM
        };
        lws_context_creation_info info{};
        info.port = CONTEXT_PORT_NO_LISTEN;
        info.protocols = protocols;
        /* This app also creates a separate lws_context in chat_client.cpp
         * (ChatClient), which sets this same flag. Each context genuinely
         * needs it on its own to make outgoing SSL connections work at
         * all - confirmed by testing: dropping it from one context broke
         * every SSL connect attempt from that context outright
         * ("SSL_new failed"), even well after the other context's global
         * init had already completed, so this isn't just a one-time
         * process-wide init that any single context can do on everyone
         * else's behalf. See the lws_context_destroy() comment below for
         * the other half of this - both contexts need the flag, but only
         * one of them can safely act on it at destroy time. */
        info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
        info.user = this;
        lws_context * context = lws_create_context(&info);
        if(context == nullptr) {
            set_status(SpectrumStatus::ConnectionError);
            return;
        }

        connect(context, protocols[0].name);
        auto last_attempt = Clock::now();
        while(running_.load(std::memory_order_relaxed)) {
            lws_service(context, 50);
            const auto now = Clock::now();
            if(websocket_ == nullptr && now - last_attempt > std::chrono::seconds(3)) {
                last_attempt = now;
                set_status(SpectrumStatus::Connecting);
                connect(context, protocols[0].name);
            }
        }
        /* Deliberately NOT calling lws_context_destroy() here. Two
         * lws_contexts that both set LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT
         * (this one and ChatClient's, both alive in this same process)
         * isn't safely refcounted across contexts in this libwebsockets
         * version: destroying the second one re-releases global SSL state
         * the first one's destroy already fully released, tripping an
         * internal assertion (lwsl_refcount_cx) and SIGABRT - confirmed
         * with gdb during EXIT's shutdown, where ChatClient::stop() runs
         * first (see main()'s shutdown sequence) and always destroys
         * cleanly, and this context is always the second, always-crashing
         * one. Skipping the explicit destroy here just leaks this
         * lws_context until the process exits a moment later either way -
         * which is exactly what "the app is exiting, don't bother cleaning
         * up further" already means in a few other places in this
         * codebase (see AudioOutput::close()). */
        websocket_ = nullptr;
    }

    std::atomic<bool> running_{false};
    std::thread thread_;
    lws * websocket_ = nullptr;
    std::vector<uint8_t> message_buffer_;
    bool message_is_binary_ = false;
    mutable std::mutex handoff_mutex_;
    std::vector<uint8_t> pending_message_;
    SpectrumStatus pending_status_ = SpectrumStatus::Connecting;
    std::atomic<uint64_t> received_frames_{0};
    std::atomic<uint64_t> replaced_frames_{0};
};

std::vector<uint16_t> make_offline_spectrum(size_t count)
{
    std::vector<uint16_t> bins(count);
    const auto plateau = [](double x, double left, double right, double edge) {
        const double rise = 1.0 / (1.0 + std::exp(-(x - left) / edge));
        const double fall = 1.0 / (1.0 + std::exp((x - right) / edge));
        return rise * fall;
    };
    for(size_t index = 0; index < count; ++index) {
        const double fraction = static_cast<double>(index) / std::max<size_t>(1, count - 1);
        const double frequency = kSpectrumStartMhz + kSpectrumSpanMhz * fraction;
        const double beacon = 25000.0 * plateau(frequency, 10490.72, 10492.25, 0.04);
        const double signal = 20000.0 * plateau(frequency, 10497.10, 10497.45, 0.018);
        const double noise = 10800.0 + 300.0 * std::sin(frequency * 44.0) +
                             180.0 * std::sin(frequency * 117.0);
        bins[index] = static_cast<uint16_t>(std::clamp(noise + beacon + signal, 0.0, 65535.0));
    }
    return bins;
}

class SpectrumTexture {
public:
    SpectrumTexture(SDL_Renderer * renderer, int width, int height, float max_db)
        : renderer_(renderer), width_(width), height_(height), max_db_(max_db),
          pixels_(static_cast<size_t>(width) * height)
    {
        texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGB565,
                                     SDL_TEXTUREACCESS_STREAMING, width_, height_);
        if(texture_ != nullptr) SDL_SetTextureScaleMode(texture_, SDL_ScaleModeLinear);
    }

    ~SpectrumTexture() { SDL_DestroyTexture(texture_); }

    bool valid() const { return texture_ != nullptr; }
    const std::vector<DetectedSignal> & signals() const { return signals_; }

    void update(const std::vector<uint16_t> & bins)
    {
        if(texture_ == nullptr || bins.empty()) return;
        const uint16_t black = rgb565(0, 0, 0);
        const uint16_t grid = rgb565(45, 56, 66);
        const uint16_t trace = rgb565(255, 225, 235);
        std::fill(pixels_.begin(), pixels_.end(), black);
        for(int division = 0; division <= 2; ++division) {
            const int y = division * (height_ - 1) / 2;
            for(int x = 0; x < width_; ++x)
                if((x % 8) < 4) pixels_[static_cast<size_t>(y) * width_ + x] = grid;
        }
        for(int division = 1; division <= 9; ++division) {
            const int x = static_cast<int>(((division - 0.5) / 9.0) * (width_ - 1));
            for(int y = 0; y < height_; ++y)
                if((y % 8) < 4) pixels_[static_cast<size_t>(y) * width_ + x] = grid;
        }

        std::vector<uint16_t> palette(height_);
        for(int y = 0; y < height_; ++y) {
            /* Divide by max_db_ (may exceed kDisplayMaxDb - see Layout) so
             * the existing 0..kDisplayMaxDb gradient keeps the same pixel
             * density as before; any extra rows above that (real headroom,
             * compact layout only) all land past spectrum_gradient()'s own
             * clamp and just repeat its top colour - never actually
             * distinguishable, real signals don't reach that high. */
            const float db = static_cast<float>(height_ - 1 - y) /
                             std::max(1, height_ - 1) * max_db_;
            const Colour colour = spectrum_gradient(db);
            palette[y] = rgb565(colour.r, colour.g, colour.b);
        }
        for(int x = 0; x < width_; ++x) {
            const float position = static_cast<float>(x) * bins.size() / width_;
            const size_t first = std::min(static_cast<size_t>(position), bins.size() - 1);
            const size_t second = std::min(first + 1, bins.size() - 1);
            const float fraction = position - static_cast<float>(first);
            const float server_value = bins[first] + fraction * (bins[second] - bins[first]);
            const float displayed_db = server_value / kServerUnitsPerDb - kDisplayZeroOffsetDb;
            /* Clamp stays at the original kDisplayMaxDb - that's the real
             * measured-signal ceiling, unrelated to plot size. Dividing by
             * max_db_ instead of kDisplayMaxDb is what keeps this pixel
             * height identical to the non-compact layout while leaving the
             * extra rows empty above it as headroom. */
            const float limited_db = std::clamp(displayed_db, 0.0F, kDisplayMaxDb);
            const int height = static_cast<int>(limited_db / max_db_ * (height_ - 1));
            const int top = height_ - 1 - height;
            for(int y = top + 1; y < height_; ++y)
                pixels_[static_cast<size_t>(y) * width_ + x] = palette[y];
            pixels_[static_cast<size_t>(top) * width_ + x] = trace;
        }
        SDL_UpdateTexture(texture_, nullptr, pixels_.data(), width_ * sizeof(uint16_t));
        signals_ = detect_signals(bins);
    }

    void draw(const SDL_Rect & destination) const
    {
        if(texture_ != nullptr) SDL_RenderCopy(renderer_, texture_, nullptr, &destination);
    }

private:
    SDL_Renderer * renderer_ = nullptr;
    SDL_Texture * texture_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    float max_db_ = kDisplayMaxDb;
    std::vector<uint16_t> pixels_;
    std::vector<DetectedSignal> signals_;
};

enum class SpectrumMarkerKind {
    None, Tune, TuneQueued, AlreadyTuned, NoSignal, SpectrumNotReady
};

enum class VideoNotice {
    None, WaitingForTuner, Tuning, NoVideoStream
};

struct SpectrumMarker {
    SpectrumMarkerKind kind = SpectrumMarkerKind::None;
    double frequency_mhz = 0.0;
    Clock::time_point expires_at{};
};

void draw_spectrum(SDL_Renderer * renderer, TextCache & text, const Layout & layout,
                   const SpectrumTexture & texture, SpectrumStatus status,
                   const SpectrumMarker & marker,
                   const qo100::ReceiverStatus & receiver,
                   double tuned_frequency_mhz)
{
    fill_panel(renderer, layout.spectrum_panel);
    set_colour(renderer, {0, 0, 0});
    SDL_RenderFillRect(renderer, &layout.spectrum_plot);
    texture.draw(layout.spectrum_plot);

    float beacon_strength = 0.0F;
    for(const auto & signal : texture.signals()) {
        if(signal.frequency_mhz < 10492.0 && signal.symbol_rate_ms >= 1.0F) {
            beacon_strength = signal.strength;
            break;
        }
    }
    struct LabelArea { int x; int y; int width; int height; };
    const auto overlaps = [](const LabelArea & first, const LabelArea & second) {
        constexpr int gap = 4;
        return first.x < second.x + second.width + gap &&
               first.x + first.width + gap > second.x &&
               first.y < second.y + second.height + gap &&
               first.y + first.height + gap > second.y;
    };
    std::vector<LabelArea> occupied;
    size_t label_count = 0;
    const std::string tuned_service = !receiver.service_name.empty()
        ? receiver.service_name : receiver.service_provider;
    for(const auto & signal : texture.signals()) {
        const bool beacon = signal.frequency_mhz < 10492.0 && signal.symbol_rate_ms >= 1.0F;
        const double half_width_mhz = std::max(
            0.02, static_cast<double>(signal.measured_width_mhz) / 2.0);
        const bool tuned_signal = receiver.locked() && !tuned_service.empty() &&
            std::abs(signal.frequency_mhz - tuned_frequency_mhz) <= half_width_mhz;
        if(beacon || label_count++ >= 16) continue;
        char first_line[48];
        if(signal.symbol_rate_ms < 0.7F)
            std::snprintf(first_line, sizeof(first_line), "%.0fKS %.3f",
                          signal.symbol_rate_ms * 1000.0F, signal.frequency_mhz);
        else
            std::snprintf(first_line, sizeof(first_line), "%.1fMS %.3f",
                          signal.symbol_rate_ms, signal.frequency_mhz);
        char second_line[32];
        if(beacon_strength > 0.0F)
            std::snprintf(second_line, sizeof(second_line), "%+.2f dB BCN",
                          (signal.strength - beacon_strength) / kServerUnitsPerDb);
        else
            std::snprintf(second_line, sizeof(second_line), "-- dB BCN");

        const double fraction = (signal.frequency_mhz - kSpectrumStartMhz) / kSpectrumSpanMhz;
        const int centre_x = layout.spectrum_plot.x +
            static_cast<int>(fraction * layout.spectrum_plot.w);
        const float displayed_db = signal.strength / kServerUnitsPerDb - kDisplayZeroOffsetDb;
        const int trace_y = layout.spectrum_plot.y + layout.spectrum_plot.h - 1 -
            static_cast<int>(std::clamp(displayed_db, 0.0F, kDisplayMaxDb) /
                             layout.spectrum_max_db * (layout.spectrum_plot.h - 1));
        const auto [first_width, line_height] = text.measure(first_line);
        const auto [second_width, ignored_height] = text.measure(second_line);
        (void)ignored_height;
        const int service_width = tuned_signal ? text.measure(tuned_service).first : 0;
        const int label_width = std::max({first_width, second_width, service_width});
        const int label_height = line_height * (tuned_signal ? 3 : 2);
        const int natural_x = std::clamp(centre_x - label_width / 2,
            layout.spectrum_plot.x + 2,
            layout.spectrum_plot.x + layout.spectrum_plot.w - label_width - 2);
        const int natural_y = std::clamp(trace_y - label_height - 5,
            layout.spectrum_plot.y + 4,
            layout.spectrum_plot.y + layout.spectrum_plot.h - label_height - 3);

        LabelArea chosen{natural_x, natural_y, label_width, label_height};
        const int row = label_height + 5;
        const int y_offsets[] = {0, -row, row, -2 * row, 2 * row};
        const int x_offsets[] = {0, -label_width / 2, label_width / 2,
                                  -label_width, label_width};
        bool found_position = false;
        for(int x_offset : x_offsets) {
            for(int y_offset : y_offsets) {
                LabelArea candidate{
                    std::clamp(natural_x + x_offset,
                        layout.spectrum_plot.x + 2,
                        layout.spectrum_plot.x + layout.spectrum_plot.w - label_width - 2),
                    std::clamp(natural_y + y_offset,
                        layout.spectrum_plot.y + 4,
                        layout.spectrum_plot.y + layout.spectrum_plot.h - label_height - 3),
                    label_width, label_height
                };
                bool collision = false;
                for(const LabelArea & used : occupied) {
                    if(overlaps(candidate, used)) {
                        collision = true;
                        break;
                    }
                }
                if(!collision) {
                    chosen = candidate;
                    found_position = true;
                    break;
                }
            }
            if(found_position) break;
        }
        occupied.push_back(chosen);
        int text_y = chosen.y;
        if(tuned_signal) {
            text.draw(tuned_service, chosen.x + (label_width - service_width) / 2,
                      text_y, kGreen);
            text_y += line_height;
        }
        text.draw(first_line, chosen.x + (label_width - first_width) / 2,
                  text_y, kText);
        text.draw(second_line, chosen.x + (label_width - second_width) / 2,
                  text_y + line_height, kText);
    }

    if(marker.kind != SpectrumMarkerKind::None &&
       marker.frequency_mhz >= kSpectrumStartMhz &&
       marker.frequency_mhz <= kSpectrumStartMhz + kSpectrumSpanMhz) {
        const int marker_x = layout.spectrum_plot.x + static_cast<int>(
            (marker.frequency_mhz - kSpectrumStartMhz) / kSpectrumSpanMhz *
            layout.spectrum_plot.w);
        const Colour marker_colour = marker.kind == SpectrumMarkerKind::Tune
            ? kCyan : (marker.kind == SpectrumMarkerKind::TuneQueued
                ? kYellow : (marker.kind == SpectrumMarkerKind::AlreadyTuned
                    ? kGreen : (marker.kind == SpectrumMarkerKind::SpectrumNotReady
                        ? kYellow : kTextDim)));
        set_colour(renderer, marker_colour);
        SDL_RenderDrawLine(renderer, marker_x, layout.spectrum_plot.y + 18,
                           marker_x, layout.spectrum_plot.y + layout.spectrum_plot.h - 1);
        char marker_text[32];
        if(marker.kind == SpectrumMarkerKind::Tune)
            std::snprintf(marker_text, sizeof(marker_text), "%.3f", marker.frequency_mhz);
        else if(marker.kind == SpectrumMarkerKind::TuneQueued)
            std::snprintf(marker_text, sizeof(marker_text), "TUNE QUEUED");
        else if(marker.kind == SpectrumMarkerKind::AlreadyTuned)
            std::snprintf(marker_text, sizeof(marker_text), "ALREADY TUNED");
        else if(marker.kind == SpectrumMarkerKind::NoSignal)
            std::snprintf(marker_text, sizeof(marker_text), "NO SIGNAL");
        else
            std::snprintf(marker_text, sizeof(marker_text), "SPECTRUM NOT READY");
        const auto [marker_width, marker_height] = text.measure(marker_text);
        (void)marker_height;
        const int marker_label_x = std::clamp(marker_x - marker_width / 2,
            layout.spectrum_plot.x + 2,
            layout.spectrum_plot.x + layout.spectrum_plot.w - marker_width - 2);
        text.draw(marker_text, marker_label_x, layout.spectrum_plot.y - 3,
                  marker_colour);
    }

    if(status != SpectrumStatus::Live) {
        const Colour status_colour = status == SpectrumStatus::ConnectionError ? kRed : kYellow;
        text.draw(spectrum_status_text(status), layout.spectrum_plot.x + 8,
                  layout.spectrum_plot.y + 8, status_colour);
    }
    if(!layout.compact) {
        for(int number = 1; number <= 8; ++number) {
            const int x = layout.spectrum_plot.x + number * layout.spectrum_plot.w / 9;
            text.draw(std::to_string(10490 + number), x,
                      layout.spectrum_plot.y + layout.spectrum_plot.h + 12,
                      kTextDim, 14, true);
        }
    }
}

void fill_rounded_rect(SDL_Renderer * renderer, const SDL_Rect & rect,
                       int radius, Colour colour)
{
    radius = std::min(radius, std::min(rect.w, rect.h) / 2);
    set_colour(renderer, colour);
    const SDL_Rect middle{rect.x + radius, rect.y, rect.w - 2 * radius, rect.h};
    SDL_RenderFillRect(renderer, &middle);
    const SDL_Rect left{rect.x, rect.y + radius, radius, rect.h - 2 * radius};
    SDL_RenderFillRect(renderer, &left);
    const SDL_Rect right{
        rect.x + rect.w - radius, rect.y + radius, radius, rect.h - 2 * radius};
    SDL_RenderFillRect(renderer, &right);
    for(int y = 0; y < radius; ++y) {
        const int j = radius - y;
        const int half = static_cast<int>(
            std::sqrt(std::max(0, radius * radius - j * j)));
        if(half <= 0) continue;
        const SDL_Rect top_left{rect.x + radius - half, rect.y + y, half, 1};
        SDL_RenderFillRect(renderer, &top_left);
        const SDL_Rect top_right{rect.x + rect.w - radius, rect.y + y, half, 1};
        SDL_RenderFillRect(renderer, &top_right);
        const SDL_Rect bottom_left{
            rect.x + radius - half, rect.y + rect.h - 1 - y, half, 1};
        SDL_RenderFillRect(renderer, &bottom_left);
        const SDL_Rect bottom_right{
            rect.x + rect.w - radius, rect.y + rect.h - 1 - y, half, 1};
        SDL_RenderFillRect(renderer, &bottom_right);
    }
}

void draw_rounded_rect(SDL_Renderer * renderer, const SDL_Rect & rect,
                       int radius, Colour colour)
{
    radius = std::min(radius, std::min(rect.w, rect.h) / 2);
    set_colour(renderer, colour);
    SDL_RenderDrawLine(renderer, rect.x + radius, rect.y,
                       rect.x + rect.w - radius - 1, rect.y);
    SDL_RenderDrawLine(renderer, rect.x + radius, rect.y + rect.h - 1,
                       rect.x + rect.w - radius - 1, rect.y + rect.h - 1);
    SDL_RenderDrawLine(renderer, rect.x, rect.y + radius,
                       rect.x, rect.y + rect.h - radius - 1);
    SDL_RenderDrawLine(renderer, rect.x + rect.w - 1, rect.y + radius,
                       rect.x + rect.w - 1, rect.y + rect.h - radius - 1);
    for(int angle = 0; angle <= 90; ++angle) {
        const double rad = angle * M_PI / 180.0;
        const int dx = static_cast<int>(std::round(radius * std::cos(rad)));
        const int dy = static_cast<int>(std::round(radius * std::sin(rad)));
        SDL_RenderDrawPoint(renderer, rect.x + radius - dx, rect.y + radius - dy);
        SDL_RenderDrawPoint(renderer, rect.x + rect.w - 1 - radius + dx,
                            rect.y + radius - dy);
        SDL_RenderDrawPoint(renderer, rect.x + radius - dx,
                            rect.y + rect.h - 1 - radius + dy);
        SDL_RenderDrawPoint(renderer, rect.x + rect.w - 1 - radius + dx,
                            rect.y + rect.h - 1 - radius + dy);
    }
}

void draw_button(SDL_Renderer * renderer, TextCache & text, SDL_Rect rect,
                 const std::string & label, Colour colour, int font_size = 16,
                 bool active = false, bool pressed = false)
{
    constexpr int kButtonRadius = 10;
    fill_rounded_rect(renderer, rect, kButtonRadius, active ? colour : kPanel);
    draw_rounded_rect(renderer, rect, kButtonRadius, colour);
    text.draw(label, rect.x + rect.w / 2, rect.y + rect.h / 2,
              active ? kBackground : colour, font_size, true);
    /* A translucent wash on top of whatever's already drawn, rather than
     * a distinct pressed colour per button - works the same regardless of
     * the button's own colour or active/outline state. */
    if(pressed) fill_rounded_rect(renderer, rect, kButtonRadius, {255, 255, 255, 60});
}

enum class TunerPopupKind { None, Detected, NotFound };

SDL_Rect tuner_popup_rect(int screen_width, int screen_height,
                          TunerPopupKind kind)
{
    const int width = std::min(600, screen_width - 80);
    const int height = kind == TunerPopupKind::NotFound ? 230 : 170;
    return {(screen_width - width) / 2, (screen_height - height) / 2,
            width, height};
}

SDL_Rect tuner_popup_close_rect(int screen_width, int screen_height)
{
    const SDL_Rect popup = tuner_popup_rect(
        screen_width, screen_height, TunerPopupKind::NotFound);
    return {popup.x + (popup.w - 150) / 2, popup.y + popup.h - 62, 150, 44};
}

void draw_tuner_popup(SDL_Renderer * renderer, TextCache & text,
                      int screen_width, int screen_height,
                      TunerPopupKind kind, const std::string & product,
                      const TouchState & touch)
{
    if(kind == TunerPopupKind::None) return;

    const SDL_Rect screen{0, 0, screen_width, screen_height};
    set_colour(renderer, {0, 0, 0, 180});
    SDL_RenderFillRect(renderer, &screen);

    const SDL_Rect popup = tuner_popup_rect(screen_width, screen_height, kind);
    constexpr int kPopupRadius = 16;
    fill_rounded_rect(renderer, popup, kPopupRadius, kPanel);
    draw_rounded_rect(renderer, popup, kPopupRadius,
                      kind == TunerPopupKind::Detected ? kGreen : kRed);

    const int centre_x = popup.x + popup.w / 2;
    if(kind == TunerPopupKind::Detected) {
        text.draw("TUNER DETECTED", centre_x, popup.y + 45, kGreen, 32, true);
        text.draw(product, centre_x, popup.y + 112, kText, 20, true);
    }
    else {
        text.draw("NO TUNER FOUND", centre_x, popup.y + 40, kRed, 32, true);
        text.draw("No MiniTiouner detected on USB.", centre_x,
                  popup.y + 92, kText, 20, true);
        text.draw("Check the cable and power, then restart the app.", centre_x,
                  popup.y + 122, kText, 20, true);
        const SDL_Rect close_button = tuner_popup_close_rect(screen_width, screen_height);
        draw_button(renderer, text, close_button, "CLOSE", kRed, 16,
                    false, is_pressed(touch, close_button));
    }
}

enum class AppPage { Main, Settings, Chat };
enum class ChatInput { None, Nick, Message };

SDL_Rect page_back_rect(int width)
{
    return {width - 114, 6, 106, 40};
}

/* The Settings page has two size classes, chosen the same way Layout picks
 * its own compact mode (width <= 800): the 800x480 panel has only 480px of
 * height to work with, vs 600px on the 1024x600 panel, so its cards, rows,
 * and the SAVE & APPLY button all need tighter, smaller measurements to
 * actually fit - the original fixed layout below was sized for 1024x600
 * only and ran ~100px past the bottom of an 800x480 screen, hiding SAVE &
 * APPLY entirely and clipping the Exit Behaviour card. */
bool settings_compact(int width)
{
    return width <= 800;
}

SDL_Rect settings_receiver_card_rect(int width)
{
    return settings_compact(width) ? SDL_Rect{16, 56, (width - 48) / 2, 188}
                                    : SDL_Rect{40, 70, 460, 260};
}

SDL_Rect settings_display_card_rect(int width)
{
    const SDL_Rect receiver = settings_receiver_card_rect(width);
    const int gap = settings_compact(width) ? 12 : 20;
    const int height = settings_compact(width) ? 102 : 140;
    return {receiver.x, receiver.y + receiver.h + gap, receiver.w, height};
}

SDL_Rect settings_diagnostics_card_rect(int width)
{
    /* Same height as settings_receiver_card_rect, so the two right-column
     * cards below this one (Display Resolution / Exit Behaviour) line up
     * exactly with their left-column counterparts instead of trailing lower. */
    const SDL_Rect receiver = settings_receiver_card_rect(width);
    const int gap = settings_compact(width) ? 16 : 24;
    const int margin = settings_compact(width) ? 16 : 40;
    const int x = receiver.x + receiver.w + gap;
    return {x, receiver.y, width - x - margin, receiver.h};
}

SDL_Rect settings_exit_card_rect(int width)
{
    const SDL_Rect diagnostics = settings_diagnostics_card_rect(width);
    const int gap = settings_compact(width) ? 12 : 20;
    const int height = settings_compact(width) ? 102 : 140;
    return {diagnostics.x, diagnostics.y + diagnostics.h + gap, diagnostics.w, height};
}

SDL_Rect settings_autostart_card_rect(int width)
{
    const SDL_Rect exit_card = settings_exit_card_rect(width);
    const int gap = settings_compact(width) ? 12 : 20;
    const int height = settings_compact(width) ? 102 : 140;
    return {exit_card.x, exit_card.y + exit_card.h + gap, exit_card.w, height};
}

SDL_Rect settings_lo_button_rect(int width, bool increment)
{
    const SDL_Rect card = settings_receiver_card_rect(width);
    if(settings_compact(width))
        return {card.x + (increment ? card.w - 60 : 16), card.y + 70, 44, 38};
    return {card.x + (increment ? 228 : 24), card.y + 88, 48, 48};
}

SDL_Rect settings_voltage_rect(int width, int index)
{
    const SDL_Rect card = settings_receiver_card_rect(width);
    if(settings_compact(width))
        return {card.x + 16 + index * 118, card.y + 144, 106, 38};
    return {card.x + 24 + index * 116, card.y + 188, 104, 50};
}

SDL_Rect settings_display_res_rect(int width, int index)
{
    const SDL_Rect card = settings_display_card_rect(width);
    if(settings_compact(width))
        return {card.x + 16 + index * 178, card.y + 40, 166, 34};
    return {card.x + 24 + index * 216, card.y + 56, 200, 50};
}

SDL_Rect settings_save_rect(int width)
{
    const SDL_Rect card = settings_display_card_rect(width);
    const SDL_Rect autostart_card = settings_autostart_card_rect(width);
    const int button_width = settings_compact(width) ? 150 : 200;
    const int height = settings_compact(width) ? 38 : 50;
    /* Left-aligned with the DISPLAY RESOLUTION card above it, and vertically
     * centred on AUTO START AT BOOT - the card it ends up sitting beside now
     * that EXIT (right-aligned with that same card, see settings_exit_page_
     * rect) shares this row. */
    return {card.x, autostart_card.y + (autostart_card.h - height) / 2,
            button_width, height};
}

SDL_Rect settings_exit_page_rect(int width)
{
    const SDL_Rect card = settings_display_card_rect(width);
    const SDL_Rect save = settings_save_rect(width);
    return {card.x + card.w - save.w, save.y, save.w, save.h};
}

SDL_Rect settings_exit_behaviour_rect(int width, int index)
{
    const SDL_Rect card = settings_exit_card_rect(width);
    if(settings_compact(width))
        return {card.x + 16 + index * 178, card.y + 40, 166, 34};
    return {card.x + 24 + index * 216, card.y + 56, 200, 50};
}

SDL_Rect settings_autostart_rect(int width, int index)
{
    const SDL_Rect card = settings_autostart_card_rect(width);
    if(settings_compact(width))
        return {card.x + 16 + index * 178, card.y + 40, 166, 34};
    return {card.x + 24 + index * 216, card.y + 56, 200, 50};
}

void draw_settings_card(SDL_Renderer * renderer, TextCache & text,
                        const SDL_Rect & card, const std::string & title, bool compact)
{
    fill_panel(renderer, card);
    set_colour(renderer, kBorder);
    SDL_RenderDrawRect(renderer, &card);
    const int margin = compact ? 16 : 24;
    text.draw(title, card.x + margin, card.y + 16, kCyan, 14);
    const SDL_Rect rule{card.x + margin, card.y + 40, card.w - margin * 2, 1};
    set_colour(renderer, kBorder);
    SDL_RenderFillRect(renderer, &rule);
}

void draw_choice_button(SDL_Renderer * renderer, TextCache & text,
                        const SDL_Rect & button, const std::string & label,
                        bool selected, bool disabled, bool pressed,
                        int font_size)
{
    set_colour(renderer, selected ? Colour{0x1f, 0x4d, 0x33} : kPanel);
    SDL_RenderFillRect(renderer, &button);
    set_colour(renderer, selected ? kGreen : kBorder);
    SDL_RenderDrawRect(renderer, &button);
    text.draw(label, button.x + button.w / 2, button.y + button.h / 2,
              selected ? kGreen : (disabled ? kTextDim : kText), font_size, true);
    if(pressed) fill_rounded_rect(renderer, button, 0, {255, 255, 255, 60});
}

void draw_settings_page(SDL_Renderer * renderer, TextCache & text,
                        int width, int height, int lo_mhz,
                        int voltage_choice, int display_choice,
                        int exit_behaviour_choice,
                        const std::string & tuner_product,
                        bool longmynd_connected, bool can_use_1024x600,
                        const TouchState & touch)
{
    const bool compact = settings_compact(width);
    const int label_size = compact ? 14 : 16;
    const int button_label_size = compact ? 14 : 16;

    set_colour(renderer, kBackground);
    const SDL_Rect screen{0, 0, width, height};
    SDL_RenderFillRect(renderer, &screen);
    text.draw("SETTINGS", 12, 12, kCyan, 20);

    const SDL_Rect receiver_card = settings_receiver_card_rect(width);
    draw_settings_card(renderer, text, receiver_card, "RECEIVER TUNING", compact);
    const int lo_label_y = compact ? receiver_card.y + 46 : receiver_card.y + 56;
    text.draw("LNB LO Offset (MHz)", receiver_card.x + (compact ? 16 : 24), lo_label_y,
              kText, label_size);
    const SDL_Rect minus_button = settings_lo_button_rect(width, false);
    const SDL_Rect plus_button = settings_lo_button_rect(width, true);
    draw_button(renderer, text, minus_button, "-", kText, compact ? 16 : 20,
                false, is_pressed(touch, minus_button));
    const SDL_Rect lo_value{minus_button.x + minus_button.w + (compact ? 8 : 8),
                            minus_button.y,
                            plus_button.x - (minus_button.x + minus_button.w) - 16,
                            minus_button.h};
    set_colour(renderer, kBackground);
    SDL_RenderFillRect(renderer, &lo_value);
    set_colour(renderer, kBorder);
    SDL_RenderDrawRect(renderer, &lo_value);
    text.draw(std::to_string(lo_mhz), lo_value.x + lo_value.w / 2,
              lo_value.y + lo_value.h / 2, kText, compact ? 16 : 20, true);
    draw_button(renderer, text, plus_button, "+", kText, compact ? 16 : 20,
                false, is_pressed(touch, plus_button));

    const int voltage_label_y = compact ? receiver_card.y + 122 : receiver_card.y + 156;
    text.draw("LNB Bias Voltage", receiver_card.x + (compact ? 16 : 24), voltage_label_y,
              kText, label_size);
    const char * voltage_labels[] = {"OFF", "13V", "18V"};
    for(int index = 0; index < 3; ++index) {
        const SDL_Rect button = settings_voltage_rect(width, index);
        draw_choice_button(renderer, text, button, voltage_labels[index],
                           index == voltage_choice, false,
                           is_pressed(touch, button), button_label_size);
    }

    const SDL_Rect display_card = settings_display_card_rect(width);
    draw_settings_card(renderer, text, display_card, "DISPLAY RESOLUTION", compact);
    const char * display_labels[] = {"1024 x 600", "800 x 480"};
    for(int index = 0; index < 2; ++index) {
        const SDL_Rect button = settings_display_res_rect(width, index);
        const bool disabled = index == 0 && !can_use_1024x600;
        const bool selected = !disabled && index == display_choice;
        draw_choice_button(renderer, text, button, display_labels[index],
                           selected, disabled,
                           !disabled && is_pressed(touch, button), button_label_size);
    }
    if(!can_use_1024x600 && compact)
        text.draw("1024x600 too big for this screen",
                  display_card.x + display_card.w / 2,
                  display_card.y + display_card.h - 14, kTextDim, 14, true);
    else if(!can_use_1024x600)
        text.draw("1024x600 doesn't fit this screen",
                  display_card.x + 24,
                  display_card.y + display_card.h - 24, kTextDim, 14);
    else if(!compact)
        text.draw("Restarts the app to apply", display_card.x + 24,
                  display_card.y + display_card.h - 24, kTextDim, 14);

    draw_button(renderer, text, settings_save_rect(width), "SAVE & APPLY", kCyan,
                compact ? 14 : 16, false, is_pressed(touch, settings_save_rect(width)));
    draw_button(renderer, text, settings_exit_page_rect(width), "EXIT", kCyan,
                compact ? 14 : 16, false, is_pressed(touch, settings_exit_page_rect(width)));

    const SDL_Rect diagnostics_card = settings_diagnostics_card_rect(width);
    draw_settings_card(renderer, text, diagnostics_card, "DIAGNOSTICS", compact);
    const int diagnostic_x = diagnostics_card.x + (compact ? 16 : 24);
    if(compact) {
        text.draw("Tuner (USB)", diagnostic_x, diagnostics_card.y + 46, kText, 14);
        text.draw(tuner_product.empty() ? "Not detected" : tuner_product,
                  diagnostic_x, diagnostics_card.y + 64,
                  tuner_product.empty() ? kRed : kText, 14);
        text.draw("Longmynd Link", diagnostic_x, diagnostics_card.y + 88, kText, 14);
        text.draw(longmynd_connected ? "Connected" : "Not connected",
                  diagnostic_x, diagnostics_card.y + 106,
                  longmynd_connected ? kGreen : kRed, 14);
        text.draw("Watch in VLC (same network)", diagnostic_x,
                  diagnostics_card.y + 130, kText, 14);
        text.draw(qo100::ts_stream_vlc_url(), diagnostic_x,
                  diagnostics_card.y + 150, kCyan, 14);
    }
    else {
        text.draw("Tuner (USB)", diagnostic_x, diagnostics_card.y + 58, kText, 16);
        text.draw(tuner_product.empty() ? "Not detected" : tuner_product,
                  diagnostic_x, diagnostics_card.y + 82,
                  tuner_product.empty() ? kRed : kText, 16);
        text.draw("Longmynd Link", diagnostic_x, diagnostics_card.y + 122, kText, 16);
        text.draw(longmynd_connected ? "Connected" : "Not connected",
                  diagnostic_x, diagnostics_card.y + 146,
                  longmynd_connected ? kGreen : kRed, 16);
        text.draw("Watch in VLC (same network)", diagnostic_x, diagnostics_card.y + 186, kText, 16);
        text.draw("Media > Open Network Stream:", diagnostic_x, diagnostics_card.y + 210, kTextDim, 14);
        text.draw(qo100::ts_stream_vlc_url(), diagnostic_x, diagnostics_card.y + 230, kCyan, 16);
    }

    const SDL_Rect exit_card = settings_exit_card_rect(width);
    draw_settings_card(renderer, text, exit_card, "EXIT BUTTON BEHAVIOUR", compact);
    const char * exit_labels[] = {"Restart", "Full Stop"};
    for(int index = 0; index < 2; ++index) {
        const SDL_Rect button = settings_exit_behaviour_rect(width, index);
        draw_choice_button(renderer, text, button, exit_labels[index],
                           index == exit_behaviour_choice, false,
                           is_pressed(touch, button), button_label_size);
    }
    if(!compact)
        text.draw(exit_behaviour_choice == 1
                      ? "EXIT stops the app - restart via desktop icon"
                      : "EXIT restarts the app automatically",
                  exit_card.x + 24, exit_card.y + exit_card.h - 24, kTextDim, 14);

    const SDL_Rect autostart_card = settings_autostart_card_rect(width);
    draw_settings_card(renderer, text, autostart_card, "AUTO START AT BOOT", compact);
    const bool autostart_on = autostart_enabled();
    const char * autostart_labels[] = {"On", "Off"};
    for(int index = 0; index < 2; ++index) {
        const SDL_Rect button = settings_autostart_rect(width, index);
        const bool selected = (index == 0) == autostart_on;
        draw_choice_button(renderer, text, button, autostart_labels[index],
                           selected, false, is_pressed(touch, button),
                           button_label_size);
    }
    if(!compact)
        text.draw("Launches automatically at power-on (kiosk mode)",
                  autostart_card.x + 24, autostart_card.y + autostart_card.h - 24,
                  kTextDim, 14);
}

struct KeyboardKey {
    SDL_Rect rect;
    std::string label;
};

void add_keyboard_row(std::vector<KeyboardKey> & keys,
                      const std::vector<std::string> & labels,
                      int y, int screen_width, int key_height,
                      int side_margin = 8)
{
    constexpr int gap = 5;
    const int available = screen_width - side_margin * 2 -
                          gap * static_cast<int>(labels.size() - 1);
    const int key_width = available / static_cast<int>(labels.size());
    int x = side_margin;
    for(size_t index = 0; index < labels.size(); ++index) {
        const int width = index + 1 == labels.size()
            ? screen_width - side_margin - x : key_width;
        keys.push_back({{x, y, width, key_height}, labels[index]});
        x += width + gap;
    }
}

std::vector<KeyboardKey> keyboard_keys(int screen_width, int screen_height,
                                       bool symbols, bool shifted)
{
    const int top = screen_height - 250;
    constexpr int height = 54;
    constexpr int row_gap = 6;
    std::vector<KeyboardKey> keys;
    if(!symbols) {
        std::vector<std::string> row1{"q","w","e","r","t","y","u","i","o","p"};
        std::vector<std::string> row2{"a","s","d","f","g","h","j","k","l"};
        std::vector<std::string> row3{"SHIFT","z","x","c","v","b","n","m","BACK"};
        if(shifted) {
            for(auto * row : {&row1, &row2, &row3}) {
                for(std::string & label : *row) {
                    if(label.size() == 1)
                        label[0] = static_cast<char>(std::toupper(label[0]));
                }
            }
        }
        add_keyboard_row(keys, row1, top, screen_width, height);
        add_keyboard_row(keys, row2, top + height + row_gap, screen_width, height, 38);
        add_keyboard_row(keys, row3, top + 2 * (height + row_gap), screen_width, height);
    }
    else {
        add_keyboard_row(keys, {"1","2","3","4","5","6","7","8","9","0"},
                         top, screen_width, height);
        add_keyboard_row(keys, {"!","@","#","$","%","&","*","(",")"},
                         top + height + row_gap, screen_width, height, 38);
        add_keyboard_row(keys, {"ABC",".",",","?","!","'","\"","/","BACK"},
                         top + 2 * (height + row_gap), screen_width, height);
    }
    add_keyboard_row(keys, {symbols ? "ABC" : "SYM", "SPACE", "-", "CANCEL", "ENTER"},
                     top + 3 * (height + row_gap), screen_width, height);
    return keys;
}

void draw_keyboard(SDL_Renderer * renderer, TextCache & text,
                   int width, int height, bool symbols, bool shifted,
                   const TouchState & touch)
{
    const SDL_Rect background{0, height - 258, width, 258};
    set_colour(renderer, kPanel);
    SDL_RenderFillRect(renderer, &background);
    set_colour(renderer, kBorder);
    SDL_RenderDrawLine(renderer, 0, background.y, width, background.y);
    for(const KeyboardKey & key : keyboard_keys(width, height, symbols, shifted)) {
        Colour colour = kText;
        if(key.label == "ENTER") colour = kGreen;
        else if(key.label == "CANCEL" || key.label == "BACK") colour = kRed;
        else if(key.label == "SYM" || key.label == "ABC" || key.label == "SHIFT")
            colour = kYellow;
        draw_button(renderer, text, key.rect, key.label, colour, 16,
                    false, is_pressed(touch, key.rect));
    }
}

std::string tail_that_fits(TextCache & text, const std::string & value, int width)
{
    if(text.measure(value, 16).first <= width) return value;
    size_t start = 0;
    while(start < value.size() && text.measure(value.substr(start), 16).first > width) {
        ++start;
        while(start < value.size() &&
              (static_cast<unsigned char>(value[start]) & 0xc0U) == 0x80U) ++start;
    }
    return value.substr(start);
}

void draw_text_input(SDL_Renderer * renderer, TextCache & text,
                     const SDL_Rect & rect, const std::string & value,
                     const char * placeholder, bool active)
{
    set_colour(renderer, kBackground);
    SDL_RenderFillRect(renderer, &rect);
    set_colour(renderer, active ? kCyan : kBorder);
    SDL_RenderDrawRect(renderer, &rect);
    const std::string shown = value.empty()
        ? std::string(placeholder) : tail_that_fits(text, value, rect.w - 16);
    const SDL_Rect clip{rect.x + 6, rect.y + 2, rect.w - 12, rect.h - 4};
    SDL_RenderSetClipRect(renderer, &clip);
    text.draw(shown, rect.x + 8, rect.y + (rect.h - 16) / 2,
              value.empty() ? kTextDim : kText, 16, false);
    SDL_RenderSetClipRect(renderer, nullptr);
}

std::vector<std::string> wrap_chat_text(TextCache & text,
                                        const std::string & message, int width,
                                        int font_size)
{
    std::vector<std::string> lines;
    std::string line;
    size_t position = 0;
    while(position < message.size()) {
        while(position < message.size() && message[position] == ' ') ++position;
        size_t end = message.find(' ', position);
        if(end == std::string::npos) end = message.size();
        const std::string word = message.substr(position, end - position);
        const std::string candidate = line.empty() ? word : line + " " + word;
        if(!line.empty() && text.measure(candidate, font_size).first > width) {
            lines.push_back(line);
            line = word;
        }
        else line = candidate;
        position = end;
    }
    if(!line.empty()) lines.push_back(line);
    if(lines.empty()) lines.emplace_back();
    return lines;
}

void draw_chat_history(SDL_Renderer * renderer, TextCache & text,
                       const SDL_Rect & box, const qo100::ChatState & state,
                       size_t & first_visible, size_t & last_visible,
                       bool & follow_latest)
{
    set_colour(renderer, {0x3f, 0x46, 0x4c});
    SDL_RenderFillRect(renderer, &box);
    set_colour(renderer, kBorder);
    SDL_RenderDrawRect(renderer, &box);
    constexpr int kChatFontSize = 16;
    if(state.lines.empty()) {
        text.draw("Waiting for chat history...", box.x + 10, box.y + 10, kText, kChatFontSize);
        first_visible = 0;
        last_visible = 0;
        follow_latest = true;
        return;
    }

    constexpr int line_height = 22;
    const int message_x = box.x + 245;
    const int message_width = box.x + box.w - message_x - 10;
    const int available_height = box.h - 16;
    struct Visible {
        size_t index;
        std::vector<std::string> lines;
        int height;
    };
    std::vector<Visible> visible;
    int used_height = 0;

    if(follow_latest) {
        for(size_t index = state.lines.size(); index > 0; --index) {
            auto wrapped = wrap_chat_text(
                text, state.lines[index - 1].message, message_width, kChatFontSize);
            const int needed = static_cast<int>(wrapped.size()) * line_height + 3;
            if(!visible.empty() && used_height + needed > available_height) break;
            visible.insert(visible.begin(),
                           {index - 1, std::move(wrapped), needed});
            used_height += needed;
        }
    }
    else {
        first_visible = std::min(first_visible, state.lines.size() - 1);
        for(size_t index = first_visible; index < state.lines.size(); ++index) {
            auto wrapped = wrap_chat_text(
                text, state.lines[index].message, message_width, kChatFontSize);
            const int needed = static_cast<int>(wrapped.size()) * line_height + 3;
            if(!visible.empty() && used_height + needed > available_height) break;
            visible.push_back({index, std::move(wrapped), needed});
            used_height += needed;
        }
    }

    if(visible.empty()) return;
    first_visible = visible.front().index;
    last_visible = visible.back().index;
    if(!follow_latest && last_visible + 1 >= state.lines.size()) follow_latest = true;
    int y = follow_latest ? box.y + box.h - 8 - used_height : box.y + 8;
    const SDL_Rect clip{box.x + 1, box.y + 1, box.w - 2, box.h - 2};
    SDL_RenderSetClipRect(renderer, &clip);
    for(const Visible & item : visible) {
        const qo100::ChatLine & source = state.lines[item.index];
        text.draw(source.time, box.x + 10, y, {0x9a, 0xa0, 0xa6}, kChatFontSize);
        text.draw(source.name, box.x + 68, y, kYellow, kChatFontSize);
        for(size_t line = 0; line < item.lines.size(); ++line)
            text.draw(item.lines[line], message_x,
                      y + static_cast<int>(line) * line_height, kText, kChatFontSize);
        y += item.height;
    }
    SDL_RenderSetClipRect(renderer, nullptr);
    if(!follow_latest)
        text.draw("SCROLLED", box.x + box.w - 82, box.y + 7, kYellow, kChatFontSize);
}

SDL_Rect chat_history_rect(int width, int height, bool keyboard_open)
{
    const int content_bottom = keyboard_open ? height - 316 : height - 68;
    return {8, 48, width - 234, content_bottom - 48};
}

SDL_Rect chat_nick_rect(int height, bool keyboard_open)
{
    return {8, keyboard_open ? height - 306 : height - 58, 150, 48};
}

SDL_Rect chat_set_rect(int height, bool keyboard_open)
{
    return {164, keyboard_open ? height - 306 : height - 58, 76, 48};
}

SDL_Rect chat_message_rect(int width, int height, bool keyboard_open)
{
    return {248, keyboard_open ? height - 306 : height - 58, width - 370, 48};
}

SDL_Rect chat_send_rect(int width, int height, bool keyboard_open)
{
    return {width - 114, keyboard_open ? height - 306 : height - 58, 106, 48};
}

void draw_chat_page(SDL_Renderer * renderer, TextCache & text,
                    int width, int height, const qo100::ChatState & state,
                    const std::string & nick, const std::string & message,
                    ChatInput active_input, bool symbols, bool shifted,
                    size_t & first_visible, size_t & last_visible,
                    bool & follow_latest, const TouchState & touch)
{
    set_colour(renderer, kBackground);
    const SDL_Rect screen{0, 0, width, height};
    SDL_RenderFillRect(renderer, &screen);
    text.draw("QO-100 WIDEBAND CHAT", 12, 12, kCyan, 20);
    const char * status = state.connection == qo100::ChatState::Connection::Connected
        ? "CONNECTED" : (state.connection == qo100::ChatState::Connection::Reconnecting
            ? "RECONNECTING..." : "CONNECTING...");
    const Colour status_colour = state.connection == qo100::ChatState::Connection::Connected
        ? kGreen : kYellow;
    text.draw(status, 330, 17, status_colour, 14);
    text.draw("VIEWERS: " + state.viewers, width - 260, 17, kTextDim, 14);
    draw_button(renderer, text, page_back_rect(width), "BACK", kCyan, 16,
                false, is_pressed(touch, page_back_rect(width)));

    const bool keyboard_open = active_input != ChatInput::None;
    const int content_bottom = keyboard_open ? height - 316 : height - 68;
    const SDL_Rect history = chat_history_rect(width, height, keyboard_open);
    const SDL_Rect users{width - 218, 48, 210, content_bottom - 48};
    draw_chat_history(renderer, text, history, state,
                      first_visible, last_visible, follow_latest);
    set_colour(renderer, {0x3f, 0x46, 0x4c});
    SDL_RenderFillRect(renderer, &users);
    set_colour(renderer, kBorder);
    SDL_RenderDrawRect(renderer, &users);
    text.draw("ONLINE", users.x + 10, users.y + 8, kYellow, 16);
    const size_t max_users = users.h > 40 ? static_cast<size_t>((users.h - 40) / 22) : 0U;
    for(size_t index = 0; index < std::min(max_users, state.users.size()); ++index)
        text.draw(state.users[index], users.x + 10, users.y + 34 +
                  static_cast<int>(index) * 22, kText, 16);

    draw_text_input(renderer, text, chat_nick_rect(height, keyboard_open),
                    nick, "CALLSIGN", active_input == ChatInput::Nick);
    const SDL_Rect set_button = chat_set_rect(height, keyboard_open);
    draw_button(renderer, text, set_button, "SET", kYellow, 16,
                false, is_pressed(touch, set_button));
    draw_text_input(renderer, text, chat_message_rect(width, height, keyboard_open),
                    message, "Type a message...", active_input == ChatInput::Message);
    const SDL_Rect send_button = chat_send_rect(width, height, keyboard_open);
    draw_button(renderer, text, send_button, "SEND", kCyan, 16,
                false, is_pressed(touch, send_button));
    if(keyboard_open)
        draw_keyboard(renderer, text, width, height, symbols, shifted, touch);
}

void erase_last_utf8(std::string & value)
{
    if(value.empty()) return;
    size_t position = value.size() - 1;
    while(position > 0 &&
          (static_cast<unsigned char>(value[position]) & 0xc0U) == 0x80U) --position;
    value.erase(position);
}

void draw_video_notice(TextCache & text, const SDL_Rect & bounds, VideoNotice notice,
                       double elapsed_seconds)
{
    const char * message = nullptr;
    Colour colour = kYellow;
    switch(notice) {
        case VideoNotice::WaitingForTuner:
            message = "WAITING FOR TUNER";
            break;
        case VideoNotice::Tuning:
            message = "WAIT - TUNING";
            break;
        case VideoNotice::NoVideoStream:
            message = "POOR SIGNAL";
            colour = kRed;
            break;
        case VideoNotice::None:
            return;
    }

    const int centre_x = bounds.x + bounds.w / 2;
    const int centre_y = bounds.y + bounds.h / 2;
    /* A steady message never visibly changes over a long search, which
     * reads as "frozen" even though the app is fine underneath - so pulse
     * the colour and show a running mm:ss so it's obvious time is passing
     * and the app is still actively retrying, not hung. */
    const float pulse = 0.5F + 0.5F * static_cast<float>(
        std::sin(elapsed_seconds * 2.0));
    const Colour pulsing_colour = mix_colour(colour, kTextDim, pulse * 0.5F);
    text.draw(message, centre_x + 2, centre_y + 2, {0, 0, 0}, 20, true);
    text.draw(message, centre_x, centre_y, pulsing_colour, 20, true);

    const int elapsed_total = static_cast<int>(elapsed_seconds);
    char elapsed_text[16];
    std::snprintf(elapsed_text, sizeof(elapsed_text), "%d:%02d",
                  elapsed_total / 60, elapsed_total % 60);
    text.draw(elapsed_text, centre_x + 1, centre_y + 27, {0, 0, 0}, 14, true);
    text.draw(elapsed_text, centre_x, centre_y + 26, kTextDim, 14, true);
}

/* Shared by status_button_rect and status_row_height so they can't drift
 * apart - kStatusButtonBlockH is the button height plus the margin above
 * it. Shrunk from 54/65 to free up room for a less-squeezed VU meter. */
constexpr int kStatusButtonHeight = 46;
constexpr int kStatusButtonBlockH = 57;
constexpr int kVolRowH = 20;
constexpr int kVuRowH = 22;

SDL_Rect status_button_rect(const Layout & layout, int index)
{
    constexpr int gap = 8;
    constexpr int margin = 8;
    const int button_width = (layout.status_panel.w - 2 * margin - 3 * gap) / 4;
    const int button_y = layout.status_panel.y + layout.status_panel.h - kStatusButtonBlockH;
    return {layout.status_panel.x + margin + index * (button_width + gap),
            button_y, button_width, kStatusButtonHeight};
}

/* The volume slider track - shared by drawing (draw_status) and hit-testing
 * (touch/mouse handling), so they can't drift apart. */
/* Status grid row height, sized to whatever's actually left in the panel
 * after the fixed-height rows below it (VOL bar, VU meter, button block) -
 * rather than a fixed formula that can outgrow the panel at smaller
 * resolutions and push the button row off the bottom (observed at 800x480:
 * the VU meter and CHAT/SET/... buttons overlapped by a few pixels). */
int status_row_height(const Layout & layout)
{
    constexpr int kRowCount = 6;
    constexpr int kTopPad = 8;
    const int available = layout.status_panel.h - kTopPad - kVolRowH - kVuRowH - kStatusButtonBlockH;
    return std::clamp(available / kRowCount, 16, 30);
}

SDL_Rect volume_track_rect(const Layout & layout)
{
    constexpr int kRowCount = 6;
    const int row_height = status_row_height(layout);
    const int grid_bottom = layout.status_panel.y + 8 + kRowCount * row_height;
    return {layout.status_panel.x + 50, grid_bottom + 6,
            layout.status_panel.w - 105, 7};
}

int volume_from_x(const SDL_Rect & track, int x)
{
    return std::clamp((x - track.x) * 100 / track.w, 0, 100);
}

struct ModcodEntry { const char * modulation; const char * fec; };

const ModcodEntry kDvbsModcod[] = {
    {"QPSK", "1/2"}, {"QPSK", "2/3"}, {"QPSK", "3/4"},
    {"QPSK", "5/6"}, {"QPSK", "6/7"}, {"QPSK", "7/8"}
};

const ModcodEntry kDvbs2Modcod[] = {
    {"---", "---"}, {"QPSK", "1/4"}, {"QPSK", "1/3"}, {"QPSK", "2/5"},
    {"QPSK", "1/2"}, {"QPSK", "3/5"}, {"QPSK", "2/3"}, {"QPSK", "3/4"},
    {"QPSK", "4/5"}, {"QPSK", "5/6"}, {"QPSK", "8/9"}, {"QPSK", "9/10"},
    {"8PSK", "3/5"}, {"8PSK", "2/3"}, {"8PSK", "3/4"}, {"8PSK", "5/6"},
    {"8PSK", "8/9"}, {"8PSK", "9/10"}, {"16APSK", "2/3"}, {"16APSK", "3/4"},
    {"16APSK", "4/5"}, {"16APSK", "5/6"}, {"16APSK", "8/9"}, {"16APSK", "9/10"},
    {"32APSK", "3/4"}, {"32APSK", "4/5"}, {"32APSK", "5/6"},
    {"32APSK", "8/9"}, {"32APSK", "9/10"}
};

const double kDvbsRequiredMer[] = {1.7, 3.3, 4.2, 5.1, 5.5, 5.8};
const double kDvbs2RequiredMer[] = {
    0.0, -2.35, -1.24, -0.30, 1.00, 2.23, 3.10, 4.03, 4.68, 5.18,
    6.20, 6.42, 5.50, 6.62, 7.91, 9.35, 10.69, 10.98, 8.97, 10.21,
    11.03, 11.61, 12.89, 13.13, 12.73, 13.64, 14.28, 15.69, 16.05
};

bool required_mer(const qo100::ReceiverStatus & status, double & value)
{
    if(status.modcod < 0) return false;
    if(status.demod_state == 4 && status.modcod > 0 &&
       static_cast<size_t>(status.modcod) < sizeof(kDvbs2RequiredMer) / sizeof(double)) {
        value = kDvbs2RequiredMer[status.modcod];
        return true;
    }
    if(status.demod_state == 3 &&
       static_cast<size_t>(status.modcod) < sizeof(kDvbsRequiredMer) / sizeof(double)) {
        value = kDvbsRequiredMer[status.modcod];
        return true;
    }
    return false;
}

struct StatusField {
    std::string value;
    Colour colour = kTextDim;
    std::string unit;
    /* Only meaningful when unit is empty - right-align to the same edge as
     * the numeric fields above/below it instead of the default flush-left,
     * so e.g. "Quality" lines up with Freq/IF/SR/MER/Margin's right edge
     * rather than drifting depending on word length ("Poor" vs "Excellent"). */
    bool right_align = false;
};

void draw_status(SDL_Renderer * renderer, TextCache & text, const Layout & layout,
                 int volume_percent, int audio_peak_percent,
                 const qo100::ReceiverStatus & receiver,
                 bool monitor_connected, bool status_received,
                 double tuned_frequency_mhz, long tuned_if_khz,
                 long tuned_symbol_rate_ksps,
                 const std::string & video_codec,
                 const std::string & audio_codec,
                 bool scan_active, const TouchState & touch)
{
    fill_panel(renderer, layout.status_panel);
    const bool locked = receiver.locked();
    double threshold = 0.0;
    const bool have_threshold = locked && required_mer(receiver, threshold);
    const double mer_margin = receiver.mer_x10 / 10.0 - threshold;
    Colour quality_colour = kTextDim;
    std::string quality = "---";
    if(have_threshold) {
        if(mer_margin >= 4.0) { quality = "Excellent"; quality_colour = kGreen; }
        else if(mer_margin >= 2.0) { quality = "Good"; quality_colour = kGreen; }
        else if(mer_margin >= 0.5) { quality = "Marginal"; quality_colour = kYellow; }
        else { quality = "Poor"; quality_colour = kRed; }
    }

    const ModcodEntry * modcod = nullptr;
    if(locked && receiver.modcod >= 0) {
        if(receiver.demod_state == 4 &&
           static_cast<size_t>(receiver.modcod) < sizeof(kDvbs2Modcod) / sizeof(kDvbs2Modcod[0]))
            modcod = &kDvbs2Modcod[receiver.modcod];
        else if(receiver.demod_state == 3 &&
                static_cast<size_t>(receiver.modcod) < sizeof(kDvbsModcod) / sizeof(kDvbsModcod[0]))
            modcod = &kDvbsModcod[receiver.modcod];
    }

    char mer_text[24] = "---";
    char margin_text[24] = "---";
    char ber_text[24] = "---";
    char modfec_text[24] = "---";
    char null_text[24] = "---";
    if(locked) {
        std::snprintf(mer_text, sizeof(mer_text), "%.1f", receiver.mer_x10 / 10.0);
        std::snprintf(ber_text, sizeof(ber_text), "%.2f%%", receiver.ber_x100 / 100.0);
    }
    if(have_threshold) std::snprintf(margin_text, sizeof(margin_text), "%+.1f", mer_margin);
    if(modcod != nullptr)
        std::snprintf(modfec_text, sizeof(modfec_text), "%s %s", modcod->modulation, modcod->fec);
    if(locked && receiver.null_packet_percent >= 0)
        std::snprintf(null_text, sizeof(null_text), "%d%%", receiver.null_packet_percent);

    const std::string service = !receiver.service_name.empty()
        ? receiver.service_name : receiver.service_provider;

    char frequency_text[32];
    char if_frequency_text[32];
    char symbol_rate_text[24];
    std::snprintf(frequency_text, sizeof(frequency_text), "%.3f", tuned_frequency_mhz);
    const long displayed_if_khz = locked && receiver.carrier_khz > 0
        ? receiver.carrier_khz : tuned_if_khz;
    std::snprintf(if_frequency_text, sizeof(if_frequency_text), "%.3f",
                  displayed_if_khz / 1000.0);
    std::snprintf(symbol_rate_text, sizeof(symbol_rate_text), "%ld", tuned_symbol_rate_ksps);
    (void)monitor_connected;
    (void)status_received;

    const StatusField left[] = {
        {frequency_text, kText, "MHz"},
        {if_frequency_text, kText, "MHz"},
        {symbol_rate_text, kText, "kS/s"},
        {mer_text, locked ? (have_threshold ? quality_colour : kText) : kTextDim,
            locked ? "dB" : ""},
        {quality, quality_colour, "", true},
        {margin_text, have_threshold ? quality_colour : kTextDim, have_threshold ? "dB" : ""}
    };
    const StatusField right[] = {
        {locked && !service.empty() ? service : "---", locked && !service.empty() ? kGreen : kTextDim, ""},
        {video_codec.empty() ? "---" : video_codec, video_codec.empty() ? kTextDim : kText, ""},
        {audio_codec.empty() ? "---" : audio_codec, audio_codec.empty() ? kTextDim : kText, ""},
        {modfec_text, modcod != nullptr ? kText : kTextDim, ""},
        {null_text, locked && receiver.null_packet_percent >= 0 ? kText : kTextDim, ""},
        {ber_text, locked ? kText : kTextDim, ""}
    };
    static const char * left_labels[] = {
        "Freq", "IF", "SR", "MER", "Quality", "Margin"
    };
    static const char * right_labels[] = {
        "Service", "Video", "Audio", "MOD/FEC", "Null", "BER"
    };
    const int row_height = status_row_height(layout);
    const int left_x = layout.status_panel.x + 10;
    /* Column x-positions and value-column widths scale with the panel's
     * actual width rather than a fixed pixel layout - at 1024x600 this
     * panel is ~475px wide (the reference these proportions were tuned
     * against); at 800x480 it's only ~368px, and the old fixed offsets put
     * the right column's labels on top of the left column's values. Only
     * 14/16/20/32 are preloaded fonts (see TextCache::load_font calls), so
     * the narrower size below has to be one of those, not something
     * in-between. */
    constexpr int kReferencePanelW = 475;
    const bool narrow = layout.status_panel.w < 420;
    const int kFontSize = narrow ? 14 : 16;
    const int right_x = layout.status_panel.x +
        layout.status_panel.w * 261 / kReferencePanelW;
    constexpr int kRowCount = 6;
    const int kValueX = layout.status_panel.w * 95 / kReferencePanelW;
    const int kNumberColumnWidth = layout.status_panel.w * 82 / kReferencePanelW;
    const int kUnitGap = std::max(4, layout.status_panel.w * 8 / kReferencePanelW);
    const auto draw_value = [&](const StatusField & field, int column_x, int y) {
        if(field.unit.empty()) {
            if(field.right_align) {
                const auto [text_w, text_h] = text.measure(field.value, kFontSize);
                (void)text_h;
                text.draw(field.value, column_x + kValueX + kNumberColumnWidth - text_w,
                          y, field.colour, kFontSize);
            }
            else {
                text.draw(field.value, column_x + kValueX, y, field.colour, kFontSize);
            }
            return;
        }
        const int number_right = column_x + kValueX + kNumberColumnWidth;
        const auto [number_w, number_h] = text.measure(field.value, kFontSize);
        (void)number_h;
        text.draw(field.value, number_right - number_w, y, field.colour, kFontSize);
        text.draw(field.unit, number_right + kUnitGap, y, kTextDim, kFontSize);
    };
    for(int i = 0; i < kRowCount; ++i) {
        const int y = layout.status_panel.y + 8 + i * row_height;
        text.draw(left_labels[i], left_x, y, kTextDim, kFontSize);
        draw_value(left[i], left_x, y);
        text.draw(right_labels[i], right_x, y, kTextDim, kFontSize);
        draw_value(right[i], right_x, y);
    }

    const int grid_bottom = layout.status_panel.y + 8 + kRowCount * row_height;
    text.draw("VOL", left_x, grid_bottom, kTextDim);
    text.draw(std::to_string(volume_percent) + "%",
              layout.status_panel.x + layout.status_panel.w - 46,
              grid_bottom, kText);
    const SDL_Rect track = volume_track_rect(layout);
    set_colour(renderer, kBorder);
    SDL_RenderFillRect(renderer, &track);
    SDL_Rect filled = track;
    filled.w = track.w * volume_percent / 100;
    set_colour(renderer, {0x2b, 0x8e, 0xa3});
    SDL_RenderFillRect(renderer, &filled);
    set_colour(renderer, {0x65, 0xb7, 0xc7});
    constexpr int kVolumeKnobRadius = 7;
    const int knob_x = track.x + filled.w;
    for(int y = -kVolumeKnobRadius; y <= kVolumeKnobRadius; ++y) {
        const int half = static_cast<int>(
            std::sqrt(kVolumeKnobRadius * kVolumeKnobRadius - y * y));
        SDL_RenderDrawLine(renderer, knob_x - half, track.y + 3 + y,
                           knob_x + half, track.y + 3 + y);
    }

    /* VU meter: fast attack (jumps straight to a new louder peak), slow
     * release (decays a few points per frame) - a raw per-callback peak
     * would just flicker. Its right edge tracks the volume knob's 3-o'clock
     * edge rather than always spanning the full track, so it visually reads
     * as "how loud, out of what the volume slider currently allows" instead
     * of implying headroom past where the volume is actually set. */
    static int displayed_peak = 0;
    displayed_peak = std::max(audio_peak_percent, displayed_peak - 3);
    const int vu_row_y = grid_bottom + kVolRowH;
    text.draw("VU", left_x, vu_row_y, kTextDim);
    constexpr int kVuSegments = 20;
    constexpr int kVuGap = 3;
    const int vu_row_w = std::min(track.w, filled.w + kVolumeKnobRadius);
    const int vu_segment_w = (vu_row_w - (kVuSegments - 1) * kVuGap) / kVuSegments;
    const SDL_Rect vu_row{track.x, vu_row_y, vu_row_w, 14};
    const int lit_segments = (displayed_peak * kVuSegments + 99) / 100;
    for(int i = 0; i < kVuSegments; ++i) {
        const Colour segment_colour = i >= kVuSegments * 9 / 10 ? kRed
            : i >= kVuSegments * 7 / 10 ? kYellow : kGreen;
        set_colour(renderer, i < lit_segments ? segment_colour : kBorder);
        const SDL_Rect segment{vu_row.x + i * (vu_segment_w + kVuGap), vu_row.y,
                               vu_segment_w, vu_row.h};
        SDL_RenderFillRect(renderer, &segment);
    }

    const char * labels[] = {"CHAT", "SET", "SCAN", "EXIT"};
    const Colour colours[] = {kCyan, kYellow, kPurple, kRed};
    for(int i = 0; i < 4; ++i) {
        const SDL_Rect button = status_button_rect(layout, i);
        draw_button(renderer, text, button, labels[i], colours[i], 16,
                    i == 2 && scan_active, is_pressed(touch, button));
    }
}

VideoFrame make_demo_frame(int width, int height, uint64_t index, int64_t pts_us)
{
    VideoFrame frame;
    frame.width = width;
    frame.height = height;
    frame.y_pitch = width;
    frame.uv_pitch = (width + 1) / 2;
    const int chroma_height = (height + 1) / 2;
    const size_t y_size = static_cast<size_t>(frame.y_pitch) * height;
    const size_t uv_size = static_cast<size_t>(frame.uv_pitch) * chroma_height;
    frame.u_offset = y_size;
    frame.v_offset = y_size + uv_size;
    frame.pts_us = pts_us;
    frame.yuv420.resize(y_size + uv_size * 2U);
    const int bar = static_cast<int>((index * 4) % std::max(1, width));
    for(int y = 0; y < height; ++y) {
        for(int x = 0; x < width; ++x) {
            const bool highlight = std::abs(x - bar) < 10;
            frame.yuv420[static_cast<size_t>(y) * frame.y_pitch + x] =
                highlight ? 190 : static_cast<uint8_t>(40 + 80 * x / std::max(1, width));
        }
    }
    std::fill(frame.yuv420.begin() + static_cast<ptrdiff_t>(frame.u_offset),
              frame.yuv420.begin() + static_cast<ptrdiff_t>(frame.v_offset), 150);
    std::fill(frame.yuv420.begin() + static_cast<ptrdiff_t>(frame.v_offset),
              frame.yuv420.end(), 90);
    return frame;
}

SDL_Rect aspect_fit(int source_width, int source_height, const SDL_Rect & bounds)
{
    if(source_width <= 0 || source_height <= 0) return bounds;
    const double source_aspect = static_cast<double>(source_width) / source_height;
    const double target_aspect = static_cast<double>(bounds.w) / bounds.h;
    SDL_Rect result = bounds;
    if(source_aspect > target_aspect) {
        result.h = std::max(1, static_cast<int>(bounds.w / source_aspect));
        result.y += (bounds.h - result.h) / 2;
    }
    else {
        result.w = std::max(1, static_cast<int>(bounds.h * source_aspect));
        result.x += (bounds.w - result.w) / 2;
    }
    return result;
}

bool save_screenshot(SDL_Renderer * renderer, int width, int height, const std::string & path)
{
    SDL_Surface * surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32,
                                                            SDL_PIXELFORMAT_ARGB8888);
    if(surface == nullptr) return false;
    const int result = SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888,
                                            surface->pixels, surface->pitch);
    const bool saved = result == 0 && SDL_SaveBMP(surface, path.c_str()) == 0;
    if(!saved) qo100::log("[SCREENSHOT] %s\n", SDL_GetError());
    SDL_FreeSurface(surface);
    return saved;
}

struct Options {
    bool demo = false;
    bool offline_spectrum = false;
    bool no_tuner = false;
    int seconds = 0;
    std::string screenshot;
};

Options parse_options(int argc, char ** argv)
{
    Options options;
    for(int i = 1; i < argc; ++i) {
        if(std::strcmp(argv[i], "--demo") == 0) options.demo = true;
        else if(std::strcmp(argv[i], "--offline-spectrum") == 0)
            options.offline_spectrum = true;
        else if(std::strcmp(argv[i], "--no-tuner") == 0)
            options.no_tuner = true;
        else if(std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
            options.seconds = std::max(0, std::atoi(argv[++i]));
        else if(std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc)
            options.screenshot = argv[++i];
    }
    return options;
}

std::string read_first_line(const std::string & path)
{
    std::ifstream file(path);
    std::string line;
    if(file) std::getline(file, line);
    while(!line.empty() && (line.back() == '\0' || line.back() == '\n'))
        line.pop_back();
    return line;
}

std::string read_proc_field(const std::string & path, const std::string & key)
{
    std::ifstream file(path);
    std::string line;
    while(file && std::getline(file, line))
        if(line.rfind(key, 0) == 0) return line;
    return {};
}

/* Resident memory of this process, for spotting a slow leak over a long
 * unattended run (e.g. an unbounded cache) that a single boot-time
 * snapshot can't show. */
long process_rss_kb()
{
    const std::string field = read_proc_field("/proc/self/status", "VmRSS:");
    long kb = 0;
    std::sscanf(field.c_str(), "VmRSS: %ld", &kb);
    return kb;
}

double cpu_temperature_c()
{
    std::ifstream file("/sys/class/thermal/thermal_zone0/temp");
    long millidegrees = -1;
    if(file) file >> millidegrees;
    return millidegrees > 0 ? millidegrees / 1000.0 : -1.0;
}

void log_startup_banner()
{
    const std::time_t now = std::time(nullptr);
    char time_buffer[64]{};
    std::strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S %Z",
                  std::localtime(&now));

    utsname system_info{};
    uname(&system_info);
    char hostname[256]{};
    gethostname(hostname, sizeof(hostname));

    const std::string model = read_first_line("/proc/device-tree/model");
    const std::string mem_total = read_proc_field("/proc/meminfo", "MemTotal");
    const std::string rmem_max = read_first_line("/proc/sys/net/core/rmem_max");
    const std::string rmem_default = read_first_line("/proc/sys/net/core/rmem_default");

    double uptime_seconds = 0.0;
    {
        std::ifstream file("/proc/uptime");
        if(file) file >> uptime_seconds;
    }
    long temp_millidegrees = -1;
    {
        std::ifstream file("/sys/class/thermal/thermal_zone0/temp");
        if(file) file >> temp_millidegrees;
    }

    qo100::log(
        "==================== QO-100 SDL RECEIVER ====================\n");
    qo100::log("[BOOT] started %s  host=%s\n", time_buffer, hostname);
    qo100::log("[BOOT] %s\n", model.empty() ? "board model unknown" : model.c_str());
    qo100::log("[BOOT] kernel=%s %s arch=%s\n",
        system_info.sysname, system_info.release, system_info.machine);
    qo100::log("[BOOT] cpus=%ld  %s\n",
        static_cast<long>(sysconf(_SC_NPROCESSORS_ONLN)),
        mem_total.empty() ? "MemTotal unknown" : mem_total.c_str());
    char temp_text[16] = "unknown";
    if(temp_millidegrees > 0)
        std::snprintf(temp_text, sizeof(temp_text), "%.1fC", temp_millidegrees / 1000.0);
    qo100::log("[BOOT] system uptime=%.0fs  cpu_temp=%s\n", uptime_seconds, temp_text);
    qo100::log("[BOOT] net.core.rmem_max=%s  rmem_default=%s\n",
        rmem_max.empty() ? "?" : rmem_max.c_str(),
        rmem_default.empty() ? "?" : rmem_default.c_str());
    qo100::log(
        "===============================================================\n");
}

} // namespace

int main(int argc, char ** argv)
{
    qo100::reset_log_clock();
    log_startup_banner();
    const Options options = parse_options(argc, argv);
    const std::string repository_root = repository_directory();
    const std::string settings_path = repository_root + "/qo100_sdl/settings.json";
    const bool settings_file_exists = std::ifstream(settings_path).good();
    qo100::ReceiverSettings receiver_settings =
        qo100::load_receiver_settings(repository_root);
    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        qo100::log("[SDL] init failed: %s\n", SDL_GetError());
        return 1;
    }
    DisplayConfig display = resolve_display_config(
        !options.screenshot.empty(), settings_file_exists, receiver_settings.display_800x480);
    /* Whether the "1024 x 600" choice on SET even fits the real screen -
     * used to grey it out there so a too-big layout can't be picked in the
     * first place. Doesn't apply under QO100_DISPLAY (native == whatever
     * was pinned, so this is trivially true there; that's fine, the SET
     * page is a secondary concern next to an explicit installer override). */
    const bool can_use_1024x600 =
        display.native_width >= kReferenceWidth && display.native_height >= kReferenceHeight;
    if(TTF_Init() != 0) {
        qo100::log("[TTF] init failed: %s\n", TTF_GetError());
        SDL_Quit();
        return 1;
    }

    const Uint32 window_flags = display.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP :
        (!options.screenshot.empty() ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN);
    SDL_Window * window = SDL_CreateWindow("QO-100 DATV Receiver (SDL)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        display.width, display.height, window_flags);
    if(window == nullptr) {
        qo100::log("[SDL] window failed: %s\n", SDL_GetError());
        TTF_Quit();
        SDL_Quit();
        return 1;
    }
    /* Everything here is touch-driven - a visible mouse pointer is just
     * clutter on a kiosk screen, and this Pi likely has no mouse plugged in
     * anyway. Only in fullscreen (the real deployment); QO100_WINDOWED/
     * screenshot mode keep it, since those are for development on an
     * actual desktop with a real mouse. */
    if(display.fullscreen) SDL_ShowCursor(SDL_DISABLE);

    Uint32 renderer_flags = SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC;
    if(!options.screenshot.empty()) renderer_flags = SDL_RENDERER_SOFTWARE;
    SDL_Renderer * renderer = SDL_CreateRenderer(window, -1, renderer_flags);
    if(renderer == nullptr && renderer_flags != SDL_RENDERER_SOFTWARE)
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if(renderer == nullptr) {
        qo100::log("[SDL] renderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        TTF_Quit();
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    /* SDL_WINDOW_FULLSCREEN_DESKTOP always sizes the actual window to match
     * the current desktop mode, ignoring whatever width/height was
     * requested at creation - so picking a resolution smaller than the real
     * screen (the 800x480 choice on SET, previewed here on a genuinely
     * 1024x600 panel; also matters if a physical panel's real size is ever
     * smaller than what auto-detect/settings.json expected) still draws a
     * full, correctly-arranged 800x480 UI, just inside a real window that's
     * actually bigger. Rather than stretching that content or silently
     * overriding the chosen resolution back up to the real screen size
     * (which made the SET page's choice pointless - it kept showing the
     * large layout no matter what was selected), centre it: everything
     * still draws using the chosen display.width/height as before, offset
     * into the middle of the real window via a render viewport, with the
     * true edges of the screen filled in as plain background rather than
     * left undrawn. Touch/mouse coordinates are corrected by the same
     * offset where they're read, further down. */
    int render_offset_x = 0;
    int render_offset_y = 0;
    if(display.fullscreen) {
        int actual_width = display.width;
        int actual_height = display.height;
        SDL_GetRendererOutputSize(renderer, &actual_width, &actual_height);
        render_offset_x = std::max(0, (actual_width - display.width) / 2);
        render_offset_y = std::max(0, (actual_height - display.height) / 2);
    }
    /* Re-applied every frame (not just once here) because SDL's renderer
     * silently resets the viewport to match the full window on some window
     * resize events - which, under Wayland, can arrive a little after the
     * window has already reached its final fullscreen size, undoing a
     * one-time viewport set made right after renderer creation. */
    const SDL_Rect content_viewport{render_offset_x, render_offset_y,
                                    display.width, display.height};

    SDL_RendererInfo renderer_info{};
    SDL_GetRendererInfo(renderer, &renderer_info);
    qo100::log("[SDL] renderer=%s accelerated=%s vsync=%s display=%dx%d\n",
        renderer_info.name != nullptr ? renderer_info.name : "unknown",
        (renderer_info.flags & SDL_RENDERER_ACCELERATED) ? "yes" : "no",
        (renderer_info.flags & SDL_RENDERER_PRESENTVSYNC) ? "yes" : "no",
        display.width, display.height);

    TextCache text(renderer);
    const std::string font_path = executable_directory() + "/Montserrat-Medium.ttf";
    for(int size : {14, 16, 20, 32}) {
        if(!text.load_font(font_path, size)) {
            SDL_DestroyRenderer(renderer);
            SDL_DestroyWindow(window);
            TTF_Quit();
            SDL_Quit();
            return 1;
        }
    }

    const Layout layout(display.width, display.height);
    auto spectrum_texture = std::make_unique<SpectrumTexture>(
        renderer, layout.spectrum_plot.w, layout.spectrum_plot.h, layout.spectrum_max_db);
    if(!spectrum_texture->valid()) {
        qo100::log("[SPECTRUM] texture failed: %s\n", SDL_GetError());
        return 1;
    }

    SpectrumFeed spectrum_feed;
    SpectrumStatus spectrum_status = SpectrumStatus::Connecting;
    std::vector<uint16_t> spectrum_bins;
    bool spectrum_ready = false;
    if(options.offline_spectrum) {
        spectrum_bins = make_offline_spectrum(2048);
        spectrum_texture->update(spectrum_bins);
        spectrum_status = SpectrumStatus::Live;
        spectrum_ready = true;
    }
    else {
        spectrum_feed.start();
    }
    qo100::ChatClient chat_client;
    chat_client.start();

    SDL_Texture * video_texture = nullptr;
    int video_source_width = 0;
    int video_source_height = 0;

    VideoScheduler scheduler;
    const bool use_tuner = !options.no_tuner && !options.demo;
    std::string tuner_product = use_tuner
        ? detect_tuner_product_string() : std::string{};
    TunerPopupKind tuner_popup = !use_tuner
        ? TunerPopupKind::None
        : (tuner_product.empty()
            ? TunerPopupKind::NotFound : TunerPopupKind::Detected);
    if(use_tuner) {
        if(tuner_product.empty())
            qo100::log("[TUNER_USB] no MiniTiouner detected (VID:PID 0403:6010)\n");
        else
            qo100::log("[TUNER_USB] detected product=%s\n", tuner_product.c_str());
    }
    int volume_percent = std::clamp(receiver_settings.audio_volume_percent, 0, 100);
    AudioOutput audio_output;
    if(use_tuner) {
        if(SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
            qo100::log("[AUDIO] SDL initialization failed: %s\n", SDL_GetError());
        else audio_output.open(volume_percent);
    }
    qo100::VideoDecoder video_decoder([&scheduler](VideoFrame && frame) {
        scheduler.push(std::move(frame));
    }, [&audio_output](qo100::AudioChunk && chunk) {
        audio_output.push(std::move(chunk));
    });
    if(use_tuner) video_decoder.start();

    constexpr long beacon_frequency_khz = 741474;
    constexpr long beacon_symbol_rate_ksps = 1500;
    auto longmynd = std::make_unique<qo100::LongmyndProcess>(repository_root);
    qo100::LongmyndClient receiver_client;
    bool receiver_enabled = false;
    if(use_tuner) {
        receiver_enabled = longmynd->start(beacon_frequency_khz, beacon_symbol_rate_ksps);
        if(receiver_enabled) {
            receiver_client.start();
            receiver_client.send_voltage(receiver_settings.lnb_voltage_enabled,
                                         receiver_settings.lnb_voltage_horizontal);
        }
    }
    qo100::ReceiverStatus receiver_status;
    bool receiver_was_locked = false;
    /* longmynd's websocket status has only a single latest-message slot, not
     * a queue (see LongmyndClient::on_monitor) - right after retuning, one
     * message already in flight when the tune command landed can still
     * describe the *previous* signal (still locked, old service name) and
     * briefly overwrite the fresh reset() below before genuinely new status
     * arrives. Discard any "still locked" report until we've seen at least
     * one real unlocked report since the tune - that unlocked report proves
     * we're finally seeing status generated after the retune took effect. */
    bool awaiting_post_tune_unlock = false;
    auto last_tune = Clock::time_point{};
    auto last_spectrum_click = Clock::time_point{};
    bool beacon_return_armed = false;
    auto beacon_return_deadline = Clock::time_point{};
    constexpr auto kBeaconReturnDelay = std::chrono::milliseconds(2500);
    long current_tune_if_khz = beacon_frequency_khz;
    long current_tune_symbol_rate_ksps = beacon_symbol_rate_ksps;
    struct PendingTune {
        double frequency_mhz;
        long if_khz;
        long symbol_rate_ksps;
    };
    std::optional<PendingTune> pending_tune;

    std::atomic<bool> producer_running{options.demo};
    std::thread producer;
    if(options.demo) {
        producer = std::thread([&] {
            const auto start = Clock::now();
            uint64_t index = 0;
            while(producer_running.load(std::memory_order_relaxed)) {
                const int64_t pts_us = static_cast<int64_t>(index) * 20000;
                scheduler.push(make_demo_frame(layout.video_content.w,
                    layout.video_content.h, index, pts_us));
                ++index;
                std::this_thread::sleep_until(start + Microseconds(index * 20000));
            }
        });
    }

    bool running = true;
    AppPage app_page = AppPage::Main;
    bool fullscreen_video = false;
    bool have_video_frame = false;
    int settings_lo_mhz = static_cast<int>(std::lround(receiver_settings.lnb_lo_mhz));
    int settings_voltage_choice = !receiver_settings.lnb_voltage_enabled
        ? 0 : (receiver_settings.lnb_voltage_horizontal ? 2 : 1);
    /* Reflects the resolution actually resolved/clamped for this run
     * (display.width), not the raw saved preference - so a stale
     * settings.json that no longer fits the real screen shows the toggle
     * matching what's really on screen instead of an impossible choice. */
    int settings_display_choice = display.width == 800 ? 1 : 0;
    int settings_exit_behaviour_choice = receiver_settings.exit_full_stop ? 1 : 0;
    std::string chat_nick;
    std::string chat_message;
    ChatInput chat_input = ChatInput::None;
    bool chat_symbols = false;
    bool chat_shifted = false;
    size_t chat_first_visible = 0;
    size_t chat_last_visible = 0;
    bool chat_follow_latest = true;
    bool chat_history_dragging = false;
    bool chat_history_drag_moved = false;
    int chat_drag_start_y = 0;
    bool volume_dragging = false;
    size_t chat_drag_start_first = 0;
    TouchState touch;
    double selected_frequency_mhz = receiver_settings.lnb_lo_mhz +
                                    beacon_frequency_khz / 1000.0;
    SpectrumMarker spectrum_marker{
        SpectrumMarkerKind::Tune, selected_frequency_mhz, Clock::time_point{}};
    bool scan_active = false;
    bool scan_awaiting_signal = false;
    bool scan_parked_on_beacon = false;
    double scan_current_freq_mhz = selected_frequency_mhz;
    auto scan_dwell_deadline = Clock::time_point{};
    auto scan_idle_since = Clock::time_point{};
    auto scan_locked_at = Clock::time_point{};
    auto scan_tuned_at = Clock::time_point{};
    constexpr auto kScanMaxDwell = std::chrono::seconds(30);
    constexpr auto kScanBeaconReturnDelay = std::chrono::seconds(60);
    constexpr double kScanBeaconGuardMhz = 0.1;
    /* Some detected signals are too marginal to ever hold lock (narrowband
     * QO-100 DATV stations right at the noise floor) - without this, scan
     * revisits them at the same priority as everything else forever, so a
     * handful of flickering signals can dominate scan time indefinitely
     * (observed: one 3-hour trial spent ~94% of its time cycling the same
     * ~10 frequencies). A frequency only cools down after repeatedly
     * failing to hold, not on a single drop (could just be bad luck), and
     * the cooldown is short enough that a genuinely new/better signal
     * showing up on the same slot shortly after isn't locked out for long. */
    struct ScanCooldown { double frequency_mhz; int fast_loss_streak; Clock::time_point cooldown_until; };
    std::vector<ScanCooldown> scan_cooldowns;
    constexpr double kScanCooldownMatchMhz = 0.05;
    constexpr int kScanFastLossThreshold = 3;
    constexpr auto kScanFastLossWindow = std::chrono::seconds(5);
    constexpr auto kScanCooldownDuration = std::chrono::seconds(150);
    const auto scan_note_lock_result = [&](double frequency_mhz, bool held_long_enough) {
        auto entry = std::find_if(scan_cooldowns.begin(), scan_cooldowns.end(),
            [&](const ScanCooldown & c) {
                return std::abs(c.frequency_mhz - frequency_mhz) < kScanCooldownMatchMhz;
            });
        if(entry == scan_cooldowns.end()) {
            if(held_long_enough) return;
            scan_cooldowns.push_back({frequency_mhz, 1, Clock::time_point{}});
            return;
        }
        if(held_long_enough) {
            entry->fast_loss_streak = 0;
            entry->cooldown_until = Clock::time_point{};
            return;
        }
        ++entry->fast_loss_streak;
        if(entry->fast_loss_streak >= kScanFastLossThreshold) {
            entry->cooldown_until = Clock::now() + kScanCooldownDuration;
            qo100::log("[SCAN] %.3fMHz failed to hold lock %d times in a row; "
                       "cooling down for %llds\n", frequency_mhz,
                       entry->fast_loss_streak,
                       static_cast<long long>(kScanCooldownDuration.count()));
        }
    };
    const auto scan_on_cooldown = [&](double frequency_mhz) {
        const auto entry = std::find_if(scan_cooldowns.begin(), scan_cooldowns.end(),
            [&](const ScanCooldown & c) {
                return std::abs(c.frequency_mhz - frequency_mhz) < kScanCooldownMatchMhz;
            });
        return entry != scan_cooldowns.end() && Clock::now() < entry->cooldown_until;
    };
    const auto run_started = Clock::now();
    const auto tuner_popup_started_at = run_started;
    VideoNotice video_notice = use_tuner ? VideoNotice::Tuning : VideoNotice::None;
    auto video_notice_started_at = run_started;
    auto last_video_frame_at = run_started;
    uint64_t tune_reopen_before = 0;
    auto last_stats = run_started;
    auto last_present_wall = Clock::time_point{};
    int64_t interval_max_gap_us = 0;
    uint64_t interval_stalls = 0;
    uint64_t previous_presented = 0;
    uint64_t previous_queue_drops = 0;
    uint64_t previous_late_drops = 0;
    const auto apply_tune = [&](const PendingTune & tune, bool was_queued) {
        last_tune = Clock::now();
        scan_tuned_at = last_tune;
        awaiting_post_tune_unlock = true;
        beacon_return_armed = false;
        selected_frequency_mhz = tune.frequency_mhz;
        current_tune_if_khz = tune.if_khz;
        current_tune_symbol_rate_ksps = tune.symbol_rate_ksps;
        spectrum_marker = {
            SpectrumMarkerKind::Tune,
            selected_frequency_mhz,
            Clock::time_point{}};
        receiver_status.reset();
        scheduler.reset();
        have_video_frame = false;
        video_notice = VideoNotice::Tuning;
        video_notice_started_at = Clock::now();
        tune_reopen_before = video_decoder.reopen_count();
        video_decoder.request_reset();
        audio_output.reset();
        receiver_client.send_tune(tune.if_khz, tune.symbol_rate_ksps);
        qo100::log(
            "[TUNE] %s%.3fMHz -> IF=%ldkHz SR=%ldkS/s\n",
            was_queued ? "sending queued " : "",
            selected_frequency_mhz, tune.if_khz, tune.symbol_rate_ksps);
    };
    /* Steps through detected signals above the beacon, ascending, wrapping
     * back to the lowest once nothing further up is currently detected.
     * Re-reads spectrum_texture->signals() fresh each call rather than
     * tracking an index, since detections can appear/disappear between
     * scan steps. */
    const auto scan_advance = [&] {
        if(!scan_active) return;
        const double beacon_mhz = receiver_settings.lnb_lo_mhz +
                                  beacon_frequency_khz / 1000.0;
        /* Two passes: prefer signals that aren't cooling down, but if every
         * detected signal currently is (a band full of marginal stations),
         * fall back to considering all of them rather than sitting idle -
         * by the time a full lap comes back around, the earliest cooldowns
         * will likely have expired anyway. */
        const DetectedSignal * target = nullptr;
        for(const bool respect_cooldown : {true, false}) {
            const DetectedSignal * next = nullptr;
            const DetectedSignal * lowest = nullptr;
            for(const auto & signal : spectrum_texture->signals()) {
                if(signal.frequency_mhz <= beacon_mhz + kScanBeaconGuardMhz) continue;
                if(respect_cooldown && scan_on_cooldown(signal.frequency_mhz)) continue;
                if(lowest == nullptr || signal.frequency_mhz < lowest->frequency_mhz)
                    lowest = &signal;
                if(signal.frequency_mhz > scan_current_freq_mhz &&
                   (next == nullptr || signal.frequency_mhz < next->frequency_mhz))
                    next = &signal;
            }
            target = next != nullptr ? next : lowest;
            if(target != nullptr) break;
        }
        if(target == nullptr) {
            qo100::log(
                "[SCAN] nothing detected above the beacon; retrying shortly\n");
            scan_dwell_deadline = Clock::now() + std::chrono::seconds(2);
            if(!scan_awaiting_signal) scan_idle_since = Clock::now();
            scan_awaiting_signal = true;
            /* Nothing to watch for a while - park on the always-on beacon
             * instead of sitting on a dead frequency showing nothing, but
             * keep scanning in the background so it jumps off as soon as
             * a real signal reappears. */
            if(!scan_parked_on_beacon &&
               Clock::now() - scan_idle_since >= kScanBeaconReturnDelay) {
                scan_parked_on_beacon = true;
                qo100::log(
                    "[SCAN] no signal for %llds; returning to beacon while still searching\n",
                    static_cast<long long>(kScanBeaconReturnDelay.count()));
                apply_tune(PendingTune{
                    beacon_mhz, beacon_frequency_khz, beacon_symbol_rate_ksps}, false);
            }
            return;
        }
        scan_parked_on_beacon = false;
        /* If we're still locked to this exact signal (the max-dwell cap
         * fired but there's nothing else to move to - e.g. only one
         * signal on the band), forcing a retune just interrupts otherwise
         * fine reception for no benefit: it would only land back here.
         * Extend the dwell window and stay put instead. */
        if(receiver_status.locked() &&
           std::abs(target->frequency_mhz - scan_current_freq_mhz) < 0.02) {
            scan_dwell_deadline = Clock::now() + kScanMaxDwell;
            qo100::log(
                "[SCAN] still the only signal in range; staying tuned\n");
            return;
        }
        scan_awaiting_signal = false;
        scan_current_freq_mhz = target->frequency_mhz;
        scan_dwell_deadline = Clock::now() + kScanMaxDwell;
        const long target_if_khz = std::lround(
            (target->frequency_mhz - receiver_settings.lnb_lo_mhz) * 1000.0);
        const long target_symbol_rate_ksps = std::lround(
            target->symbol_rate_ms * 1000.0F);
        qo100::log("[SCAN] moving to %.3fMHz\n", target->frequency_mhz);
        apply_tune(PendingTune{
            target->frequency_mhz, target_if_khz, target_symbol_rate_ksps}, false);
    };
    const auto close_chat_keyboard = [&] {
        chat_input = ChatInput::None;
        chat_symbols = false;
        chat_shifted = false;
        SDL_StopTextInput();
    };
    const auto submit_chat_input = [&] {
        if(chat_input == ChatInput::Nick) {
            if(!chat_nick.empty()) {
                chat_client.set_nick(chat_nick);
                qo100::log("[CHAT_UI] callsign submitted: %s\n", chat_nick.c_str());
            }
        }
        else if(chat_input == ChatInput::Message && !chat_message.empty()) {
            chat_client.send_message(chat_message);
            qo100::log("[CHAT_UI] message submitted (%zu characters)\n",
                       chat_message.size());
            chat_message.clear();
        }
        close_chat_keyboard();
    };
    const auto append_chat_text = [&](const std::string & value) {
        if(chat_input == ChatInput::None) return;
        std::string & target = chat_input == ChatInput::Nick ? chat_nick : chat_message;
        const size_t maximum = chat_input == ChatInput::Nick ? 20U : 300U;
        if(target.size() + value.size() <= maximum) target += value;
    };
    while(running) {
        bool uploaded_frame_this_loop = false;
        SDL_Event event{};
        while(SDL_PollEvent(&event)) {
            /* Undo the centring offset set up around SDL_RenderSetViewport
             * above, so hit-testing (which compares against rects computed
             * in the same 0..display.width/height space everything draws
             * in) sees coordinates relative to the content, not the real,
             * possibly-larger window. */
            if(render_offset_x != 0 || render_offset_y != 0) {
                if(event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP) {
                    event.button.x -= render_offset_x;
                    event.button.y -= render_offset_y;
                }
                else if(event.type == SDL_MOUSEMOTION) {
                    event.motion.x -= render_offset_x;
                    event.motion.y -= render_offset_y;
                }
            }
            /* Tracked separately from each page's own click handling below
             * (which only fires on release) purely so buttons can draw a
             * pressed highlight the instant a finger touches down - placed
             * first, before any of that handling's `continue`s, so it always
             * runs regardless of which page/popup consumes the event. */
            if(event.type == SDL_MOUSEBUTTONDOWN &&
               event.button.button == SDL_BUTTON_LEFT) {
                touch.active = true;
                touch.x = event.button.x;
                touch.y = event.button.y;
            }
            else if(event.type == SDL_MOUSEMOTION && touch.active) {
                touch.x = event.motion.x;
                touch.y = event.motion.y;
            }
            else if(event.type == SDL_MOUSEBUTTONUP &&
                    event.button.button == SDL_BUTTON_LEFT) {
                touch.active = false;
            }
            if(event.type == SDL_QUIT) {
                running = false;
            }
            if(event.type == SDL_TEXTINPUT && app_page == AppPage::Chat)
                append_chat_text(event.text.text);
            if(event.type == SDL_KEYDOWN) {
                if(event.key.keysym.sym == SDLK_ESCAPE) {
                    if(app_page == AppPage::Chat && chat_input != ChatInput::None)
                        close_chat_keyboard();
                    else if(app_page != AppPage::Main) app_page = AppPage::Main;
                    else running = false;
                    continue;
                }
                if(app_page == AppPage::Chat && chat_input != ChatInput::None) {
                    if(event.key.keysym.sym == SDLK_BACKSPACE) {
                        std::string & target = chat_input == ChatInput::Nick
                            ? chat_nick : chat_message;
                        erase_last_utf8(target);
                    }
                    else if(event.key.keysym.sym == SDLK_RETURN ||
                            event.key.keysym.sym == SDLK_KP_ENTER) submit_chat_input();
                }
            }
            if(event.type == SDL_MOUSEWHEEL && app_page == AppPage::Chat &&
               !chat_client.state().lines.empty()) {
                chat_follow_latest = false;
                if(event.wheel.y > 0)
                    chat_first_visible = chat_first_visible > 3
                        ? chat_first_visible - 3 : 0;
                else if(event.wheel.y < 0)
                    chat_first_visible = std::min(
                        chat_first_visible + 3,
                        chat_client.state().lines.size() - 1);
            }
            if(event.type == SDL_MOUSEBUTTONDOWN &&
               event.button.button == SDL_BUTTON_LEFT &&
               app_page == AppPage::Chat) {
                const bool keyboard_open = chat_input != ChatInput::None;
                if(point_in_rect(event.button.x, event.button.y,
                                 chat_history_rect(display.width, display.height,
                                                   keyboard_open))) {
                    chat_history_dragging = true;
                    chat_history_drag_moved = false;
                    chat_drag_start_y = event.button.y;
                    chat_drag_start_first = chat_first_visible;
                }
            }
            if(event.type == SDL_MOUSEMOTION && chat_history_dragging &&
               !chat_client.state().lines.empty()) {
                const int delta_y = event.motion.y - chat_drag_start_y;
                if(std::abs(delta_y) >= 8) chat_history_drag_moved = true;
                const int message_steps = delta_y / 28;
                const int64_t requested = static_cast<int64_t>(chat_drag_start_first) -
                                          message_steps;
                chat_first_visible = static_cast<size_t>(std::clamp<int64_t>(
                    requested, 0,
                    static_cast<int64_t>(chat_client.state().lines.size() - 1)));
                chat_follow_latest = false;
            }
            if(event.type == SDL_MOUSEBUTTONUP) {
                const int x = event.button.x;
                const int y = event.button.y;
                const bool completed_chat_drag = chat_history_dragging &&
                                                 chat_history_drag_moved;
                chat_history_dragging = false;
                if(tuner_popup != TunerPopupKind::None) {
                    if(tuner_popup == TunerPopupKind::NotFound) {
                        const SDL_Rect close_button =
                            tuner_popup_close_rect(display.width, display.height);
                        if(x >= close_button.x && x < close_button.x + close_button.w &&
                           y >= close_button.y && y < close_button.y + close_button.h) {
                            tuner_popup = TunerPopupKind::None;
                            qo100::log("[TUNER_USB] no-tuner popup closed\n");
                        }
                    }
                    continue;
                }
                if(app_page == AppPage::Settings) {
                    if(point_in_rect(x, y, settings_exit_page_rect(display.width))) {
                        app_page = AppPage::Main;
                        qo100::log("[SETTINGS_UI] back to main\n");
                    }
                    else if(point_in_rect(
                                x, y, settings_lo_button_rect(display.width, false))) {
                        settings_lo_mhz = std::max(1000, settings_lo_mhz - 1);
                    }
                    else if(point_in_rect(
                                x, y, settings_lo_button_rect(display.width, true))) {
                        settings_lo_mhz = std::min(20000, settings_lo_mhz + 1);
                    }
                    else if(point_in_rect(x, y, settings_save_rect(display.width))) {
                        receiver_settings.lnb_lo_mhz = settings_lo_mhz;
                        receiver_settings.lnb_voltage_enabled =
                            settings_voltage_choice != 0;
                        receiver_settings.lnb_voltage_horizontal =
                            settings_voltage_choice == 2;
                        receiver_settings.audio_volume_percent = volume_percent;
                        receiver_settings.display_800x480 = settings_display_choice == 1;
                        receiver_settings.exit_full_stop = settings_exit_behaviour_choice == 1;
                        const bool display_change_pending =
                            receiver_settings.display_800x480 != (display.width == 800);
                        const bool applied = qo100::save_receiver_settings(
                            repository_root, receiver_settings);
                        if(applied) {
                            receiver_client.send_voltage(
                                receiver_settings.lnb_voltage_enabled,
                                receiver_settings.lnb_voltage_horizontal);
                            selected_frequency_mhz = receiver_settings.lnb_lo_mhz +
                                current_tune_if_khz / 1000.0;
                            qo100::log(
                                "[SETTINGS_UI] applied LO=%dMHz voltage=%s display=%s exit=%s\n",
                                settings_lo_mhz,
                                settings_voltage_choice == 0 ? "OFF" :
                                    (settings_voltage_choice == 1 ? "13V" : "18V"),
                                settings_display_choice == 1 ? "800x480" : "1024x600",
                                settings_exit_behaviour_choice == 1 ? "full-stop" : "restart");
                            /* Resolution is only picked up at process start
                             * (qo100_sdl/service_launch.sh reads settings.json
                             * fresh each launch) - restart to apply, the same
                             * way EXIT does. Restart=always in the systemd
                             * unit brings it back up in the new size. */
                            if(display_change_pending) {
                                qo100::log(
                                    "[SETTINGS_UI] display resolution changed; "
                                    "restarting to apply\n");
                                running = false;
                            }
                        }
                        else {
                            qo100::log("[SETTINGS_UI] failed to save settings.json\n");
                        }
                        /* Closing the page is itself the confirmation - no
                         * separate "Saved" message, which would flash for
                         * under a frame (or not at all) when a resolution
                         * change is about to tear the app down anyway. */
                        app_page = AppPage::Main;
                    }
                    else {
                        bool matched = false;
                        for(int index = 0; index < 3; ++index) {
                            if(point_in_rect(x, y,
                                             settings_voltage_rect(display.width, index))) {
                                settings_voltage_choice = index;
                                matched = true;
                                break;
                            }
                        }
                        for(int index = 0; !matched && index < 2; ++index) {
                            if(index == 0 && !can_use_1024x600) continue;
                            if(point_in_rect(x, y,
                                             settings_display_res_rect(display.width, index))) {
                                settings_display_choice = index;
                                matched = true;
                                break;
                            }
                        }
                        for(int index = 0; !matched && index < 2; ++index) {
                            if(point_in_rect(x, y,
                                             settings_exit_behaviour_rect(display.width, index))) {
                                settings_exit_behaviour_choice = index;
                                matched = true;
                                break;
                            }
                        }
                        for(int index = 0; !matched && index < 2; ++index) {
                            if(point_in_rect(x, y,
                                             settings_autostart_rect(display.width, index))) {
                                /* Takes effect immediately, unlike the rest of
                                 * this page - it's a standalone boot-time
                                 * policy, not part of receiver_settings, so
                                 * there's nothing for SAVE & APPLY to do here. */
                                if(index == 0) {
                                    qo100::log("[SETTINGS_UI] autostart enabled\n");
                                    std::system(("QO100_SKIP_SERVICE_START=1 '" +
                                                 repository_root +
                                                 "/scripts/setup_autostart.sh' &").c_str());
                                }
                                else {
                                    qo100::log("[SETTINGS_UI] autostart disabled\n");
                                    std::system(("rm -f '" +
                                                 autostart_desktop_file_path() + "'").c_str());
                                }
                                break;
                            }
                        }
                    }
                    continue;
                }
                if(app_page == AppPage::Chat) {
                    if(completed_chat_drag) continue;
                    const bool keyboard_open = chat_input != ChatInput::None;
                    if(point_in_rect(x, y, page_back_rect(display.width))) {
                        close_chat_keyboard();
                        app_page = AppPage::Main;
                        qo100::log("[CHAT_UI] back to main\n");
                        continue;
                    }
                    if(keyboard_open) {
                        bool key_pressed = false;
                        for(const KeyboardKey & key : keyboard_keys(
                                display.width, display.height,
                                chat_symbols, chat_shifted)) {
                            if(!point_in_rect(x, y, key.rect)) continue;
                            key_pressed = true;
                            if(key.label == "SHIFT") chat_shifted = !chat_shifted;
                            else if(key.label == "SYM") {
                                chat_symbols = true;
                                chat_shifted = false;
                            }
                            else if(key.label == "ABC") chat_symbols = false;
                            else if(key.label == "BACK") {
                                std::string & target = chat_input == ChatInput::Nick
                                    ? chat_nick : chat_message;
                                erase_last_utf8(target);
                            }
                            else if(key.label == "CANCEL") close_chat_keyboard();
                            else if(key.label == "ENTER") submit_chat_input();
                            else if(key.label == "SPACE") append_chat_text(" ");
                            else append_chat_text(key.label);
                            break;
                        }
                        if(key_pressed) continue;
                    }
                    if(point_in_rect(x, y, chat_nick_rect(display.height, keyboard_open))) {
                        chat_input = ChatInput::Nick;
                        chat_symbols = false;
                        chat_shifted = false;
                        SDL_StartTextInput();
                    }
                    else if(point_in_rect(x, y,
                                         chat_message_rect(display.width, display.height,
                                                           keyboard_open))) {
                        chat_input = ChatInput::Message;
                        chat_symbols = false;
                        chat_shifted = false;
                        SDL_StartTextInput();
                    }
                    else if(point_in_rect(x, y, chat_set_rect(display.height, keyboard_open))) {
                        if(!chat_nick.empty()) chat_client.set_nick(chat_nick);
                    }
                    else if(point_in_rect(x, y,
                                         chat_send_rect(display.width, display.height,
                                                        keyboard_open))) {
                        if(!chat_message.empty()) {
                            chat_client.send_message(chat_message);
                            chat_message.clear();
                        }
                    }
                    continue;
                }
                if(fullscreen_video) {
                    fullscreen_video = false;
                    qo100::log(
                        "[VIDEO_UI] fullscreen -> main tap=(%d,%d); decoder unchanged\n",
                        x, y);
                    continue;
                }
                const SDL_Rect chat_button = status_button_rect(layout, 0);
                if(point_in_rect(x, y, chat_button)) {
                    app_page = AppPage::Chat;
                    chat_follow_latest = true;
                    chat_history_dragging = false;
                    qo100::log("[CHAT_UI] opened\n");
                    continue;
                }
                const SDL_Rect settings_button = status_button_rect(layout, 1);
                if(point_in_rect(x, y, settings_button)) {
                    settings_lo_mhz = static_cast<int>(
                        std::lround(receiver_settings.lnb_lo_mhz));
                    settings_voltage_choice = !receiver_settings.lnb_voltage_enabled
                        ? 0 : (receiver_settings.lnb_voltage_horizontal ? 2 : 1);
                    settings_display_choice = display.width == 800 ? 1 : 0;
                    settings_exit_behaviour_choice = receiver_settings.exit_full_stop ? 1 : 0;
                    tuner_product = detect_tuner_product_string();
                    app_page = AppPage::Settings;
                    qo100::log("[SETTINGS_UI] opened\n");
                    continue;
                }
                const SDL_Rect scan_button = status_button_rect(layout, 2);
                if(point_in_rect(x, y, scan_button)) {
                    scan_active = !scan_active;
                    if(scan_active) {
                        qo100::log("[SCAN] started\n");
                        scan_current_freq_mhz = receiver_settings.lnb_lo_mhz +
                                                beacon_frequency_khz / 1000.0;
                        scan_parked_on_beacon = false;
                        scan_awaiting_signal = false;
                        scan_advance();
                    }
                    else qo100::log("[SCAN] stopped\n");
                    continue;
                }
                const SDL_Rect exit_button = status_button_rect(layout, 3);
                if(x >= exit_button.x && x < exit_button.x + exit_button.w &&
                   y >= exit_button.y && y < exit_button.y + exit_button.h) {
                    if(receiver_settings.exit_full_stop) {
                        /* systemctl stop is a real stop regardless of the
                         * unit's Restart=always - unlike just exiting
                         * cleanly, which Restart=always brings straight
                         * back up. No-op (harmless) when not actually
                         * running as the systemd service, e.g. a manual
                         * build_and_run.sh session. */
                        qo100::log(
                            "[APP] EXIT tapped; stopping qo100datv.service "
                            "(Full Stop selected)\n");
                        std::system("systemctl --user stop qo100datv.service &");
                    }
                    else {
                        qo100::log(
                            "[APP] EXIT tapped; closing application (system remains running)\n");
                    }
                    running = false;
                    continue;
                }
                if(x >= layout.spectrum_plot.x &&
                   x < layout.spectrum_plot.x + layout.spectrum_plot.w &&
                   y >= layout.spectrum_plot.y &&
                   y < layout.spectrum_plot.y + layout.spectrum_plot.h &&
                   Clock::now() - last_spectrum_click >= std::chrono::milliseconds(400)) {
                    last_spectrum_click = Clock::now();
                    const double clicked = kSpectrumStartMhz +
                        static_cast<double>(x - layout.spectrum_plot.x) /
                        layout.spectrum_plot.w * kSpectrumSpanMhz;
                    const DetectedSignal * selected_signal = nullptr;
                    for(const auto & signal : spectrum_texture->signals()) {
                        const double half_width = std::max(0.02,
                            static_cast<double>(signal.measured_width_mhz) / 2.0);
                        if(std::abs(clicked - signal.frequency_mhz) <= half_width) {
                            selected_signal = &signal;
                            break;
                        }
                    }
                    if(selected_signal == nullptr) {
                        spectrum_marker.kind = spectrum_ready
                            ? SpectrumMarkerKind::NoSignal
                            : SpectrumMarkerKind::SpectrumNotReady;
                        spectrum_marker.frequency_mhz = clicked;
                        spectrum_marker.expires_at = Clock::now() +
                            std::chrono::seconds(1);
                        qo100::log(
                            "[TUNE] %s at %.3fMHz; not tuning (notice for 1000ms)\n",
                            spectrum_ready ? "no detected signal" : "spectrum not ready",
                            clicked);
                    }
                    else {
                        if(scan_active) {
                            scan_active = false;
                            qo100::log("[SCAN] cancelled by manual selection\n");
                        }
                        const double target_frequency_mhz = selected_signal->frequency_mhz;
                        const long target_if_khz = std::lround(
                            (target_frequency_mhz - receiver_settings.lnb_lo_mhz) * 1000.0);
                        const long target_symbol_rate_ksps = std::lround(
                            selected_signal->symbol_rate_ms * 1000.0F);
                        const long same_signal_tolerance_khz = std::lround(
                            std::max(0.02,
                                static_cast<double>(selected_signal->measured_width_mhz) / 2.0) *
                            1000.0);
                        const bool already_tuned =
                            std::labs(target_if_khz - current_tune_if_khz) <=
                                same_signal_tolerance_khz &&
                            std::labs(target_symbol_rate_ksps -
                                      current_tune_symbol_rate_ksps) <= 10;
                        const bool tune_still_waiting_for_video =
                            video_notice == VideoNotice::Tuning ||
                            video_notice == VideoNotice::NoVideoStream;
                        if(already_tuned && !pending_tune.has_value() &&
                           tune_still_waiting_for_video) {
                            spectrum_marker = {
                                SpectrumMarkerKind::AlreadyTuned,
                                target_frequency_mhz,
                                Clock::now() + std::chrono::seconds(1)};
                            qo100::log(
                                "[TUNE] %.3fMHz is already selected; still waiting for video\n",
                                target_frequency_mhz);
                        }
                        else if(already_tuned) {
                            pending_tune.reset();
                            video_notice = VideoNotice::None;
                            selected_frequency_mhz = target_frequency_mhz;
                            spectrum_marker = {
                                SpectrumMarkerKind::AlreadyTuned,
                                selected_frequency_mhz,
                                Clock::now() + std::chrono::seconds(1)};
                            qo100::log(
                                "[TUNE] already tuned %.3fMHz IF=%ldkHz SR=%ldkS/s; "
                                "decoder unchanged (notice for 1000ms)\n",
                                selected_frequency_mhz, current_tune_if_khz,
                                current_tune_symbol_rate_ksps);
                        }
                        else if(pending_tune.has_value() &&
                                std::labs(target_if_khz - pending_tune->if_khz) <=
                                    same_signal_tolerance_khz &&
                                std::labs(target_symbol_rate_ksps -
                                          pending_tune->symbol_rate_ksps) <= 10) {
                            spectrum_marker = {
                                SpectrumMarkerKind::TuneQueued,
                                pending_tune->frequency_mhz,
                                Clock::time_point{}};
                            qo100::log(
                                "[TUNE] %.3fMHz already queued; waiting for control connection\n",
                                pending_tune->frequency_mhz);
                        }
                        else if(!receiver_enabled) {
                            qo100::log(
                                "[TUNE] selected %.3fMHz but Longmynd is not running\n",
                                target_frequency_mhz);
                        }
                        else if(!receiver_client.control_connected()) {
                            pending_tune = PendingTune{
                                target_frequency_mhz,
                                target_if_khz,
                                target_symbol_rate_ksps};
                            spectrum_marker = {
                                SpectrumMarkerKind::TuneQueued,
                                target_frequency_mhz,
                                Clock::time_point{}};
                            video_notice = VideoNotice::WaitingForTuner;
                            qo100::log(
                                "[TUNE] queued %.3fMHz IF=%ldkHz SR=%ldkS/s; "
                                "waiting for control connection\n",
                                target_frequency_mhz, target_if_khz,
                                target_symbol_rate_ksps);
                        }
                        else if(Clock::now() - last_tune < std::chrono::milliseconds(1500)) {
                            qo100::log(
                                "[TUNE] selection ignored inside debounce window\n");
                        }
                        else {
                            apply_tune(PendingTune{
                                target_frequency_mhz,
                                target_if_khz,
                                target_symbol_rate_ksps}, false);
                        }
                    }
                }
                if(x >= layout.video_panel.x && x < layout.video_panel.x + layout.video_panel.w &&
                   y >= layout.video_panel.y && y < layout.video_panel.y + layout.video_panel.h) {
                    fullscreen_video = true;
                    qo100::log(
                        "[VIDEO_UI] main -> fullscreen tap=(%d,%d); decoder unchanged\n",
                        x, y);
                }
                if(volume_dragging) {
                    volume_percent = volume_from_x(volume_track_rect(layout), x);
                    audio_output.set_volume(volume_percent);
                    receiver_settings.audio_volume_percent = volume_percent;
                    qo100::save_receiver_settings(repository_root, receiver_settings);
                    qo100::log("[AUDIO] volume=%d%%\n", volume_percent);
                }
                volume_dragging = false;
            }
            if(event.type == SDL_MOUSEBUTTONDOWN &&
               event.button.button == SDL_BUTTON_LEFT && app_page == AppPage::Main) {
                const SDL_Rect track = volume_track_rect(layout);
                /* Generous hit band around the (visually thin) 7px track -
                 * the width already spans nearly the whole panel, height is
                 * the part that was too tight to comfortably land a finger
                 * on. */
                if(event.button.y >= track.y - 16 && event.button.y <= track.y + 23 &&
                   event.button.x >= track.x && event.button.x <= track.x + track.w) {
                    volume_dragging = true;
                    volume_percent = volume_from_x(track, event.button.x);
                    audio_output.set_volume(volume_percent);
                }
            }
            if(event.type == SDL_MOUSEMOTION && volume_dragging) {
                volume_percent = volume_from_x(volume_track_rect(layout), event.motion.x);
                audio_output.set_volume(volume_percent);
            }
        }

        if(tuner_popup == TunerPopupKind::Detected &&
           Clock::now() - tuner_popup_started_at >= std::chrono::seconds(3)) {
            tuner_popup = TunerPopupKind::None;
            qo100::log("[TUNER_USB] detected popup closed after 3 seconds\n");
        }

        chat_client.consume();

        if(!options.offline_spectrum &&
           spectrum_feed.consume(spectrum_bins, spectrum_status)) {
            spectrum_texture->update(spectrum_bins);
            spectrum_ready = true;
        }

        if(receiver_enabled && receiver_client.consume_status(receiver_status)) {
            if(awaiting_post_tune_unlock) {
                if(receiver_status.locked()) {
                    /* Stale pre-tune message - undo it rather than let it
                     * flash the old lock/service name back onto the display. */
                    receiver_status.reset();
                }
                else {
                    awaiting_post_tune_unlock = false;
                }
            }
            const bool locked_now = receiver_status.locked();
            if(locked_now && !receiver_was_locked) {
                scan_locked_at = Clock::now();
                const std::string service = !receiver_status.service_name.empty()
                    ? receiver_status.service_name : receiver_status.service_provider;
                qo100::log(
                    "[TUNE] lock: %s IF=%ldkHz SR=%ldkS/s MER=%.1fdB service=%s\n",
                    receiver_status.demod_state == 4 ? "DVB-S2" : "DVB-S",
                    receiver_status.carrier_khz, receiver_status.symbol_rate_ksps,
                    receiver_status.mer_x10 / 10.0,
                    service.empty() ? "---" : service.c_str());
                if(beacon_return_armed) {
                    beacon_return_armed = false;
                    qo100::log(
                        "[TUNE] lock regained; cancelled pending return to beacon\n");
                }
            }
            else if(!locked_now && receiver_was_locked) {
                qo100::log( "[TUNE] lock lost\n");
                /* scan_locked_at <= scan_tuned_at means this edge is just the
                 * reset-induced drop from the *previous* scan step catching
                 * up (status updates arrive on their own cadence, not every
                 * frame) - the current frequency was never actually locked
                 * in the first place, so it hasn't failed anything yet.
                 * Advancing here too would skip it without a real try. */
                if(scan_active && scan_locked_at > scan_tuned_at) {
                    const auto held = Clock::now() - scan_locked_at;
                    qo100::log("[SCAN] lock lost after %.1fs; advancing\n",
                               std::chrono::duration<double>(held).count());
                    scan_note_lock_result(scan_current_freq_mhz, held >= kScanFastLossWindow);
                    scan_advance();
                }
                else if(!scan_active) {
                    const double beacon_mhz = receiver_settings.lnb_lo_mhz +
                                              beacon_frequency_khz / 1000.0;
                    if(std::abs(selected_frequency_mhz - beacon_mhz) > 0.02) {
                        beacon_return_armed = true;
                        beacon_return_deadline = Clock::now() + kBeaconReturnDelay;
                        qo100::log(
                            "[TUNE] lock lost on non-beacon signal; returning to "
                            "beacon in %lldms unless it comes back\n",
                            static_cast<long long>(kBeaconReturnDelay.count()));
                    }
                }
            }
            receiver_was_locked = locked_now;
        }

        if(beacon_return_armed && Clock::now() >= beacon_return_deadline) {
            beacon_return_armed = false;
            const double beacon_mhz = receiver_settings.lnb_lo_mhz +
                                      beacon_frequency_khz / 1000.0;
            qo100::log("[TUNE] returning to beacon\n");
            apply_tune(PendingTune{
                beacon_mhz, beacon_frequency_khz, beacon_symbol_rate_ksps}, false);
        }

        /* Safety cap: move on even if still locked, so one long-running
         * transmission can't hold the scan in place indefinitely. */
        if(scan_active && Clock::now() >= scan_dwell_deadline) {
            if(!scan_awaiting_signal) {
                qo100::log("[SCAN] max dwell reached; advancing\n");
                /* Held the full dwell (whether or not still locked this
                 * instant) - that's a pass, not a fast-loss candidate. */
                scan_note_lock_result(scan_current_freq_mhz, true);
            }
            scan_advance();
        }

        if(pending_tune.has_value() && receiver_client.control_connected()) {
            const PendingTune tune = *pending_tune;
            pending_tune.reset();
            apply_tune(tune, true);
        }

        if(video_notice == VideoNotice::Tuning &&
           Clock::now() - video_notice_started_at >= std::chrono::seconds(15)) {
            video_notice = VideoNotice::NoVideoStream;
            qo100::log(
                "[TUNE] no video stream received within 15 seconds; staying tuned\n");
        }

        if(spectrum_marker.kind == SpectrumMarkerKind::AlreadyTuned &&
           Clock::now() >= spectrum_marker.expires_at) {
            spectrum_marker = {
                have_video_frame && video_notice == VideoNotice::None
                    ? SpectrumMarkerKind::None
                    : SpectrumMarkerKind::Tune,
                selected_frequency_mhz,
                Clock::time_point{}};
        }
        else if((spectrum_marker.kind == SpectrumMarkerKind::NoSignal ||
            spectrum_marker.kind == SpectrumMarkerKind::SpectrumNotReady) &&
           Clock::now() >= spectrum_marker.expires_at) {
            spectrum_marker.kind = SpectrumMarkerKind::None;
        }

        if(auto frame = scheduler.take_due(Clock::now())) {
            if(video_texture == nullptr || frame->width != video_source_width ||
               frame->height != video_source_height) {
                SDL_DestroyTexture(video_texture);
                video_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV,
                    SDL_TEXTUREACCESS_STREAMING, frame->width, frame->height);
                if(video_texture != nullptr) {
                    SDL_SetTextureScaleMode(video_texture, SDL_ScaleModeLinear);
                    video_source_width = frame->width;
                    video_source_height = frame->height;
                    qo100::log( "[VIDEO] texture=%dx%d IYUV\n",
                                 video_source_width, video_source_height);
                }
                else {
                    qo100::log( "[VIDEO] texture failed: %s\n", SDL_GetError());
                    video_source_width = 0;
                    video_source_height = 0;
                }
            }
            if(video_texture != nullptr &&
               SDL_UpdateYUVTexture(video_texture, nullptr,
                   frame->y_plane(), frame->y_pitch,
                   frame->u_plane(), frame->uv_pitch,
                   frame->v_plane(), frame->uv_pitch) == 0) {
                have_video_frame = true;
                uploaded_frame_this_loop = true;
                last_video_frame_at = Clock::now();
                if(spectrum_marker.kind == SpectrumMarkerKind::Tune)
                    spectrum_marker.kind = SpectrumMarkerKind::None;
                if((video_notice == VideoNotice::Tuning ||
                    video_notice == VideoNotice::NoVideoStream) &&
                   video_decoder.reopen_count() > tune_reopen_before) {
                    const double acquisition_seconds = std::chrono::duration<double>(
                        Clock::now() - video_notice_started_at).count();
                    video_notice = VideoNotice::None;
                    qo100::log(
                        "[TUNE] video acquired after %.1fs\n",
                        acquisition_seconds);
                }
            }
        }

        if(use_tuner && video_notice == VideoNotice::None && have_video_frame &&
           Clock::now() - last_video_frame_at >= std::chrono::seconds(8)) {
            scheduler.reset();
            have_video_frame = false;
            video_notice = VideoNotice::NoVideoStream;
            qo100::log(
                "[TUNE] video stream stopped for 8 seconds; staying tuned\n");
        }

        set_colour(renderer, kBackground);
        SDL_RenderClear(renderer);
        SDL_RenderSetViewport(renderer, &content_viewport);
        if(app_page == AppPage::Settings) {
            draw_settings_page(renderer, text, display.width, display.height,
                               settings_lo_mhz, settings_voltage_choice,
                               settings_display_choice, settings_exit_behaviour_choice,
                               tuner_product, receiver_client.monitor_connected(),
                               can_use_1024x600, touch);
        }
        else if(app_page == AppPage::Chat) {
            draw_chat_page(renderer, text, display.width, display.height,
                           chat_client.state(), chat_nick, chat_message,
                           chat_input, chat_symbols, chat_shifted,
                           chat_first_visible, chat_last_visible,
                           chat_follow_latest, touch);
        }
        else if(fullscreen_video) {
            if(have_video_frame) {
                const SDL_Rect screen_bounds{0, 0, display.width, display.height};
                const SDL_Rect destination = aspect_fit(
                    video_source_width, video_source_height, screen_bounds);
                SDL_RenderCopy(renderer, video_texture, nullptr, &destination);
            }
            const SDL_Rect screen_bounds{0, 0, display.width, display.height};
            draw_video_notice(text, screen_bounds, video_notice,
                std::chrono::duration<double>(
                    Clock::now() - video_notice_started_at).count());
        }
        else {
            draw_spectrum(renderer, text, layout, *spectrum_texture,
                          spectrum_status, spectrum_marker, receiver_status,
                          selected_frequency_mhz);
            fill_panel(renderer, layout.video_panel);
            set_colour(renderer, kPanel);
            SDL_RenderFillRect(renderer, &layout.video_content);
            if(have_video_frame) {
                const SDL_Rect destination = aspect_fit(
                    video_source_width, video_source_height, layout.video_content);
                SDL_RenderCopy(renderer, video_texture, nullptr, &destination);
            }
            draw_video_notice(text, layout.video_content, video_notice,
                std::chrono::duration<double>(
                    Clock::now() - video_notice_started_at).count());
            const std::string video_codec = options.demo ? "DEMO" : video_decoder.codec_name();
            const std::string audio_codec = video_decoder.audio_codec_name();
            draw_status(renderer, text, layout, volume_percent, audio_output.peak_percent(),
                        receiver_status,
                        receiver_client.monitor_connected(),
                        receiver_client.received_updates() != 0,
                        selected_frequency_mhz, current_tune_if_khz,
                        current_tune_symbol_rate_ksps,
                        video_codec, audio_codec, scan_active, touch);
        }
        draw_tuner_popup(renderer, text, display.width, display.height,
                         tuner_popup, tuner_product, touch);
        SDL_RenderPresent(renderer);

        const auto now = Clock::now();
        if(uploaded_frame_this_loop) {
            if(last_present_wall.time_since_epoch().count() != 0) {
                const int64_t gap_us = std::chrono::duration_cast<Microseconds>(
                    now - last_present_wall).count();
                interval_max_gap_us = std::max(interval_max_gap_us, gap_us);
                if(gap_us > 60000) ++interval_stalls;
            }
            last_present_wall = now;
        }
        if(now - last_stats >= std::chrono::seconds(5)) {
            const auto stats = scheduler.stats();
            const double window_seconds = std::chrono::duration<double>(now - last_stats).count();
            const uint64_t presented_delta = stats.presented - previous_presented;
            const uint64_t queue_drop_delta = stats.queue_drops - previous_queue_drops;
            const uint64_t late_drop_delta = stats.late_drops - previous_late_drops;
            qo100::log(
                "[PRESENT] fps=%.1f drop=%llu late=%llu max_gap=%.1fms stalls=%llu "
                "shown=%llu rebases=%llu depth=%zu "
                 "spectrum=%llu replaced=%llu "
                 "tuner_status=%s updates=%llu overwritten=%llu tune_control=%s "
                 "decode=%llu reopen=%llu errors=%llu "
                 "audio_chunks=%llu audio_q=%ums audio_drop=%llu "
                 "audio_under=%llu audio_rebuffer=%llu audio_errors=%llu "
                 "textures=%zu rss=%ldkB cpu_temp=%.1fC\n",
                presented_delta / window_seconds,
                static_cast<unsigned long long>(queue_drop_delta),
                static_cast<unsigned long long>(late_drop_delta),
                interval_max_gap_us / 1000.0,
                static_cast<unsigned long long>(interval_stalls),
                static_cast<unsigned long long>(stats.presented),
                static_cast<unsigned long long>(stats.rebases), stats.depth,
                static_cast<unsigned long long>(spectrum_feed.received_frames()),
                static_cast<unsigned long long>(spectrum_feed.replaced_frames()),
                receiver_client.monitor_connected() ? "CONNECTED" : "DISCONNECTED",
                static_cast<unsigned long long>(receiver_client.received_updates()),
                static_cast<unsigned long long>(receiver_client.replaced_updates()),
                receiver_client.control_connected() ? "CONNECTED" : "DISCONNECTED",
                 static_cast<unsigned long long>(video_decoder.decoded_frames()),
                 static_cast<unsigned long long>(video_decoder.reopen_count()),
                 static_cast<unsigned long long>(video_decoder.decode_errors()),
                 static_cast<unsigned long long>(video_decoder.decoded_audio_chunks()),
                 audio_output.queued_ms(),
                 static_cast<unsigned long long>(audio_output.dropped_chunks()),
                 static_cast<unsigned long long>(audio_output.underruns()),
                 static_cast<unsigned long long>(audio_output.rebuffers()),
                 static_cast<unsigned long long>(video_decoder.audio_decode_errors()),
                 text.texture_count(), process_rss_kb(), cpu_temperature_c());
            previous_presented = stats.presented;
            previous_queue_drops = stats.queue_drops;
            previous_late_drops = stats.late_drops;
            interval_max_gap_us = 0;
            interval_stalls = 0;
            last_stats = now;
        }

        if(options.seconds > 0 && now - run_started >= std::chrono::seconds(options.seconds))
            running = false;
        if(!options.screenshot.empty() && options.seconds == 0 &&
           ((spectrum_ready && (!options.demo || have_video_frame)) ||
            now - run_started >= std::chrono::seconds(5)))
            running = false;
        if((renderer_info.flags & SDL_RENDERER_PRESENTVSYNC) == 0) SDL_Delay(2);
    }

    SDL_StopTextInput();
    chat_client.stop();
    spectrum_feed.stop();
    video_decoder.stop();
    const bool audio_closed_cleanly = audio_output.close();
    receiver_client.stop();
    longmynd->stop();

    if(!options.screenshot.empty()) {
        set_colour(renderer, kBackground);
        SDL_RenderClear(renderer);
        draw_spectrum(renderer, text, layout, *spectrum_texture,
                      spectrum_status, spectrum_marker, receiver_status,
                      selected_frequency_mhz);
        fill_panel(renderer, layout.video_panel);
        set_colour(renderer, kPanel);
        SDL_RenderFillRect(renderer, &layout.video_content);
        if(have_video_frame) {
            const SDL_Rect destination = aspect_fit(
                video_source_width, video_source_height, layout.video_content);
            SDL_RenderCopy(renderer, video_texture, nullptr, &destination);
        }
        const std::string video_codec = options.demo ? "DEMO" : video_decoder.codec_name();
        const std::string audio_codec = video_decoder.audio_codec_name();
        draw_status(renderer, text, layout, volume_percent, audio_output.peak_percent(),
                    receiver_status,
                    receiver_client.monitor_connected(),
                    receiver_client.received_updates() != 0,
                    selected_frequency_mhz, current_tune_if_khz,
                    current_tune_symbol_rate_ksps,
                    video_codec, audio_codec, scan_active, TouchState{});
        SDL_RenderPresent(renderer);
        if(!save_screenshot(renderer, display.width, display.height, options.screenshot))
            qo100::log( "[SCREENSHOT] failed to save %s\n", options.screenshot.c_str());
        else
            qo100::log( "[SCREENSHOT] saved %s\n", options.screenshot.c_str());
    }

    producer_running = false;
    if(producer.joinable()) producer.join();
    const auto stats = scheduler.stats();
    qo100::log(
        "[PRESENT] final shown=%llu queue_drops=%llu late_drops=%llu rebases=%llu depth=%zu\n",
        static_cast<unsigned long long>(stats.presented),
        static_cast<unsigned long long>(stats.queue_drops),
        static_cast<unsigned long long>(stats.late_drops),
        static_cast<unsigned long long>(stats.rebases), stats.depth);

    SDL_DestroyTexture(video_texture);
    spectrum_texture.reset();
    text.clear();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    if(!audio_closed_cleanly) {
        /* SDL_Quit() tears down every SDL subsystem, including audio - and
         * the abandoned SDL_CloseAudioDevice() call from AudioOutput::close()
         * is still in there on its own thread, forever, holding SDL's
         * internal audio lock. Calling SDL_Quit() here would deadlock the
         * same way (confirmed in practice: the process sat in futex_do_wait
         * indefinitely, invisible to systemd since nothing was left to
         * SIGKILL it under EXIT's "Restart" mode). Skip it and exit
         * immediately instead - the OS reclaims everything on process exit
         * regardless of whether SDL considers itself cleanly shut down. */
        qo100::log("[SDL] skipping SDL_Quit() - audio close never finished\n");
        std::_Exit(0);
    }
    SDL_Quit();
    return 0;
}
