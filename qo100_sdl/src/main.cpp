#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <libwebsockets.h>
#ifndef LWS_PROTOCOL_LIST_TERM
#define LWS_PROTOCOL_LIST_TERM {nullptr, nullptr, 0, 0, 0, nullptr, 0}
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <limits.h>
#include <unistd.h>

#include "receiver.h"
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

struct DisplayConfig {
    int width = kReferenceWidth;
    int height = kReferenceHeight;
    bool fullscreen = true;
};

DisplayConfig resolve_display_config(bool screenshot_mode)
{
    DisplayConfig config;
    if(const char * value = std::getenv("QO100_DISPLAY")) {
        int width = 0;
        int height = 0;
        if(std::sscanf(value, "%dx%d", &width, &height) == 2 && width > 0 && height > 0) {
            config.width = width;
            config.height = height;
        }
    }
    if(std::getenv("QO100_WINDOWED") != nullptr || screenshot_mode) config.fullscreen = false;
    return config;
}

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
        fonts_.clear();
    }

    bool load_font(const std::string & path, int size)
    {
        if(fonts_.count(size) != 0) return true;
        TTF_Font * font = TTF_OpenFont(path.c_str(), size);
        if(font == nullptr) {
            std::fprintf(stderr, "[FONT] cannot open %s at %dpx: %s\n",
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
            CachedText cached{texture, surface->w, surface->h};
            SDL_FreeSurface(surface);
            if(texture == nullptr) return;
            found = textures_.emplace(key, cached).first;
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

private:
    struct CachedText {
        SDL_Texture * texture = nullptr;
        int width = 0;
        int height = 0;
    };

    SDL_Renderer * renderer_ = nullptr;
    std::unordered_map<int, TTF_Font *> fonts_;
    std::unordered_map<std::string, CachedText> textures_;
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
            // A short bounded wait preserves a contiguous PTS sequence when
            // FFmpeg releases a probe/decode burst. Its UDP FIFO continues
            // receiving while this decoder thread waits. If presentation is
            // genuinely behind after one frame interval, reject the arrival.
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

struct Layout {
    int width;
    int height;
    int spectrum_height;
    int bottom_y;
    int bottom_height;
    int video_width;
    SDL_Rect spectrum_panel;
    SDL_Rect spectrum_plot;
    SDL_Rect video_panel;
    SDL_Rect video_content;
    SDL_Rect status_panel;

    explicit Layout(int w, int h)
        : width(w), height(h), spectrum_height(h * 230 / 480),
          bottom_y(4 + spectrum_height + 4), bottom_height(h - bottom_y - 4),
          video_width(w * 420 / 800),
          spectrum_panel{4, 4, w - 8, spectrum_height},
          spectrum_plot{18, 18, w - 36, spectrum_height - 36},
          video_panel{4, bottom_y, video_width, bottom_height},
          video_content{18, bottom_y + 1, video_width - 15, bottom_height - 15},
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
constexpr float kDisplayMaxDb = 10.0F;

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
                std::fprintf(stderr, "[SPECTRUM] connected to BATC\n");
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
                std::fprintf(stderr, "[SPECTRUM] connection error: %.*s\n",
                    static_cast<int>(length), data != nullptr ? static_cast<const char *>(data) : "");
                break;
            case LWS_CALLBACK_CLIENT_CLOSED:
                websocket_ = nullptr;
                set_status(SpectrumStatus::Disconnected);
                std::fprintf(stderr, "[SPECTRUM] disconnected\n");
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
        lws_context_destroy(context);
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
    SpectrumTexture(SDL_Renderer * renderer, int width, int height)
        : renderer_(renderer), width_(width), height_(height),
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
            const float db = static_cast<float>(height_ - 1 - y) /
                             std::max(1, height_ - 1) * kDisplayMaxDb;
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
            const float limited_db = std::clamp(displayed_db, 0.0F, kDisplayMaxDb);
            const int height = static_cast<int>(limited_db / kDisplayMaxDb * (height_ - 1));
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
    std::vector<uint16_t> pixels_;
    std::vector<DetectedSignal> signals_;
};

void draw_spectrum(SDL_Renderer * renderer, TextCache & text, const Layout & layout,
                   const SpectrumTexture & texture, SpectrumStatus status,
                   double selected_frequency_mhz)
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
    for(const auto & signal : texture.signals()) {
        const bool beacon = signal.frequency_mhz < 10492.0 && signal.symbol_rate_ms >= 1.0F;
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
                             kDisplayMaxDb * (layout.spectrum_plot.h - 1));
        const auto [first_width, line_height] = text.measure(first_line);
        const auto [second_width, ignored_height] = text.measure(second_line);
        (void)ignored_height;
        const int label_width = std::max(first_width, second_width);
        const int label_height = line_height * 2;
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
        text.draw(first_line, chosen.x + (label_width - first_width) / 2,
                  chosen.y, kText);
        text.draw(second_line, chosen.x + (label_width - second_width) / 2,
                  chosen.y + line_height, kText);
    }

    if(selected_frequency_mhz >= kSpectrumStartMhz &&
       selected_frequency_mhz <= kSpectrumStartMhz + kSpectrumSpanMhz) {
        const int marker_x = layout.spectrum_plot.x + static_cast<int>(
            (selected_frequency_mhz - kSpectrumStartMhz) / kSpectrumSpanMhz *
            layout.spectrum_plot.w);
        set_colour(renderer, kCyan);
        SDL_RenderDrawLine(renderer, marker_x, layout.spectrum_plot.y + 18,
                           marker_x, layout.spectrum_plot.y + layout.spectrum_plot.h - 1);
        char marker[32];
        std::snprintf(marker, sizeof(marker), "%.3f", selected_frequency_mhz);
        text.draw(marker, marker_x, layout.spectrum_plot.y - 3, kCyan, 14, true);
    }

    if(status != SpectrumStatus::Live) {
        const Colour status_colour = status == SpectrumStatus::ConnectionError ? kRed : kYellow;
        text.draw(spectrum_status_text(status), layout.spectrum_plot.x + 8,
                  layout.spectrum_plot.y + 8, status_colour);
    }
    for(int number = 1; number <= 9; ++number) {
        const int x = layout.spectrum_plot.x + number * layout.spectrum_plot.w / 9;
        text.draw(std::to_string(10490 + number), x,
                  layout.spectrum_plot.y + layout.spectrum_plot.h + 4,
                  kTextDim, 14, true);
    }
}

void draw_button(SDL_Renderer * renderer, TextCache & text, SDL_Rect rect,
                 const std::string & label, Colour colour, int font_size = 16)
{
    set_colour(renderer, kPanel);
    SDL_RenderFillRect(renderer, &rect);
    set_colour(renderer, colour);
    SDL_RenderDrawRect(renderer, &rect);
    text.draw(label, rect.x + rect.w / 2, rect.y + rect.h / 2,
              colour, font_size, true);
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
};

void draw_status(SDL_Renderer * renderer, TextCache & text, const Layout & layout,
                 int volume_percent, const qo100::ReceiverStatus & receiver,
                 bool monitor_connected, const std::string & video_codec)
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
    char agc_text[24] = "---";
    char ber_text[24] = "---";
    char ldpc_text[24] = "---";
    if(locked) {
        std::snprintf(mer_text, sizeof(mer_text), "%.1f dB", receiver.mer_x10 / 10.0);
        const double agc_percent = ((receiver.agc1 + receiver.agc2) / 2.0) / 65535.0 * 100.0;
        std::snprintf(agc_text, sizeof(agc_text), "%.0f%%", agc_percent);
        std::snprintf(ber_text, sizeof(ber_text), "%.2f%%", receiver.ber_x100 / 100.0);
        if(receiver.demod_state == 4)
            std::snprintf(ldpc_text, sizeof(ldpc_text), "%ld", receiver.ldpc_errors);
    }
    if(have_threshold) std::snprintf(margin_text, sizeof(margin_text), "%+.1f dB", mer_margin);

    const std::string service = !receiver.service_name.empty()
        ? receiver.service_name : receiver.service_provider;
    const char * frame_type = locked && receiver.demod_state == 4 && receiver.short_frames >= 0
        ? (receiver.short_frames ? "Short" : "Normal") : "---";
    const char * pilots = locked && receiver.demod_state == 4 && receiver.pilots >= 0
        ? (receiver.pilots ? "On" : "Off") : "---";

    const StatusField left[] = {
        {locked ? (receiver.demod_state == 4 ? "DVB-S2" : "DVB-S")
                : (monitor_connected ? "---" : "NO LINK"), locked ? kText : (monitor_connected ? kTextDim : kRed)},
        {mer_text, locked ? (have_threshold ? quality_colour : kText) : kTextDim},
        {quality, quality_colour},
        {margin_text, have_threshold ? quality_colour : kTextDim},
        {agc_text, locked ? kGreen : kTextDim},
        {modcod != nullptr ? modcod->fec : "---", modcod != nullptr ? kText : kTextDim},
        {modcod != nullptr ? modcod->modulation : "---", modcod != nullptr ? kText : kTextDim}
    };
    const StatusField right[] = {
        {locked && !service.empty() ? service : "---", locked && !service.empty() ? kGreen : kTextDim},
        {video_codec.empty() ? "---" : video_codec, video_codec.empty() ? kTextDim : kText},
        {"---", kTextDim},
        {ber_text, locked ? kText : kTextDim},
        {ldpc_text, locked && receiver.demod_state == 4 ? kText : kTextDim},
        {frame_type, locked && receiver.demod_state == 4 ? kText : kTextDim},
        {pilots, locked && receiver.demod_state == 4 ? kText : kTextDim}
    };
    static const char * left_labels[] = {"Mode", "MER", "Quality", "Margin", "AGC", "FEC", "MOD"};
    static const char * right_labels[] = {"Service", "Video", "Audio", "BER", "LDPC", "Frames", "Pilots"};
    const int row_height = std::clamp(layout.status_panel.h * 24 / 301, 18, 24);
    const int left_x = layout.status_panel.x + 10;
    const int right_x = layout.status_panel.x + layout.status_panel.w / 2 + 8;
    for(int i = 0; i < 7; ++i) {
        const int y = layout.status_panel.y + 8 + i * row_height;
        text.draw(left_labels[i], left_x, y, kTextDim);
        text.draw(left[i].value, left_x + 90, y, left[i].colour);
        text.draw(right_labels[i], right_x, y, kTextDim);
        text.draw(right[i].value, right_x + 90, y, right[i].colour);
    }

    const int grid_bottom = layout.status_panel.y + 8 + 7 * row_height;
    text.draw("VOL", left_x, grid_bottom, kTextDim);
    text.draw(std::to_string(volume_percent) + "%",
              layout.status_panel.x + layout.status_panel.w - 46,
              grid_bottom, kText);
    SDL_Rect track{layout.status_panel.x + 50, grid_bottom + 6,
                   layout.status_panel.w - 105, 7};
    set_colour(renderer, kBorder);
    SDL_RenderFillRect(renderer, &track);
    SDL_Rect filled = track;
    filled.w = track.w * volume_percent / 100;
    set_colour(renderer, {0x2b, 0x8e, 0xa3});
    SDL_RenderFillRect(renderer, &filled);
    set_colour(renderer, {0x65, 0xb7, 0xc7});
    const int knob_x = track.x + filled.w;
    for(int y = -7; y <= 7; ++y) {
        const int half = static_cast<int>(std::sqrt(49 - y * y));
        SDL_RenderDrawLine(renderer, knob_x - half, track.y + 3 + y,
                           knob_x + half, track.y + 3 + y);
    }

    constexpr int gap = 8;
    constexpr int margin = 8;
    const int button_width = (layout.status_panel.w - 2 * margin - 4 * gap) / 5;
    const int button_y = layout.status_panel.y + layout.status_panel.h - 65;
    const char * labels[] = {"SNAP", "CHAT", "SET", "SCAN", "EXIT"};
    const Colour colours[] = {kGreen, kCyan, kYellow, kPurple, kRed};
    for(int i = 0; i < 5; ++i) {
        SDL_Rect button{layout.status_panel.x + margin + i * (button_width + gap),
                        button_y, button_width, 54};
        draw_button(renderer, text, button, labels[i], colours[i]);
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
    if(!saved) std::fprintf(stderr, "[SCREENSHOT] %s\n", SDL_GetError());
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

} // namespace

int main(int argc, char ** argv)
{
    const Options options = parse_options(argc, argv);
    const DisplayConfig display = resolve_display_config(!options.screenshot.empty());
    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "[SDL] init failed: %s\n", SDL_GetError());
        return 1;
    }
    if(TTF_Init() != 0) {
        std::fprintf(stderr, "[TTF] init failed: %s\n", TTF_GetError());
        SDL_Quit();
        return 1;
    }

    const Uint32 window_flags = display.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP :
        (!options.screenshot.empty() ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN);
    SDL_Window * window = SDL_CreateWindow("QO-100 DATV Receiver (SDL)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        display.width, display.height, window_flags);
    if(window == nullptr) {
        std::fprintf(stderr, "[SDL] window failed: %s\n", SDL_GetError());
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    Uint32 renderer_flags = SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC;
    if(!options.screenshot.empty()) renderer_flags = SDL_RENDERER_SOFTWARE;
    SDL_Renderer * renderer = SDL_CreateRenderer(window, -1, renderer_flags);
    if(renderer == nullptr && renderer_flags != SDL_RENDERER_SOFTWARE)
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if(renderer == nullptr) {
        std::fprintf(stderr, "[SDL] renderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    SDL_RendererInfo renderer_info{};
    SDL_GetRendererInfo(renderer, &renderer_info);
    std::fprintf(stderr, "[SDL] renderer=%s accelerated=%s vsync=%s display=%dx%d\n",
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
    const std::string repository_root = repository_directory();
    const qo100::ReceiverSettings receiver_settings =
        qo100::load_receiver_settings(repository_root);
    auto spectrum_texture = std::make_unique<SpectrumTexture>(
        renderer, layout.spectrum_plot.w, layout.spectrum_plot.h);
    if(!spectrum_texture->valid()) {
        std::fprintf(stderr, "[SPECTRUM] texture failed: %s\n", SDL_GetError());
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

    SDL_Texture * video_texture = nullptr;
    int video_source_width = 0;
    int video_source_height = 0;

    VideoScheduler scheduler;
    const bool use_tuner = !options.no_tuner && !options.demo;
    qo100::VideoDecoder video_decoder([&scheduler](VideoFrame && frame) {
        scheduler.push(std::move(frame));
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
    auto last_tune = Clock::time_point{};

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
    bool fullscreen_video = false;
    int volume_percent = std::clamp(receiver_settings.audio_volume_percent, 0, 100);
    bool have_video_frame = false;
    double selected_frequency_mhz = receiver_settings.lnb_lo_mhz +
                                    beacon_frequency_khz / 1000.0;
    const auto run_started = Clock::now();
    auto last_stats = run_started;
    auto last_present_wall = Clock::time_point{};
    int64_t interval_max_gap_us = 0;
    uint64_t interval_stalls = 0;
    uint64_t previous_presented = 0;
    uint64_t previous_queue_drops = 0;
    uint64_t previous_late_drops = 0;
    while(running) {
        bool uploaded_frame_this_loop = false;
        SDL_Event event{};
        while(SDL_PollEvent(&event)) {
            if(event.type == SDL_QUIT ||
               (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)) {
                running = false;
            }
            if(event.type == SDL_MOUSEBUTTONUP) {
                const int x = event.button.x;
                const int y = event.button.y;
                if(x >= layout.spectrum_plot.x &&
                   x < layout.spectrum_plot.x + layout.spectrum_plot.w &&
                   y >= layout.spectrum_plot.y &&
                   y < layout.spectrum_plot.y + layout.spectrum_plot.h) {
                    const double clicked = kSpectrumStartMhz +
                        static_cast<double>(x - layout.spectrum_plot.x) /
                        layout.spectrum_plot.w * kSpectrumSpanMhz;
                    const DetectedSignal * selected_signal = nullptr;
                    for(const auto & signal : spectrum_texture->signals()) {
                        const double half_width = std::max(0.02,
                            static_cast<double>(signal.measured_width_mhz) / 2.0);
                        if(std::abs(clicked - signal.frequency_mhz) <= half_width) {
                            selected_frequency_mhz = signal.frequency_mhz;
                            selected_signal = &signal;
                            break;
                        }
                    }
                    if(selected_signal == nullptr) {
                        selected_frequency_mhz = clicked;
                        std::fprintf(stderr, "[TUNE] no detected signal at %.3fMHz; not tuning\n",
                                     clicked);
                    }
                    else if(!receiver_enabled || !receiver_client.control_connected()) {
                        std::fprintf(stderr, "[TUNE] selected %.3fMHz but control link is not ready\n",
                                     selected_frequency_mhz);
                    }
                    else if(Clock::now() - last_tune < std::chrono::milliseconds(1500)) {
                        std::fprintf(stderr, "[TUNE] selection ignored inside debounce window\n");
                    }
                    else {
                        last_tune = Clock::now();
                        const long if_khz = std::lround(
                            (selected_frequency_mhz - receiver_settings.lnb_lo_mhz) * 1000.0);
                        const long symbol_rate_ksps = std::lround(
                            selected_signal->symbol_rate_ms * 1000.0F);
                        receiver_status.reset();
                        scheduler.reset();
                        have_video_frame = false;
                        receiver_client.send_tune(if_khz, symbol_rate_ksps);
                        std::fprintf(stderr,
                            "[TUNE] %.3fMHz -> IF=%ldkHz SR=%ldkS/s\n",
                            selected_frequency_mhz, if_khz, symbol_rate_ksps);
                    }
                }
                if(x >= layout.video_panel.x && x < layout.video_panel.x + layout.video_panel.w &&
                   y >= layout.video_panel.y && y < layout.video_panel.y + layout.video_panel.h)
                    fullscreen_video = !fullscreen_video;
                const int slider_y = layout.status_panel.y + 8 + 7 *
                    std::clamp(layout.status_panel.h * 24 / 301, 18, 24) + 6;
                const int slider_x = layout.status_panel.x + 50;
                const int slider_w = layout.status_panel.w - 105;
                if(y >= slider_y - 10 && y <= slider_y + 17 &&
                   x >= slider_x && x <= slider_x + slider_w)
                    volume_percent = std::clamp((x - slider_x) * 100 / slider_w, 0, 100);
            }
        }

        if(!options.offline_spectrum &&
           spectrum_feed.consume(spectrum_bins, spectrum_status)) {
            spectrum_texture->update(spectrum_bins);
            spectrum_ready = true;
        }

        if(receiver_enabled && receiver_client.consume_status(receiver_status)) {
            const bool locked_now = receiver_status.locked();
            if(locked_now && !receiver_was_locked) {
                scheduler.reset();
                have_video_frame = false;
                video_decoder.request_reset();
                const std::string service = !receiver_status.service_name.empty()
                    ? receiver_status.service_name : receiver_status.service_provider;
                std::fprintf(stderr,
                    "[TUNE] lock: %s IF=%ldkHz SR=%ldkS/s MER=%.1fdB service=%s\n",
                    receiver_status.demod_state == 4 ? "DVB-S2" : "DVB-S",
                    receiver_status.carrier_khz, receiver_status.symbol_rate_ksps,
                    receiver_status.mer_x10 / 10.0,
                    service.empty() ? "---" : service.c_str());
            }
            else if(!locked_now && receiver_was_locked) {
                std::fprintf(stderr, "[TUNE] lock lost\n");
            }
            receiver_was_locked = locked_now;
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
                    std::fprintf(stderr, "[VIDEO] texture=%dx%d IYUV\n",
                                 video_source_width, video_source_height);
                }
                else {
                    std::fprintf(stderr, "[VIDEO] texture failed: %s\n", SDL_GetError());
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
            }
        }

        set_colour(renderer, kBackground);
        SDL_RenderClear(renderer);
        if(fullscreen_video) {
            if(have_video_frame) {
                const SDL_Rect screen_bounds{0, 0, display.width, display.height};
                const SDL_Rect destination = aspect_fit(
                    video_source_width, video_source_height, screen_bounds);
                SDL_RenderCopy(renderer, video_texture, nullptr, &destination);
            }
        }
        else {
            draw_spectrum(renderer, text, layout, *spectrum_texture,
                          spectrum_status, selected_frequency_mhz);
            fill_panel(renderer, layout.video_panel);
            set_colour(renderer, {0, 0, 0});
            SDL_RenderFillRect(renderer, &layout.video_content);
            if(have_video_frame) {
                const SDL_Rect destination = aspect_fit(
                    video_source_width, video_source_height, layout.video_content);
                SDL_RenderCopy(renderer, video_texture, nullptr, &destination);
            }
            const std::string video_codec = options.demo ? "DEMO" : video_decoder.codec_name();
            draw_status(renderer, text, layout, volume_percent, receiver_status,
                        receiver_client.monitor_connected(), video_codec);
        }
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
            std::fprintf(stderr,
                "[PRESENT] fps=%.1f drop=%llu late=%llu max_gap=%.1fms stalls=%llu "
                "shown=%llu rebases=%llu depth=%zu "
                "spectrum=%llu replaced=%llu receiver=%llu/%llu link=%s/%s "
                "decode=%llu reopen=%llu errors=%llu\n",
                presented_delta / window_seconds,
                static_cast<unsigned long long>(queue_drop_delta),
                static_cast<unsigned long long>(late_drop_delta),
                interval_max_gap_us / 1000.0,
                static_cast<unsigned long long>(interval_stalls),
                static_cast<unsigned long long>(stats.presented),
                static_cast<unsigned long long>(stats.rebases), stats.depth,
                static_cast<unsigned long long>(spectrum_feed.received_frames()),
                static_cast<unsigned long long>(spectrum_feed.replaced_frames()),
                static_cast<unsigned long long>(receiver_client.received_updates()),
                static_cast<unsigned long long>(receiver_client.replaced_updates()),
                receiver_client.monitor_connected() ? "monitor" : "---",
                receiver_client.control_connected() ? "control" : "---",
                static_cast<unsigned long long>(video_decoder.decoded_frames()),
                static_cast<unsigned long long>(video_decoder.reopen_count()),
                static_cast<unsigned long long>(video_decoder.decode_errors()));
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

    spectrum_feed.stop();
    video_decoder.stop();
    receiver_client.stop();
    longmynd->stop();

    if(!options.screenshot.empty()) {
        set_colour(renderer, kBackground);
        SDL_RenderClear(renderer);
        draw_spectrum(renderer, text, layout, *spectrum_texture,
                      spectrum_status, selected_frequency_mhz);
        fill_panel(renderer, layout.video_panel);
        set_colour(renderer, {0, 0, 0});
        SDL_RenderFillRect(renderer, &layout.video_content);
        if(have_video_frame) {
            const SDL_Rect destination = aspect_fit(
                video_source_width, video_source_height, layout.video_content);
            SDL_RenderCopy(renderer, video_texture, nullptr, &destination);
        }
        const std::string video_codec = options.demo ? "DEMO" : video_decoder.codec_name();
        draw_status(renderer, text, layout, volume_percent, receiver_status,
                    receiver_client.monitor_connected(), video_codec);
        SDL_RenderPresent(renderer);
        if(!save_screenshot(renderer, display.width, display.height, options.screenshot))
            std::fprintf(stderr, "[SCREENSHOT] failed to save %s\n", options.screenshot.c_str());
        else
            std::fprintf(stderr, "[SCREENSHOT] saved %s\n", options.screenshot.c_str());
    }

    producer_running = false;
    if(producer.joinable()) producer.join();
    const auto stats = scheduler.stats();
    std::fprintf(stderr,
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
    SDL_Quit();
    return 0;
}
