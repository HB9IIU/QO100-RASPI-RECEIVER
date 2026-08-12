#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

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

struct VideoFrame {
    std::vector<uint16_t> pixels;
    int width = 0;
    int height = 0;
    int64_t pts_us = 0;
};

class VideoScheduler {
public:
    void push(VideoFrame frame)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(queue_.empty()) first_queued_at_ = Clock::now();
        if(!queue_.empty() && frame.pts_us <= queue_.back().pts_us) {
            frame.pts_us = queue_.back().pts_us + 1;
        }
        while(queue_.size() >= kVideoQueueCapacity) {
            queue_.pop_front();
            ++queue_drops_;
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
        return selected;
    }

    void reset()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        clock_started_ = false;
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

Colour spectrum_colour(float value)
{
    value = std::clamp(value, 0.0F, 1.0F);
    if(value < 0.2F) return {0, static_cast<uint8_t>(40 + value * 500), 170};
    if(value < 0.4F) return {0, 185, static_cast<uint8_t>(220 - (value - 0.2F) * 700)};
    if(value < 0.6F) return {static_cast<uint8_t>((value - 0.4F) * 900), 210, 40};
    if(value < 0.8F) return {255, static_cast<uint8_t>(210 - (value - 0.6F) * 500), 20};
    return {255, static_cast<uint8_t>(110 - (value - 0.8F) * 300), 30};
}

float synthetic_spectrum(double frequency)
{
    const auto plateau = [](double x, double left, double right, double edge) {
        const double rise = 1.0 / (1.0 + std::exp(-(x - left) / edge));
        const double fall = 1.0 / (1.0 + std::exp((x - right) / edge));
        return rise * fall;
    };
    const double beacon = 0.93 * plateau(frequency, 10490.72, 10492.25, 0.04);
    const double signal = 0.75 * plateau(frequency, 10497.10, 10497.45, 0.018);
    const double floor = 0.035 + 0.015 * std::sin(frequency * 44.0) +
                         0.01 * std::sin(frequency * 117.0);
    return static_cast<float>(std::clamp(beacon + signal + floor, 0.0, 1.0));
}

void draw_spectrum(SDL_Renderer * renderer, TextCache & text, const Layout & layout)
{
    fill_panel(renderer, layout.spectrum_panel);
    set_colour(renderer, {0, 0, 0});
    SDL_RenderFillRect(renderer, &layout.spectrum_plot);

    set_colour(renderer, {30, 55, 70});
    for(int i = 1; i < 9; ++i) {
        const int x = layout.spectrum_plot.x + i * layout.spectrum_plot.w / 9;
        SDL_RenderDrawLine(renderer, x, layout.spectrum_plot.y, x,
                           layout.spectrum_plot.y + layout.spectrum_plot.h);
    }
    for(int i = 1; i < 2; ++i) {
        const int y = layout.spectrum_plot.y + i * layout.spectrum_plot.h / 2;
        SDL_RenderDrawLine(renderer, layout.spectrum_plot.x, y,
                           layout.spectrum_plot.x + layout.spectrum_plot.w, y);
    }

    const int baseline = layout.spectrum_plot.y + layout.spectrum_plot.h - 1;
    for(int px = 0; px < layout.spectrum_plot.w; ++px) {
        const double fraction = static_cast<double>(px) / std::max(1, layout.spectrum_plot.w - 1);
        const double frequency = 10490.5 + 9.0 * fraction;
        const float level = synthetic_spectrum(frequency);
        const int top = baseline - static_cast<int>(level * (layout.spectrum_plot.h - 10));
        for(int y = top; y <= baseline; ++y) {
            const float vertical = static_cast<float>(baseline - y) /
                                   std::max(1, baseline - top);
            set_colour(renderer, spectrum_colour(vertical));
            SDL_RenderDrawPoint(renderer, layout.spectrum_plot.x + px, y);
        }
    }

    set_colour(renderer, kCyan);
    const int marker_x = layout.spectrum_plot.x + static_cast<int>(
        (10491.525 - 10490.5) / 9.0 * layout.spectrum_plot.w);
    SDL_RenderDrawLine(renderer, marker_x, layout.spectrum_plot.y + 18,
                       marker_x, baseline);
    text.draw("10491.525", marker_x, layout.spectrum_plot.y - 3, kCyan, 14, true);
    text.draw("333KS 10497.262", layout.spectrum_plot.x +
              static_cast<int>((10497.262 - 10490.5) / 9.0 * layout.spectrum_plot.w),
              layout.spectrum_plot.y + 39, kText, 14, true);

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

void draw_status(SDL_Renderer * renderer, TextCache & text, const Layout & layout,
                 int volume_percent)
{
    fill_panel(renderer, layout.status_panel);
    static const char * left[][2] = {
        {"Mode", "---"}, {"MER", "---"}, {"Quality", "---"},
        {"Margin", "---"}, {"AGC", "---"}, {"FEC", "---"}, {"MOD", "---"}
    };
    static const char * right[][2] = {
        {"Service", "---"}, {"Video", "---"}, {"Audio", "---"},
        {"BER", "---"}, {"LDPC", "---"}, {"Frames", "---"}, {"Pilots", "---"}
    };
    const int row_height = std::clamp(layout.status_panel.h * 24 / 301, 18, 24);
    const int left_x = layout.status_panel.x + 10;
    const int right_x = layout.status_panel.x + layout.status_panel.w / 2 + 8;
    for(int i = 0; i < 7; ++i) {
        const int y = layout.status_panel.y + 8 + i * row_height;
        text.draw(left[i][0], left_x, y, kTextDim);
        text.draw(left[i][1], left_x + 90, y, kTextDim);
        text.draw(right[i][0], right_x, y, kTextDim);
        text.draw(right[i][1], right_x + 90, y, kTextDim);
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
    frame.pts_us = pts_us;
    frame.pixels.resize(static_cast<size_t>(width) * height);
    const int bar = static_cast<int>((index * 4) % std::max(1, width));
    for(int y = 0; y < height; ++y) {
        for(int x = 0; x < width; ++x) {
            const uint8_t red = static_cast<uint8_t>(25 + 80 * x / std::max(1, width));
            const uint8_t green = static_cast<uint8_t>(25 + 60 * y / std::max(1, height));
            const bool highlight = std::abs(x - bar) < 10;
            frame.pixels[static_cast<size_t>(y) * width + x] =
                highlight ? rgb565(0x39, 0xd6, 0xff) : rgb565(red, green, 90);
        }
    }
    return frame;
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
    int seconds = 0;
    std::string screenshot;
};

Options parse_options(int argc, char ** argv)
{
    Options options;
    for(int i = 1; i < argc; ++i) {
        if(std::strcmp(argv[i], "--demo") == 0) options.demo = true;
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
    SDL_Texture * video_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565,
        SDL_TEXTUREACCESS_STREAMING, layout.video_content.w, layout.video_content.h);
    if(video_texture == nullptr) {
        std::fprintf(stderr, "[VIDEO] texture failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetTextureScaleMode(video_texture, SDL_ScaleModeLinear);

    VideoScheduler scheduler;
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
    int volume_percent = 100;
    bool have_video_frame = false;
    const auto run_started = Clock::now();
    auto last_stats = run_started;
    while(running) {
        SDL_Event event{};
        while(SDL_PollEvent(&event)) {
            if(event.type == SDL_QUIT ||
               (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)) {
                running = false;
            }
            if(event.type == SDL_MOUSEBUTTONUP) {
                const int x = event.button.x;
                const int y = event.button.y;
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

        if(auto frame = scheduler.take_due(Clock::now())) {
            if(frame->width == layout.video_content.w && frame->height == layout.video_content.h) {
                SDL_UpdateTexture(video_texture, nullptr, frame->pixels.data(),
                                  frame->width * static_cast<int>(sizeof(uint16_t)));
                have_video_frame = true;
            }
        }

        set_colour(renderer, kBackground);
        SDL_RenderClear(renderer);
        if(fullscreen_video) {
            if(have_video_frame) SDL_RenderCopy(renderer, video_texture, nullptr, nullptr);
        }
        else {
            draw_spectrum(renderer, text, layout);
            fill_panel(renderer, layout.video_panel);
            set_colour(renderer, {0, 0, 0});
            SDL_RenderFillRect(renderer, &layout.video_content);
            if(have_video_frame)
                SDL_RenderCopy(renderer, video_texture, nullptr, &layout.video_content);
            draw_status(renderer, text, layout, volume_percent);
        }
        SDL_RenderPresent(renderer);

        const auto now = Clock::now();
        if(now - last_stats >= std::chrono::seconds(5)) {
            const auto stats = scheduler.stats();
            std::fprintf(stderr,
                "[PRESENT] shown=%llu queue_drops=%llu late_drops=%llu rebases=%llu depth=%zu\n",
                static_cast<unsigned long long>(stats.presented),
                static_cast<unsigned long long>(stats.queue_drops),
                static_cast<unsigned long long>(stats.late_drops),
                static_cast<unsigned long long>(stats.rebases), stats.depth);
            last_stats = now;
        }

        if(options.seconds > 0 && now - run_started >= std::chrono::seconds(options.seconds))
            running = false;
        if(!options.screenshot.empty() && have_video_frame) running = false;
        if((renderer_info.flags & SDL_RENDERER_PRESENTVSYNC) == 0) SDL_Delay(2);
    }

    if(!options.screenshot.empty()) {
        set_colour(renderer, kBackground);
        SDL_RenderClear(renderer);
        draw_spectrum(renderer, text, layout);
        fill_panel(renderer, layout.video_panel);
        set_colour(renderer, {0, 0, 0});
        SDL_RenderFillRect(renderer, &layout.video_content);
        if(have_video_frame) SDL_RenderCopy(renderer, video_texture, nullptr, &layout.video_content);
        draw_status(renderer, text, layout, volume_percent);
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
    text.clear();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    SDL_Quit();
    return 0;
}
