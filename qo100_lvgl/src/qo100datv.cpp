/*
 * qo100datv.cpp
 * ---------------------------------------------------------------------------
 * The real target application (see /home/daniel/DATVreceiver/tempo/mydream.png
 * for the visual spec). Layout follows that mockup; panels are filled in with
 * real data one at a time.
 *
 * Status: spectrum panel shows the live BATC WB feed (visual only - no
 * click-to-tune yet, no signal-detection labels). Video/status panels are
 * still placeholders. The spectrum rendering here is adapted from the
 * verified standalone src/spectrum_test/spectrum_live.cpp, just drawn inside
 * a panel instead of full-screen.
 */

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <libwebsockets.h>
#ifndef LWS_PROTOCOL_LIST_TERM
#define LWS_PROTOCOL_LIST_TERM { nullptr, nullptr, 0, 0, 0, nullptr, 0 }
#endif
#include <netinet/in.h>
#include <limits.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <SDL2/SDL.h>
#include <lvgl/lvgl.h>

#include "lib/driver_backends.h"
#include "lib/simulator_settings.h"

extern "C" simulator_settings_t settings;

namespace {

/* Diagnostic: reference point for [STARTUP] timing logs in main() and the
 * first-timer-call marker in service_websocket(). */
const auto g_boot_t0 = std::chrono::steady_clock::now();

constexpr int SCREEN_W = 1024;
constexpr int SCREEN_H = 600;

/* Panel grid, derived from SCREEN_W/H so it keeps the original design's
 * proportions (hand-tuned for the first 800x480 display) on any other
 * panel size - only SCREEN_W/H need to change if the display changes again. */
constexpr int PANEL_MARGIN = 4;
/* Shared inset for content drawn inside a panel (spectrum plot, video canvas)
 * - keeps their margins equal by construction rather than by coincidence. */
constexpr int CONTENT_MARGIN = 14;
constexpr int SPECTRUM_PANEL_H = SCREEN_H * 230 / 480;
constexpr int BOTTOM_ROW_Y = PANEL_MARGIN + SPECTRUM_PANEL_H + PANEL_MARGIN;
constexpr int BOTTOM_ROW_H = SCREEN_H - BOTTOM_ROW_Y - PANEL_MARGIN;
constexpr int BOTTOM_ROW_INNER_W = SCREEN_W - 4 * PANEL_MARGIN;
constexpr int VIDEO_PANEL_W = SCREEN_W * 420 / 800;
constexpr int STATUS_PANEL_W = SCREEN_W * 178 / 800;
constexpr int STATUS2_PANEL_W = BOTTOM_ROW_INNER_W - VIDEO_PANEL_W - STATUS_PANEL_W;
constexpr int STATUS_PANEL_X = PANEL_MARGIN + VIDEO_PANEL_W + PANEL_MARGIN;
constexpr int STATUS2_PANEL_X = STATUS_PANEL_X + STATUS_PANEL_W + PANEL_MARGIN;

constexpr lv_color_t COLOR_BG        = LV_COLOR_MAKE(0x10, 0x14, 0x1c);
constexpr lv_color_t COLOR_PANEL     = LV_COLOR_MAKE(0x18, 0x1e, 0x2a);
constexpr lv_color_t COLOR_BORDER    = LV_COLOR_MAKE(0x2a, 0x33, 0x44);
constexpr lv_color_t COLOR_TEXT      = LV_COLOR_MAKE(0xd8, 0xde, 0xe9);
constexpr lv_color_t COLOR_TEXT_DIM  = LV_COLOR_MAKE(0x70, 0x78, 0x88);
constexpr lv_color_t COLOR_CYAN      = LV_COLOR_MAKE(0x39, 0xd6, 0xff);
constexpr lv_color_t COLOR_GREEN     = LV_COLOR_MAKE(0x35, 0xd4, 0x6a);
constexpr lv_color_t COLOR_YELLOW    = LV_COLOR_MAKE(0xff, 0xd1, 0x66);
constexpr lv_color_t COLOR_RED       = LV_COLOR_MAKE(0xff, 0x4d, 0x4d);

lv_obj_t * make_panel(lv_obj_t * parent, int x, int y, int w, int h)
{
    lv_obj_t * panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, COLOR_BORDER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 4, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    return panel;
}

lv_obj_t * make_label(lv_obj_t * parent, const char * text, lv_color_t color,
                      const lv_font_t * font = &lv_font_montserrat_14)
{
    lv_obj_t * label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_font(label, font, 0);
    return label;
}

/* ===================================================================
 * BATC live WB spectrum (visual only - see src/spectrum_test for the
 * fuller standalone version this was adapted from, including signal
 * detection and click-to-tune math not yet ported here).
 * =================================================================== */

lv_obj_t * spectrum_canvas = nullptr;
lv_obj_t * spectrum_status_label = nullptr;
uint16_t * spectrum_pixels = nullptr;
std::vector<uint16_t> last_bins;

int plot_x = 0;
int plot_y = 0;
int plot_w = 0;
int plot_h = 0;
int spectrum_panel_w = 0;

lv_obj_t * tune_marker = nullptr;
lv_obj_t * tune_marker_label = nullptr;
lv_obj_t * video_panel = nullptr;
/* Covers the video canvas from the moment a retune is requested (see
 * retune_exact()) until either a fresh frame from the new stream actually
 * reaches the screen (video_frame_update_cb) or the lock watchdog gives up
 * (finish_rx_status_update()) - otherwise the old stream's last decoded
 * frame just sits there frozen for the whole search/relock/redecode
 * stretch, looking like tapping a new signal did nothing. */
lv_obj_t * tuning_overlay = nullptr;
/* Set from retune_exact(), read in video_frame_update_cb() - both always run
 * on the UI thread (LVGL event callback / lv_timer callback), so this needs
 * no synchronization despite decode_session_count itself being atomic. */
int tuning_overlay_target_session = 0;
/* Bumped once per decode_loop() outer-loop iteration, i.e. every time it
 * tears down and reopens a fresh AVFormatContext. tuning_overlay's hide
 * condition waits for this rather than "any successful frame" - data
 * already past the tuner and sitting in the ring buffer at tap time can
 * still belong to the *old* stream, so the very next rendered frame after a
 * retune isn't necessarily from the new one; the next fresh decode session
 * is. See retune_exact() and video_frame_update_cb(). */
std::atomic<int> decode_session_count{0};
/* Everything except video_panel (spectrum + both status panels) - hidden as
 * one unit while the video is fullscreen, see video_panel_click_cb. */
lv_obj_t * normal_view = nullptr;
lv_obj_t * chat_page = nullptr;
lv_obj_t * settings_page = nullptr;
void request_video_reset();
void return_to_beacon();

constexpr float DISPLAY_MAX_DB = 10.0F;
constexpr float SERVER_UNITS_PER_DB = 3276.8F;
constexpr float DISPLAY_ZERO_OFFSET_DB = 20.0F / 6.0F;
constexpr double START_FREQUENCY_MHZ = 10490.5;
constexpr double SPAN_MHZ = 9.0;

constexpr const char * SERVER_HOST = "eshail.batc.org.uk";
constexpr int SERVER_PORT = 443;
constexpr const char * SERVER_PATH = "/wb/fft";

lws_context * websocket_context = nullptr;
lws * batc_wsi = nullptr;
std::vector<uint8_t> message_buffer;
bool message_is_binary = false;
std::chrono::steady_clock::time_point last_batc_connect_attempt{};
constexpr auto BATC_RECONNECT_INTERVAL = std::chrono::milliseconds(3000);

/* libwebsockets may block inside lws_service() for the duration of a TLS
 * connection timeout even when passed a zero timeout. Keep all BATC network
 * work off the LVGL thread and hand only completed messages/status across. */
std::thread batc_ws_thread;
std::atomic<bool> batc_ws_thread_running{false};
std::mutex batc_handoff_mutex;
std::vector<uint8_t> batc_pending_message;
enum class BatcUiStatus { None, Waiting, ConnectionError, Disconnected };
BatcUiStatus batc_pending_status = BatcUiStatus::None;

uint16_t rgb565(uint8_t red, uint8_t green, uint8_t blue)
{
    return lv_color_to_u16(lv_color_make(red, green, blue));
}

uint8_t mix_channel(uint8_t start, uint8_t end, float amount)
{
    return static_cast<uint8_t>(start + (static_cast<float>(end) - start) * amount);
}

uint16_t mix_colour(uint8_t r1, uint8_t g1, uint8_t b1,
                    uint8_t r2, uint8_t g2, uint8_t b2, float amount)
{
    return rgb565(mix_channel(r1, r2, amount), mix_channel(g1, g2, amount),
                  mix_channel(b1, b2, amount));
}

uint16_t spectrum_gradient(float db)
{
    db = std::clamp(db, 0.0F, DISPLAY_MAX_DB);

    if(db < 2.0F) return mix_colour(0, 18, 105, 0, 145, 220, db / 2.0F);
    if(db < 4.0F) return mix_colour(0, 145, 220, 0, 190, 55, (db - 2.0F) / 2.0F);
    if(db < 5.5F) return mix_colour(0, 190, 55, 245, 225, 0, (db - 4.0F) / 1.5F);
    if(db < 7.0F) return mix_colour(245, 225, 0, 255, 82, 0, (db - 5.5F) / 1.5F);
    if(db < 8.5F) return mix_colour(255, 82, 0, 245, 0, 35, (db - 7.0F) / 1.5F);

    return mix_colour(245, 0, 35, 255, 35, 155, (db - 8.5F) / 1.5F);
}

void draw_fft(const std::vector<uint16_t> & bins)
{
    const uint16_t black = rgb565(0, 0, 0);
    const uint16_t grid = rgb565(45, 56, 66);
    const uint16_t trace = rgb565(255, 225, 235);
    std::vector<uint16_t> vertical_palette(plot_h);

    for(int y = 0; y < plot_h; ++y) {
        const float db = (static_cast<float>(plot_h - 1 - y) / (plot_h - 1)) * DISPLAY_MAX_DB;
        vertical_palette[y] = spectrum_gradient(db);
    }

    std::fill(spectrum_pixels, spectrum_pixels + plot_w * plot_h, black);

    for(int division = 0; division <= 2; ++division) {
        const int y = (division * (plot_h - 1)) / 2;
        for(int x = 0; x < plot_w; ++x)
            if((x % 8) < 4) spectrum_pixels[y * plot_w + x] = grid;
    }

    for(int division = 1; division <= 9; ++division) {
        const int x = static_cast<int>(((division - 0.5) / 9.0) * (plot_w - 1));
        for(int y = 0; y < plot_h; ++y)
            if((y % 8) < 4) spectrum_pixels[y * plot_w + x] = grid;
    }

    for(int x = 0; x < plot_w; ++x) {
        const float sample_position = (static_cast<float>(x) * bins.size()) / plot_w;
        const size_t first = std::min(static_cast<size_t>(sample_position), bins.size() - 1);
        const size_t second = std::min(first + 1, bins.size() - 1);
        const float fraction = sample_position - static_cast<float>(first);
        const float server_value = bins[first] + fraction * (bins[second] - bins[first]);

        const float displayed_db = server_value / SERVER_UNITS_PER_DB - DISPLAY_ZERO_OFFSET_DB;
        const float limited_db = std::clamp(displayed_db, 0.0F, DISPLAY_MAX_DB);

        const int height = static_cast<int>((limited_db / DISPLAY_MAX_DB) * (plot_h - 1));
        const int top = plot_h - 1 - height;

        for(int y = top + 1; y < plot_h; ++y)
            spectrum_pixels[y * plot_w + x] = vertical_palette[y];
        spectrum_pixels[top * plot_w + x] = trace;
    }

    lv_obj_invalidate(spectrum_canvas);
}

/* Signal detection - ported from spectrum_test/spectrum_live.cpp, which
 * itself matches the official BATC viewer's JavaScript detection logic. */

struct DetectedSignal {
    size_t start_bin;
    size_t end_bin;
    float middle_bin;
    float strength;
    float measured_width_mhz;
    float symbol_rate_ms;
    double frequency_mhz;
};

std::vector<DetectedSignal> last_detected_signals;
constexpr size_t MAX_SIGNAL_LABELS = 16;
lv_obj_t * signal_labels[MAX_SIGNAL_LABELS]{};
lv_obj_t * callsign_labels[MAX_SIGNAL_LABELS]{};

struct DecodedService {
    double frequency_mhz;
    std::string callsign;
};
std::vector<DecodedService> decoded_services;

/* Match a measured signal width to one of the standard rates used by the
 * official BATC viewer. Returns MS/s (0.500 means 500 kS/s). */
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

/*
 * Detect the raised rectangular DVB signals.
 * 1. Scan using a three-bin average and threshold 16000.
 * 2. Average the middle 40% to estimate the flat signal top.
 * 3. Move both edges inward to 75% between noise (11000) and the top.
 * 4. Use the midpoint for frequency and the edge distance for symbol rate.
 */
std::vector<DetectedSignal> detect_signals(const std::vector<uint16_t> & bins)
{
    constexpr float NOISE_LEVEL = 11000.0F;
    constexpr float SIGNAL_THRESHOLD = 16000.0F;

    std::vector<DetectedSignal> detected;
    bool in_signal = false;
    size_t initial_start = 0;

    for(size_t index = 2; index < bins.size(); ++index) {
        const float three_bin_average =
            (bins[index] + bins[index - 1] + bins[index - 2]) / 3.0F;

        if(!in_signal && three_bin_average > SIGNAL_THRESHOLD) {
            in_signal = true;
            initial_start = index;
            continue;
        }

        if(!in_signal || three_bin_average >= SIGNAL_THRESHOLD) continue;
        in_signal = false;
        const size_t initial_end = index;

        if(initial_end <= initial_start + 2) continue;

        const size_t middle_start = initial_start +
            static_cast<size_t>(0.3F * (initial_end - initial_start));
        const size_t middle_end = initial_start +
            static_cast<size_t>(0.7F * (initial_end - initial_start));
        if(middle_end <= middle_start) continue;

        uint64_t sum = 0;
        for(size_t bin = middle_start; bin < middle_end; ++bin)
            sum += bins[bin];
        const float strength = static_cast<float>(sum) / (middle_end - middle_start);
        const float edge_level = NOISE_LEVEL + 0.75F * (strength - NOISE_LEVEL);

        size_t refined_start = initial_start;
        while(refined_start < initial_end && bins[refined_start] < edge_level)
            ++refined_start;

        size_t refined_end = std::min(initial_end, bins.size() - 1);
        while(refined_end > refined_start && bins[refined_end] < edge_level)
            --refined_end;

        if(refined_end <= refined_start) continue;

        const float middle_bin = refined_start + (refined_end - refined_start) / 2.0F;
        const float measured_width_mhz =
            (refined_end - refined_start) * static_cast<float>(SPAN_MHZ / bins.size());
        const float symbol_rate_ms = align_symbol_rate(measured_width_mhz);
        if(symbol_rate_ms == 0.0F) continue;

        const double frequency_mhz =
            START_FREQUENCY_MHZ + ((middle_bin + 1.0) / bins.size()) * SPAN_MHZ;

        detected.push_back({
            refined_start, refined_end, middle_bin, strength,
            measured_width_mhz, symbol_rate_ms, frequency_mhz
        });
    }

    return detected;
}

void update_signal_labels(const std::vector<DetectedSignal> & signals)
{
    struct LabelArea { int x, y, width, height; };
    auto overlaps = [](const LabelArea & a, const LabelArea & b) {
        constexpr int gap = 4;
        return a.x < b.x + b.width + gap && a.x + a.width + gap > b.x &&
               a.y < b.y + b.height + gap && a.y + a.height + gap > b.y;
    };

    for(lv_obj_t * label : signal_labels)
        if(label != nullptr) lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
    for(lv_obj_t * label : callsign_labels)
        if(label != nullptr) lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);

    float beacon_strength = 0.0F;
    for(const DetectedSignal & signal : signals) {
        if(signal.frequency_mhz < 10492.0 && signal.symbol_rate_ms >= 1.0F) {
            beacon_strength = signal.strength;
            break;
        }
    }

    std::vector<LabelArea> occupied;
    const size_t count = std::min(signals.size(), MAX_SIGNAL_LABELS);
    for(size_t index = 0; index < count; ++index) {
        const DetectedSignal & signal = signals[index];
        lv_obj_t * label = signal_labels[index];
        if(label == nullptr) continue;

        const bool beacon = signal.frequency_mhz < 10492.0 && signal.symbol_rate_ms >= 1.0F;
        /* The beacon remains the 0 dB reference, but its large occupied
         * bandwidth makes an on-plot label visually distracting. */
        if(beacon) continue;

        char text[64];
        if(beacon_strength > 0.0F) {
            const float relative_db = (signal.strength - beacon_strength) / SERVER_UNITS_PER_DB;
            if(signal.symbol_rate_ms < 0.7F)
                std::snprintf(text, sizeof(text), "%.0fKS %.3f\n%+.2f dB BCN",
                              signal.symbol_rate_ms * 1000.0F, signal.frequency_mhz, relative_db);
            else
                std::snprintf(text, sizeof(text), "%.1fMS %.3f\n%+.2f dB BCN",
                              signal.symbol_rate_ms, signal.frequency_mhz, relative_db);
        }
        else {
            std::snprintf(text, sizeof(text), "%.0fKS %.3f\n-- dB BCN",
                          signal.symbol_rate_ms * 1000.0F, signal.frequency_mhz);
        }
        lv_label_set_text(label, text);
        lv_obj_update_layout(label);

        const int label_w = lv_obj_get_width(label);
        const int label_h = lv_obj_get_height(label);
        const float displayed_db = signal.strength / SERVER_UNITS_PER_DB - DISPLAY_ZERO_OFFSET_DB;
        const float limited_db = std::clamp(displayed_db, 0.0F, DISPLAY_MAX_DB);
        const int trace_y = plot_y + plot_h - 1 -
            static_cast<int>((limited_db / DISPLAY_MAX_DB) * (plot_h - 1));
        const double fraction = (signal.frequency_mhz - START_FREQUENCY_MHZ) / SPAN_MHZ;
        const int centre_x = plot_x + static_cast<int>(fraction * plot_w);
        const int natural_x = std::clamp(centre_x - label_w / 2, 0, spectrum_panel_w - label_w);
        const int natural_y = std::clamp(trace_y - label_h - 4,
                                         plot_y + 2, plot_y + plot_h - label_h - 2);

        LabelArea chosen{natural_x, natural_y, label_w, label_h};
        bool found = false;
        const int row = label_h + 5;
        const int y_offsets[] = {0, -row, row, -2 * row, 2 * row};
        const int x_offsets[] = {0, -label_w / 2, label_w / 2};
        for(int x_offset : x_offsets) {
            for(int y_offset : y_offsets) {
                LabelArea candidate{
                    std::clamp(natural_x + x_offset, 0, spectrum_panel_w - label_w),
                    std::clamp(natural_y + y_offset, plot_y + 2,
                               plot_y + plot_h - label_h - 2),
                    label_w, label_h
                };
                bool collision = false;
                for(const LabelArea & used : occupied) {
                    if(overlaps(candidate, used)) { collision = true; break; }
                }
                if(!collision) { chosen = candidate; found = true; break; }
            }
            if(found) break;
        }

        occupied.push_back(chosen);
        lv_obj_set_pos(label, chosen.x, chosen.y);
        lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);

        const DecodedService * decoded = nullptr;
        const double half_width = std::max(0.05, signal.measured_width_mhz / 2.0);
        for(const DecodedService & candidate : decoded_services) {
            if(std::abs(candidate.frequency_mhz - signal.frequency_mhz) <= half_width) {
                decoded = &candidate;
                break;
            }
        }
        if(decoded != nullptr && callsign_labels[index] != nullptr) {
            lv_obj_t * callsign = callsign_labels[index];
            lv_label_set_text(callsign, decoded->callsign.c_str());
            lv_obj_update_layout(callsign);
            const int callsign_h = lv_obj_get_height(callsign);
            lv_obj_set_pos(callsign, chosen.x,
                           std::max(plot_y + 1, chosen.y - callsign_h - 2));
            lv_obj_remove_flag(callsign, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void decode_and_draw(const std::vector<uint8_t> & bytes)
{
    if(bytes.empty() || (bytes.size() % 2U) != 0U) return;

    const size_t bin_count = bytes.size() / 2U;
    std::vector<uint16_t> bins(bin_count);
    for(size_t index = 0; index < bin_count; ++index) {
        bins[index] = static_cast<uint16_t>(bytes[index * 2U]) |
            static_cast<uint16_t>(bytes[index * 2U + 1U] << 8U);
    }

    draw_fft(bins);
    last_bins = bins;
    last_detected_signals = detect_signals(bins);
    update_signal_labels(last_detected_signals);

    if(spectrum_status_label != nullptr) {
        lv_obj_add_flag(spectrum_status_label, LV_OBJ_FLAG_HIDDEN);
    }
}

int websocket_callback(lws * websocket, lws_callback_reasons reason,
                       void *, void * data, size_t length)
{
    switch(reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            batc_wsi = websocket;
            std::fprintf(stderr, "[WS] BATC spectrum connected\n");
            {
                std::lock_guard<std::mutex> lock(batc_handoff_mutex);
                batc_pending_status = BatcUiStatus::Waiting;
            }
            break;

        case LWS_CALLBACK_CLIENT_RECEIVE: {
            if(lws_is_first_fragment(websocket)) {
                message_buffer.clear();
                message_is_binary = lws_frame_is_binary(websocket) != 0;
            }

            const auto * bytes = static_cast<const uint8_t *>(data);
            message_buffer.insert(message_buffer.end(), bytes, bytes + length);

            if(lws_is_final_fragment(websocket) &&
               lws_remaining_packet_payload(websocket) == 0U) {
                if(message_is_binary) {
                    std::lock_guard<std::mutex> lock(batc_handoff_mutex);
                    batc_pending_message = message_buffer;
                }
                message_buffer.clear();
            }
            break;
        }

        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            batc_wsi = nullptr;
            std::fprintf(stderr, "[WS] BATC connection error: %.*s\n",
                         static_cast<int>(length), data != nullptr ? static_cast<const char *>(data) : "");
            {
                std::lock_guard<std::mutex> lock(batc_handoff_mutex);
                batc_pending_status = BatcUiStatus::ConnectionError;
            }
            break;

        case LWS_CALLBACK_CLIENT_CLOSED:
            batc_wsi = nullptr;
            std::fprintf(stderr, "[WS] BATC connection closed\n");
            {
                std::lock_guard<std::mutex> lock(batc_handoff_mutex);
                batc_pending_status = BatcUiStatus::Disconnected;
            }
            break;

        default:
            break;
    }
    return 0;
}

const lws_protocols protocols[] = {
    {"fft_m0dtslivetune", websocket_callback, 0, 64 * 1024, 0, nullptr, 0},
    LWS_PROTOCOL_LIST_TERM
};

void connect_batc_ws()
{
    lws_client_connect_info connection_info{};
    connection_info.context = websocket_context;
    connection_info.address = SERVER_HOST;
    connection_info.port = SERVER_PORT;
    connection_info.path = SERVER_PATH;
    connection_info.host = SERVER_HOST;
    connection_info.origin = "https://eshail.batc.org.uk";
    connection_info.ssl_connection = LCCSCF_USE_SSL;
    connection_info.local_protocol_name = protocols[0].name;
    connection_info.protocol = protocols[0].name;
    /* Without this, lws (built with LWS_WITH_HTTP2 on this system) offers
     * "h2" in its TLS ALPN list. BATC's nginx front picks it, then 502s
     * the WS upgrade against its backend (reproduced with `curl` defaulting
     * to HTTP/2 vs. `--http1.1`) - surfaces here as an immediate
     * LWS_CALLBACK_CLIENT_CONNECTION_ERROR, retried forever. */
    connection_info.alpn = "http/1.1";

    lws_client_connect_via_info(&connection_info);
}

void batc_websocket_loop()
{
    lws_set_log_level(LLL_ERR | LLL_WARN, nullptr);

    lws_context_creation_info context_info{};
    context_info.port = CONTEXT_PORT_NO_LISTEN;
    context_info.protocols = protocols;
    context_info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

    websocket_context = lws_create_context(&context_info);
    if(websocket_context == nullptr) return;

    connect_batc_ws();
    last_batc_connect_attempt = std::chrono::steady_clock::now();

    while(batc_ws_thread_running.load(std::memory_order_relaxed)) {
        lws_service(websocket_context, 50);

        const auto now = std::chrono::steady_clock::now();
        if(batc_wsi == nullptr && now - last_batc_connect_attempt > BATC_RECONNECT_INTERVAL) {
            last_batc_connect_attempt = now;
            connect_batc_ws();
        }
    }

    lws_context_destroy(websocket_context);
    websocket_context = nullptr;
}

void start_websocket()
{
    batc_ws_thread_running = true;
    batc_ws_thread = std::thread(batc_websocket_loop);
}

/* A single failed/dropped connection attempt used to leave the spectrum
 * panel stuck on "CONNECTION ERROR" forever (no retry) - retry on the same
 * cadence as the local Longmynd WebSocket client. */
void service_websocket(lv_timer_t *)
{
    static bool first_call = true;
    if(first_call) {
        first_call = false;
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - g_boot_t0).count();
        std::fprintf(stderr, "[STARTUP] first service_websocket call at +%.1fms\n", ms);
    }

    std::vector<uint8_t> message;
    BatcUiStatus status = BatcUiStatus::None;
    {
        std::lock_guard<std::mutex> lock(batc_handoff_mutex);
        message.swap(batc_pending_message);
        status = batc_pending_status;
        batc_pending_status = BatcUiStatus::None;
    }

    if(!message.empty()) decode_and_draw(message);

    if(spectrum_status_label != nullptr) {
        switch(status) {
            case BatcUiStatus::Waiting:
                lv_label_set_text(spectrum_status_label, "WAITING FOR FFT...");
                lv_obj_remove_flag(spectrum_status_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_text_color(spectrum_status_label, lv_color_hex(0xFFD166), 0);
                break;
            case BatcUiStatus::ConnectionError:
                lv_label_set_text(spectrum_status_label, "CONNECTION ERROR - RETRYING...");
                lv_obj_remove_flag(spectrum_status_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_text_color(spectrum_status_label, lv_color_hex(0xFF4D4D), 0);
                break;
            case BatcUiStatus::Disconnected:
                lv_label_set_text(spectrum_status_label, "DISCONNECTED - RETRYING...");
                lv_obj_remove_flag(spectrum_status_label, LV_OBJ_FLAG_HIDDEN);
                break;
            case BatcUiStatus::None:
                break;
        }
    }
}

/* ================================================================= */

/* ===================================================================
 * Longmynd + video.
 *
 * Uses the philcrump/longmynd (M0DNY) fork, launched ONCE at startup
 * with its -W <port> WebSocket server enabled, instead of stock
 * Longmynd killed/restarted on every retune (that's what OpenTuner
 * does too - see open_tuner/MediaSources/Longmynd/LongmyndWS.cs).
 * Retuning and status go over that WebSocket (control/monitor
 * protocols below).
 *
 * Video: TS is sent over UDP (-i 127.0.0.1 <port>, exclusive with the
 * FIFO per longmynd_ws/ts.c) rather than a FIFO, matching OpenTuner's
 * architecture. Reason: LVGL's built-in lv_ffmpeg widget reads its
 * source synchronously from the single-threaded UI timer loop - a FIFO
 * read blocks that entire loop (spectrum, WebSockets, everything)
 * whenever Longmynd briefly stops producing TS (e.g. mid-retune while
 * re-acquiring lock), which was the root cause of the original freeze
 * complaint even after moving retuning off the kill/restart path. UDP
 * datagrams are consumed by our own background thread into a ring
 * buffer (see the "video decode pipeline" section further down) so the
 * UI thread never touches a blocking read.
 * =================================================================== */

constexpr const char * LONGMYND_DIR = "/home/daniel/DATVreceiver/longmynd_ws";
const std::string LONGMYND_BIN = std::string(LONGMYND_DIR) + "/longmynd";
const std::string LONGMYND_LOG = std::string(LONGMYND_DIR) + "/longmynd.log";
const std::string LONGMYND_PID_FILE = std::string(LONGMYND_DIR) + "/longmynd.pid";

constexpr int LONGMYND_WS_PORT = 8765;
constexpr int LONGMYND_TS_UDP_PORT = 5600;

/* QO-100 DATV beacon (A71A/QARS), as last confirmed locked on this LNB. */
constexpr long BEACON_FREQ_KHZ = 741474;
constexpr long BEACON_SYMRATE_KSPS = 1500;

pid_t longmynd_pid = -1;

void stop_longmynd()
{
    if(longmynd_pid > 0) {
        kill(longmynd_pid, SIGTERM);
        longmynd_pid = -1;
    }
    std::remove(LONGMYND_PID_FILE.c_str());
}

/* Kills only a longmynd this same app started on a previous run and left
 * orphaned (e.g. after a crash) - identified via LONGMYND_PID_FILE, written
 * below right after fork(). A blanket `pkill -x longmynd` would also take
 * out an unrelated instance, e.g. longmynd/ used by qo100_monitor.py. */
void kill_stale_longmynd()
{
    FILE * pid_file = std::fopen(LONGMYND_PID_FILE.c_str(), "r");
    if(pid_file == nullptr) return;
    int stale_pid = -1;
    const int scanned = std::fscanf(pid_file, "%d", &stale_pid);
    std::fclose(pid_file);
    if(scanned == 1 && stale_pid > 0 && kill(static_cast<pid_t>(stale_pid), 0) == 0)
        kill(static_cast<pid_t>(stale_pid), SIGTERM);
}

/* Launched once at app startup (initial freq/sr just seeds the receiver -
 * all subsequent retunes go over the control WebSocket, see below). */
void start_longmynd(long freq_khz, long symrate_ksps)
{
    kill_stale_longmynd();

    pid_t pid = fork();
    if(pid == 0) {
        chdir(LONGMYND_DIR);
        int fd = open(LONGMYND_LOG.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if(fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
        }
        char ws_port_text[8], udp_port_text[8], freq_text[16], sr_text[16];
        std::snprintf(ws_port_text, sizeof(ws_port_text), "%d", LONGMYND_WS_PORT);
        std::snprintf(udp_port_text, sizeof(udp_port_text), "%d", LONGMYND_TS_UDP_PORT);
        std::snprintf(freq_text, sizeof(freq_text), "%ld", freq_khz);
        std::snprintf(sr_text, sizeof(sr_text), "%ld", symrate_ksps);
        execl("/usr/bin/stdbuf", "stdbuf", "-oL", "-eL",
              LONGMYND_BIN.c_str(), "-W", ws_port_text, "-i", "127.0.0.1", udp_port_text,
              freq_text, sr_text, (char *)nullptr);
        _exit(127);
    }

    longmynd_pid = pid;
    FILE * pid_file = std::fopen(LONGMYND_PID_FILE.c_str(), "w");
    if(pid_file != nullptr) {
        std::fprintf(pid_file, "%d", static_cast<int>(pid));
        std::fclose(pid_file);
    }
}

/* ===================================================================
 * Longmynd status FIFO parsing. Format is documented in
 * longmynd/README.md ("Status Output Interface"): lines of "$id,value".
 * MODCOD tables copied verbatim from that README's "MODCOD Lookup"
 * section - do not invent values not in that table.
 * =================================================================== */

/* LNB local oscillator offset (MHz) and bias-tee voltage - user-configurable
 * via the settings page (gear button, see build_settings_page()). Defaults
 * match a standard QO-100 LNB with externally-fed power (no bias tee). */
double g_lnb_lo_mhz = 9750.0;
bool g_lnb_voltage_enabled = false;
bool g_lnb_voltage_horizontal = false; /* false = 13V (vertical), true = 18V (horizontal) */

struct ModcodEntry { const char * mod; const char * fec; };

const ModcodEntry DVBS_MODCOD[] = {
    {"QPSK", "1/2"}, {"QPSK", "2/3"}, {"QPSK", "3/4"},
    {"QPSK", "5/6"}, {"QPSK", "6/7"}, {"QPSK", "7/8"},
};

const ModcodEntry DVBS2_MODCOD[] = {
    {"---", "---"} /* 0: DummyPL */, {"QPSK", "1/4"}, {"QPSK", "1/3"},
    {"QPSK", "2/5"}, {"QPSK", "1/2"}, {"QPSK", "3/5"}, {"QPSK", "2/3"},
    {"QPSK", "3/4"}, {"QPSK", "4/5"}, {"QPSK", "5/6"}, {"QPSK", "8/9"},
    {"QPSK", "9/10"}, {"8PSK", "3/5"}, {"8PSK", "2/3"}, {"8PSK", "3/4"},
    {"8PSK", "5/6"}, {"8PSK", "8/9"}, {"8PSK", "9/10"}, {"16APSK", "2/3"},
    {"16APSK", "3/4"}, {"16APSK", "4/5"}, {"16APSK", "5/6"}, {"16APSK", "8/9"},
    {"16APSK", "9/10"}, {"32APSK", "3/4"}, {"32APSK", "4/5"}, {"32APSK", "5/6"},
    {"32APSK", "8/9"}, {"32APSK", "9/10"},
};

int rx_state = 0;             /* 0=init 1=searching 2=headers 3=DVB-S 4=DVB-S2 */
bool rx_was_locked = false;   /* tracks the unlock->lock edge - see parse_monitor_json() */
long rx_carrier_khz = 0;
long rx_symrate_ksps = 0;
int rx_mer_x10 = 0;
int rx_modcod = -1;
long rx_agc1 = 0;
long rx_agc2 = 0;
std::string rx_service_provider;
std::string rx_service_name;
int rx_ber_x100 = 0;
int rx_short_frames = -1;   /* -1=unknown, 0=normal, 1=short (DVB-S2 only) */
int rx_pilots = -1;         /* -1=unknown, 0=off, 1=on (DVB-S2 only) */
long rx_ldpc_errors = 0;

/* Debounced: a stuck/double mouse-button event (seen during testing - e.g.
 * residual button-down state from a prior synthetic click landing on a
 * freshly-created window) can fire two retunes in quick succession. Cheap
 * insurance against sending redundant/rapid-fire control commands. */
constexpr auto RETUNE_MIN_INTERVAL = std::chrono::milliseconds(1500);
std::chrono::steady_clock::time_point last_retune_time{};

/* Lock-timeout watchdog: warns if a retune doesn't lock within a few of
 * Longmynd's own retry cycles (it resets/tries the next symbol rate every
 * TS_TIMEOUT_PERIOD, default 5000ms), instead of silently sitting there
 * with no feedback about whether it's still trying or has effectively
 * failed. */
constexpr auto LOCK_WATCHDOG_TIMEOUT = std::chrono::milliseconds(8000);
bool tuning_pending = false;
bool watchdog_timeout_logged = false;

/* Auto-return-to-beacon: watching someone's TX (a non-beacon signal, see
 * retune_exact()) and their signal disappears - most often they just
 * unkeyed, not a real dropout. finish_rx_status_update() arms this on the
 * lock->unlock edge, waits AUTO_BEACON_RETURN_DELAY in case lock comes back
 * on its own (a brief fade, or they're still mid-transmission), and only
 * then calls return_to_beacon(). Never arms while already on the beacon. */
constexpr auto AUTO_BEACON_RETURN_DELAY = std::chrono::milliseconds(2500);
bool tuned_to_beacon = true;
bool auto_beacon_return_armed = false;
std::chrono::steady_clock::time_point auto_beacon_return_deadline{};

lv_obj_t * g_mode_value = nullptr;
lv_obj_t * g_mer_value = nullptr;
lv_obj_t * g_agc_value = nullptr;
lv_obj_t * g_fec_value = nullptr;
lv_obj_t * g_mod_value = nullptr;
lv_obj_t * g_service_value = nullptr;
lv_obj_t * g_ber_value = nullptr;
lv_obj_t * g_ldpc_value = nullptr;
lv_obj_t * g_shortframes_value = nullptr;
lv_obj_t * g_pilots_value = nullptr;
lv_obj_t * g_video_codec_value = nullptr;
lv_obj_t * g_audio_codec_value = nullptr;
lv_obj_t * g_quality_value = nullptr;
lv_obj_t * g_margin_value = nullptr;

/* Approximate MER / EsN0 requirements for quasi-error-free reception,
 * indexed by Longmynd's MODCOD value. They are used as a practical quality
 * reference, not as laboratory pass/fail limits. */
const double DVBS_REQUIRED_MER[] = {1.7, 3.3, 4.2, 5.1, 5.5, 5.8};
const double DVBS2_REQUIRED_MER[] = {
    0.0, -2.35, -1.24, -0.30, 1.00, 2.23, 3.10, 4.03, 4.68, 5.18,
    6.20, 6.42, 5.50, 6.62, 7.91, 9.35, 10.69, 10.98, 8.97, 10.21,
    11.03, 11.61, 12.89, 13.13, 12.73, 13.64, 14.28, 15.69, 16.05
};

bool required_mer_for_current_modcod(double & required)
{
    if(rx_modcod < 0) return false;
    if(rx_state == 4 && static_cast<size_t>(rx_modcod) <
       sizeof(DVBS2_REQUIRED_MER) / sizeof(DVBS2_REQUIRED_MER[0])) {
        if(rx_modcod == 0) return false; /* DummyPL */
        required = DVBS2_REQUIRED_MER[rx_modcod];
        return true;
    }
    if(rx_state == 3 && static_cast<size_t>(rx_modcod) <
       sizeof(DVBS_REQUIRED_MER) / sizeof(DVBS_REQUIRED_MER[0])) {
        required = DVBS_REQUIRED_MER[rx_modcod];
        return true;
    }
    return false;
}

void update_status_ui()
{
    const bool locked = (rx_state == 3 || rx_state == 4);
    double required_mer = 0.0;
    const bool have_required_mer = locked && required_mer_for_current_modcod(required_mer);
    const double mer_margin = rx_mer_x10 / 10.0 - required_mer;
    const char * quality = "---";
    lv_color_t quality_color = COLOR_TEXT_DIM;
    if(have_required_mer) {
        if(mer_margin >= 4.0) {
            quality = "Excellent";
            quality_color = COLOR_GREEN;
        }
        else if(mer_margin >= 2.0) {
            quality = "Good";
            quality_color = COLOR_GREEN;
        }
        else if(mer_margin >= 0.5) {
            quality = "Marginal";
            quality_color = COLOR_YELLOW;
        }
        else {
            quality = "Poor";
            quality_color = COLOR_RED;
        }
    }

    if(locked && tuning_pending) {
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - last_retune_time).count();
        std::fprintf(stderr,
            "[TUNE] LOCKED after %lld ms: mode=%s carrier=%ld kHz sr=%ld kS/s mer=%.1f dB service=%s\n",
            static_cast<long long>(elapsed_ms), rx_state == 4 ? "DVB-S2" : "DVB-S",
            rx_carrier_khz, rx_symrate_ksps, rx_mer_x10 / 10.0,
            (!rx_service_name.empty() ? rx_service_name : rx_service_provider).c_str());
        tuning_pending = false;
    }

    if(g_mode_value != nullptr) {
        const char * mode = rx_state == 4 ? "DVB-S2" : rx_state == 3 ? "DVB-S" : "---";
        lv_label_set_text(g_mode_value, mode);
        lv_obj_set_style_text_color(g_mode_value, locked ? COLOR_TEXT : COLOR_TEXT_DIM, 0);
    }

    if(g_mer_value != nullptr) {
        if(locked) {
            char text[16];
            std::snprintf(text, sizeof(text), "%.1f dB", rx_mer_x10 / 10.0);
            lv_label_set_text(g_mer_value, text);
            lv_obj_set_style_text_color(g_mer_value,
                                        have_required_mer ? quality_color : COLOR_TEXT, 0);
        }
        else {
            lv_label_set_text(g_mer_value, "---");
            lv_obj_set_style_text_color(g_mer_value, COLOR_TEXT_DIM, 0);
        }
    }

    if(g_agc_value != nullptr) {
        if(locked) {
            char text[16];
            const double agc_pct = ((rx_agc1 + rx_agc2) / 2.0) / 65535.0 * 100.0;
            std::snprintf(text, sizeof(text), "%.0f%%", agc_pct);
            lv_label_set_text(g_agc_value, text);
            lv_obj_set_style_text_color(g_agc_value, COLOR_GREEN, 0);
        }
        else {
            lv_label_set_text(g_agc_value, "---");
            lv_obj_set_style_text_color(g_agc_value, COLOR_TEXT_DIM, 0);
        }
    }

    const ModcodEntry * table = rx_state == 4 ? DVBS2_MODCOD : DVBS_MODCOD;
    const size_t table_len = rx_state == 4
        ? sizeof(DVBS2_MODCOD) / sizeof(DVBS2_MODCOD[0])
        : sizeof(DVBS_MODCOD) / sizeof(DVBS_MODCOD[0]);
    const bool have_modcod = locked && rx_modcod >= 0 &&
        static_cast<size_t>(rx_modcod) < table_len;

    if(g_mod_value != nullptr) {
        lv_label_set_text(g_mod_value, have_modcod ? table[rx_modcod].mod : "---");
        lv_obj_set_style_text_color(g_mod_value, have_modcod ? COLOR_TEXT : COLOR_TEXT_DIM, 0);
    }
    if(g_fec_value != nullptr) {
        lv_label_set_text(g_fec_value, have_modcod ? table[rx_modcod].fec : "---");
        lv_obj_set_style_text_color(g_fec_value, have_modcod ? COLOR_TEXT : COLOR_TEXT_DIM, 0);
    }

    if(g_service_value != nullptr) {
        const std::string & service = !rx_service_name.empty() ? rx_service_name : rx_service_provider;
        lv_label_set_text(g_service_value, locked && !service.empty() ? service.c_str() : "---");
        lv_obj_set_style_text_color(g_service_value,
                                    (locked && !service.empty()) ? COLOR_GREEN : COLOR_TEXT_DIM, 0);
    }

    if(g_ber_value != nullptr) {
        if(locked) {
            char text[16];
            std::snprintf(text, sizeof(text), "%.2f%%", rx_ber_x100 / 100.0);
            lv_label_set_text(g_ber_value, text);
            lv_obj_set_style_text_color(g_ber_value, COLOR_TEXT, 0);
        }
        else {
            lv_label_set_text(g_ber_value, "---");
            lv_obj_set_style_text_color(g_ber_value, COLOR_TEXT_DIM, 0);
        }
    }

    /* Short Frames / Pilots only apply to DVB-S2 (rx_state==4). */
    const bool have_s2_flags = locked && rx_state == 4 && rx_short_frames >= 0 && rx_pilots >= 0;
    if(g_shortframes_value != nullptr) {
        lv_label_set_text(g_shortframes_value, have_s2_flags ? (rx_short_frames ? "Short" : "Normal") : "---");
        lv_obj_set_style_text_color(g_shortframes_value, have_s2_flags ? COLOR_TEXT : COLOR_TEXT_DIM, 0);
    }
    if(g_pilots_value != nullptr) {
        lv_label_set_text(g_pilots_value, have_s2_flags ? (rx_pilots ? "On" : "Off") : "---");
        lv_obj_set_style_text_color(g_pilots_value, have_s2_flags ? COLOR_TEXT : COLOR_TEXT_DIM, 0);
    }

    if(g_ldpc_value != nullptr) {
        if(locked && rx_state == 4) {
            char text[16];
            std::snprintf(text, sizeof(text), "%ld", rx_ldpc_errors);
            lv_label_set_text(g_ldpc_value, text);
            lv_obj_set_style_text_color(g_ldpc_value, COLOR_TEXT, 0);
        }
        else {
            lv_label_set_text(g_ldpc_value, "---");
            lv_obj_set_style_text_color(g_ldpc_value, COLOR_TEXT_DIM, 0);
        }
    }

    if(g_quality_value != nullptr) {
        lv_label_set_text(g_quality_value, quality);
        lv_obj_set_style_text_color(g_quality_value, quality_color, 0);
    }
    if(g_margin_value != nullptr) {
        if(have_required_mer) {
            char text[20];
            std::snprintf(text, sizeof(text), "%+.1f dB", mer_margin);
            lv_label_set_text(g_margin_value, text);
            lv_obj_set_style_text_color(g_margin_value, quality_color, 0);
        }
        else {
            lv_label_set_text(g_margin_value, "---");
            lv_obj_set_style_text_color(g_margin_value, COLOR_TEXT_DIM, 0);
        }
    }
}

/* ===================================================================
 * Longmynd control + monitor WebSocket client. Replaces FIFO status
 * polling and kill/restart retuning with philcrump/longmynd's live
 * protocol (see longmynd_ws/web/web.c for the server side, and
 * open_tuner/MediaSources/Longmynd/LongmyndWS.cs for the reference
 * client this mirrors): a persistent "monitor" connection streams JSON
 * status, and a persistent "control" connection accepts "C<freq>,<sr>"
 * to retune - no process restart, so no lock/video gap.
 *
 * web_status_json()'s "frequency"/"symbolrate"/"ber"/"mer" fields carry
 * the exact same raw scaling as stock Longmynd's status FIFO (verified
 * against longmynd_ws/main.c: same status_write() calls feed both) -
 * so the conversions below match the old parse_status_line() exactly.
 * =================================================================== */

constexpr const char * LOCAL_WS_HOST = "127.0.0.1";

lws_context * longmynd_ws_context = nullptr;
lws * control_wsi = nullptr;
lws * monitor_wsi = nullptr;
/* True while the monitor link to longmynd_ws is up - shown on the settings
 * page so a tuner that enumerated on USB but failed to open (wrong udev
 * rule, permission denied, etc.) is visible without a dmesg/journalctl dive. */
std::atomic<bool> g_monitor_ws_connected{false};
std::vector<uint8_t> monitor_message_buffer;
bool monitor_message_is_binary = false;
std::string pending_control_cmd;
bool control_cmd_ready = false;
std::chrono::steady_clock::time_point last_ws_connect_attempt{};
constexpr auto WS_RECONNECT_INTERVAL = std::chrono::milliseconds(1000);
std::thread local_ws_thread;
std::atomic<bool> local_ws_thread_running{false};
std::mutex local_ws_handoff_mutex;
std::string pending_monitor_json;

const std::string LONGMYND_STATUS_FIFO = std::string(LONGMYND_DIR) + "/longmynd_main_status";
int status_fifo_fd = -1;
std::string status_fifo_buffer;

int json_get_int(json_object * obj, const char * key, int fallback)
{
    json_object * value = nullptr;
    if(obj != nullptr && json_object_object_get_ex(obj, key, &value))
        return json_object_get_int(value);
    return fallback;
}

long json_get_long(json_object * obj, const char * key, long fallback)
{
    json_object * value = nullptr;
    if(obj != nullptr && json_object_object_get_ex(obj, key, &value))
        return std::lround(json_object_get_double(value));
    return fallback;
}

int json_get_bool_as_int(json_object * obj, const char * key, int fallback)
{
    json_object * value = nullptr;
    if(obj != nullptr && json_object_object_get_ex(obj, key, &value))
        return json_object_get_boolean(value) ? 1 : 0;
    return fallback;
}

std::string json_get_string(json_object * obj, const char * key, const std::string & fallback)
{
    json_object * value = nullptr;
    if(obj != nullptr && json_object_object_get_ex(obj, key, &value)) {
        const char * text = json_object_get_string(value);
        return text != nullptr ? text : fallback;
    }
    return fallback;
}

void finish_rx_status_update()
{
    const bool locked_now = (rx_state == 3 || rx_state == 4);
    if(locked_now && !rx_was_locked) {
        std::fprintf(stderr, "[TUNE] lock established - resetting video decoder\n");
        request_video_reset();
    }
    if(!locked_now && rx_was_locked && !tuned_to_beacon) {
        auto_beacon_return_armed = true;
        auto_beacon_return_deadline = std::chrono::steady_clock::now() + AUTO_BEACON_RETURN_DELAY;
        std::fprintf(stderr, "[TUNE] lock lost on non-beacon signal - returning to beacon in %lldms unless it comes back\n",
                     static_cast<long long>(AUTO_BEACON_RETURN_DELAY.count()));
    }
    if(locked_now && auto_beacon_return_armed) {
        auto_beacon_return_armed = false;
        std::fprintf(stderr, "[TUNE] lock regained - cancelled pending return to beacon\n");
    }
    rx_was_locked = locked_now;

    const std::string & service = !rx_service_name.empty() ? rx_service_name : rx_service_provider;
    if(locked_now && rx_carrier_khz > 0 && !service.empty()) {
        const double frequency_mhz = g_lnb_lo_mhz + rx_carrier_khz / 1000.0;
        auto existing = std::find_if(decoded_services.begin(), decoded_services.end(),
            [&](const DecodedService & item) {
                return item.callsign == service || std::abs(item.frequency_mhz - frequency_mhz) < 0.05;
            });
        if(existing != decoded_services.end()) {
            existing->frequency_mhz = frequency_mhz;
            existing->callsign = service;
        }
        else {
            decoded_services.push_back({frequency_mhz, service});
        }

        /* The cyan marker is useful while selecting/tuning, but once a
         * named service is decoded the green callsign identifies the peak
         * more clearly without competing text at the top of the plot. */
        if(tune_marker != nullptr) lv_obj_add_flag(tune_marker, LV_OBJ_FLAG_HIDDEN);
        if(tune_marker_label != nullptr) lv_obj_add_flag(tune_marker_label, LV_OBJ_FLAG_HIDDEN);
    }

    update_status_ui();

    if(tuning_pending && !watchdog_timeout_logged &&
       std::chrono::steady_clock::now() - last_retune_time > LOCK_WATCHDOG_TIMEOUT) {
        std::fprintf(stderr,
            "[TUNE] WARNING: no lock %lld ms after retune (state=%d) - still searching\n",
            static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                LOCK_WATCHDOG_TIMEOUT).count()),
            rx_state);
        watchdog_timeout_logged = true;
        /* Give up covering the stale frame too - a weak/no signal on the new
         * frequency means no fresh frame is ever coming to hide it for us,
         * and leaving the overlay up forever would hide that fact. */
        if(tuning_overlay != nullptr) lv_obj_add_flag(tuning_overlay, LV_OBJ_FLAG_HIDDEN);
    }

    if(auto_beacon_return_armed && std::chrono::steady_clock::now() >= auto_beacon_return_deadline) {
        auto_beacon_return_armed = false;
        return_to_beacon();
    }
}

void parse_monitor_json(const std::string & text)
{
    json_object * root = json_tokener_parse(text.c_str());
    if(root == nullptr) return;

    json_object * packet = nullptr;
    json_object * rx = nullptr;
    json_object * ts = nullptr;
    json_object_object_get_ex(root, "packet", &packet);
    if(packet != nullptr) {
        json_object_object_get_ex(packet, "rx", &rx);
        json_object_object_get_ex(packet, "ts", &ts);
    }

    if(rx != nullptr) {
        rx_state = json_get_int(rx, "demod_state", rx_state);
        rx_carrier_khz = json_get_long(rx, "frequency", rx_carrier_khz);
        rx_symrate_ksps = (json_get_long(rx, "symbolrate", rx_symrate_ksps * 1000) + 500) / 1000;
        rx_ber_x100 = json_get_int(rx, "ber", rx_ber_x100);
        rx_mer_x10 = json_get_int(rx, "mer", rx_mer_x10);
        rx_modcod = json_get_int(rx, "modcod", rx_modcod);
        rx_short_frames = json_get_bool_as_int(rx, "short_frame", rx_short_frames);
        rx_pilots = json_get_bool_as_int(rx, "pilot_symbols", rx_pilots);
        rx_ldpc_errors = json_get_long(rx, "errors_ldpc_count", rx_ldpc_errors);
        rx_agc1 = json_get_long(rx, "agc1", rx_agc1);
        rx_agc2 = json_get_long(rx, "agc2", rx_agc2);
    }
    if(ts != nullptr) {
        rx_service_name = json_get_string(ts, "service_name", rx_service_name);
        rx_service_provider = json_get_string(ts, "service_provider_name", rx_service_provider);
    }

    json_object_put(root);

    /* Mirrors OpenTuner's LongmyndWS.cs Monitorws_OnMessage: reset the video
     * decode pipeline on the unlock->lock edge, not on the retune command
     * itself. A live retune (config_set_frequency_and_symbolrate(), no
     * process restart) still makes Longmynd's receiver thread drop through
     * "searching" before it re-locks, so this edge reliably fires once per
     * retune - and only once the new service's PAT/PMT is actually live,
     * which a blind reset-on-command wouldn't guarantee. Since the same
     * Longmynd process keeps streaming continuously, a locked DIFFERENT
     * service can otherwise leave the demuxer holding stale PID/codec info
     * from the previous lock. */
    finish_rx_status_update();
}

void parse_status_fifo_line(const std::string & line)
{
    if(line.size() < 4 || line[0] != '$') return;
    const size_t comma = line.find(',');
    if(comma == std::string::npos) return;

    const int id = std::atoi(line.c_str() + 1);
    const char * value_text = line.c_str() + comma + 1;
    const long value = std::strtol(value_text, nullptr, 10);

    switch(id) {
        case 1:  rx_state = static_cast<int>(value); break;
        case 6:  rx_carrier_khz = value; break;
        case 9:  rx_symrate_ksps = (value + 500) / 1000; break;
        case 11: rx_ber_x100 = static_cast<int>(value); break;
        case 12: rx_mer_x10 = static_cast<int>(value); break;
        case 13: rx_service_name = value_text; break;
        case 14: rx_service_provider = value_text; break;
        case 18: rx_modcod = static_cast<int>(value); break;
        case 19: rx_short_frames = static_cast<int>(value); break;
        case 20: rx_pilots = static_cast<int>(value); break;
        case 21: rx_ldpc_errors = value; break;
        case 26: rx_agc1 = value; break;
        case 27: rx_agc2 = value; break;
        default: break;
    }
}

void service_status_fifo(lv_timer_t *)
{
    if(status_fifo_fd < 0) {
        status_fifo_fd = open(LONGMYND_STATUS_FIFO.c_str(), O_RDONLY | O_NONBLOCK);
        if(status_fifo_fd < 0) return;
    }

    char buf[4096];
    bool received = false;
    for(;;) {
        const ssize_t n = read(status_fifo_fd, buf, sizeof(buf));
        if(n <= 0) break;
        status_fifo_buffer.append(buf, static_cast<size_t>(n));
        received = true;
    }

    size_t newline;
    while((newline = status_fifo_buffer.find('\n')) != std::string::npos) {
        std::string line = status_fifo_buffer.substr(0, newline);
        status_fifo_buffer.erase(0, newline + 1);
        if(!line.empty() && line.back() == '\r') line.pop_back();
        parse_status_fifo_line(line);
    }
    if(received) finish_rx_status_update();
}

int callback_monitor(lws * websocket, lws_callback_reasons reason,
                     void *, void * data, size_t length)
{
    switch(reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            monitor_wsi = websocket;
            g_monitor_ws_connected = true;
            std::fprintf(stderr, "[WS] monitor connected\n");
            break;

        case LWS_CALLBACK_CLIENT_RECEIVE: {
            if(lws_is_first_fragment(websocket)) {
                monitor_message_buffer.clear();
                monitor_message_is_binary = lws_frame_is_binary(websocket) != 0;
            }
            const auto * bytes = static_cast<const uint8_t *>(data);
            monitor_message_buffer.insert(monitor_message_buffer.end(), bytes, bytes + length);

            if(lws_is_final_fragment(websocket) &&
               lws_remaining_packet_payload(websocket) == 0U) {
                if(!monitor_message_is_binary) {
                    std::lock_guard<std::mutex> lock(local_ws_handoff_mutex);
                    pending_monitor_json.assign(monitor_message_buffer.begin(),
                                                monitor_message_buffer.end());
                }
                monitor_message_buffer.clear();
            }
            break;
        }

        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        case LWS_CALLBACK_CLIENT_CLOSED:
            if(monitor_wsi == websocket) {
                monitor_wsi = nullptr;
                g_monitor_ws_connected = false;
            }
            break;

        default:
            break;
    }
    return 0;
}

int callback_control(lws * websocket, lws_callback_reasons reason,
                     void *, void * data, size_t length)
{
    (void)data;
    (void)length;

    switch(reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            control_wsi = websocket;
            std::fprintf(stderr, "[WS] control connected\n");
            {
                std::lock_guard<std::mutex> lock(local_ws_handoff_mutex);
                if(control_cmd_ready) lws_callback_on_writable(websocket);
            }
            break;

        case LWS_CALLBACK_CLIENT_WRITEABLE: {
            std::string cmd;
            {
                std::lock_guard<std::mutex> lock(local_ws_handoff_mutex);
                if(control_cmd_ready) {
                    cmd = pending_control_cmd;
                    control_cmd_ready = false;
                }
            }
            if(!cmd.empty()) {
                std::vector<uint8_t> buf(LWS_PRE + cmd.size());
                std::memcpy(buf.data() + LWS_PRE, cmd.data(), cmd.size());
                lws_write(websocket, buf.data() + LWS_PRE, cmd.size(), LWS_WRITE_TEXT);
                std::fprintf(stderr, "[WS] control sent: %s\n", cmd.c_str());
            }
            break;
        }

        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        case LWS_CALLBACK_CLIENT_CLOSED:
            if(control_wsi == websocket) control_wsi = nullptr;
            break;

        default:
            break;
    }
    return 0;
}

const lws_protocols local_protocols[] = {
    {"monitor", callback_monitor, 0, 16 * 1024, 0, nullptr, 0},
    {"control", callback_control, 0, 256, 0, nullptr, 0},
    LWS_PROTOCOL_LIST_TERM
};

void connect_monitor_ws()
{
    lws_client_connect_info info{};
    info.context = longmynd_ws_context;
    info.address = LOCAL_WS_HOST;
    info.port = LONGMYND_WS_PORT;
    info.path = "/";
    info.host = LOCAL_WS_HOST;
    info.origin = LOCAL_WS_HOST;
    /* Both fields are required: local_protocol_name only selects which of
     * OUR callbacks handles this wsi - .protocol is what actually gets sent
     * as the Sec-WebSocket-Protocol request header. Without it, the server
     * (longmynd_ws/web/web.c) silently binds the connection to its "http"
     * dummy protocol instead of "monitor"/"control" and never includes it
     * in status broadcasts - the connection looks fine (ESTABLISHED) but
     * never receives anything. */
    info.local_protocol_name = "monitor";
    info.protocol = "monitor";
    lws_client_connect_via_info(&info);
}

void connect_control_ws()
{
    lws_client_connect_info info{};
    info.context = longmynd_ws_context;
    info.address = LOCAL_WS_HOST;
    info.port = LONGMYND_WS_PORT;
    info.path = "/";
    info.host = LOCAL_WS_HOST;
    info.origin = LOCAL_WS_HOST;
    info.local_protocol_name = "control";
    info.protocol = "control";
    lws_client_connect_via_info(&info);
}

void local_websocket_loop()
{
    lws_context_creation_info context_info{};
    context_info.port = CONTEXT_PORT_NO_LISTEN;
    context_info.protocols = local_protocols;

    longmynd_ws_context = lws_create_context(&context_info);
    if(longmynd_ws_context == nullptr) return;

    connect_monitor_ws();
    connect_control_ws();
    last_ws_connect_attempt = std::chrono::steady_clock::now();

    while(local_ws_thread_running.load(std::memory_order_relaxed)) {
        lws_service(longmynd_ws_context, 50);

        const auto now = std::chrono::steady_clock::now();
        if(now - last_ws_connect_attempt > WS_RECONNECT_INTERVAL) {
            last_ws_connect_attempt = now;
            if(monitor_wsi == nullptr) connect_monitor_ws();
            if(control_wsi == nullptr) connect_control_ws();
        }

        bool command_waiting = false;
        {
            std::lock_guard<std::mutex> lock(local_ws_handoff_mutex);
            command_waiting = control_cmd_ready;
        }
        if(command_waiting && control_wsi != nullptr)
            lws_callback_on_writable(control_wsi);
    }

    lws_context_destroy(longmynd_ws_context);
    longmynd_ws_context = nullptr;
}

void start_local_websocket()
{
    local_ws_thread_running = true;
    local_ws_thread = std::thread(local_websocket_loop);
}

/* Retries whichever of monitor/control isn't connected - covers both the
 * startup race (our client tries before Longmynd's -W server has bound
 * its port yet) and any later drop. */
void service_local_websocket(lv_timer_t *)
{
    std::string json;
    {
        std::lock_guard<std::mutex> lock(local_ws_handoff_mutex);
        json.swap(pending_monitor_json);
    }
    if(!json.empty()) parse_monitor_json(json);
}

void send_control_command(const std::string & cmd)
{
    {
        std::lock_guard<std::mutex> lock(local_ws_handoff_mutex);
        pending_control_cmd = cmd;
        control_cmd_ready = true;
    }
    if(longmynd_ws_context != nullptr) lws_cancel_service(longmynd_ws_context);
}

/* ===================================================================
 * Settings persistence (LNB LO offset, LNB bias-tee voltage, audio volume).
 * Loaded once at startup and rewritten whenever the settings page's SAVE
 * button is pressed - see build_settings_page() - or the volume slider
 * changes - see volume_slider_cb().
 * =================================================================== */

constexpr const char * SETTINGS_FILE = "/home/daniel/DATVreceiver/qo100_lvgl/settings.json";

/* Declared here (ahead of its normal home near the audio code below) so
 * load_settings()/save_settings() can reach it. */
std::atomic<int> audio_volume_percent{50};

void load_settings()
{
    json_object * root = json_object_from_file(SETTINGS_FILE);
    if(root == nullptr) return;

    json_object * value = nullptr;
    if(json_object_object_get_ex(root, "lnb_lo_mhz", &value))
        g_lnb_lo_mhz = json_object_get_double(value);
    if(json_object_object_get_ex(root, "lnb_voltage_enabled", &value))
        g_lnb_voltage_enabled = json_object_get_boolean(value);
    if(json_object_object_get_ex(root, "lnb_voltage_horizontal", &value))
        g_lnb_voltage_horizontal = json_object_get_boolean(value);
    if(json_object_object_get_ex(root, "audio_volume_percent", &value))
        audio_volume_percent.store(json_object_get_int(value), std::memory_order_relaxed);

    json_object_put(root);
}

void save_settings()
{
    json_object * root = json_object_new_object();
    json_object_object_add(root, "lnb_lo_mhz", json_object_new_double(g_lnb_lo_mhz));
    json_object_object_add(root, "lnb_voltage_enabled", json_object_new_boolean(g_lnb_voltage_enabled));
    json_object_object_add(root, "lnb_voltage_horizontal", json_object_new_boolean(g_lnb_voltage_horizontal));
    json_object_object_add(root, "audio_volume_percent",
                            json_object_new_int(audio_volume_percent.load(std::memory_order_relaxed)));
    json_object_to_file_ext(SETTINGS_FILE, root, JSON_C_TO_STRING_PRETTY);
    json_object_put(root);
}

/* Sends longmynd_ws's live "V<enabled>,<horizontal>" control command (see
 * longmynd_ws/web/web.c's LWS_CALLBACK_RECEIVE handler and main.c's
 * config_set_lnbv()) - applied without a process restart, same mechanism
 * retune_exact() uses for frequency/symbol rate. */
void apply_lnb_voltage()
{
    char cmd[16];
    std::snprintf(cmd, sizeof(cmd), "V%d,%d",
                  g_lnb_voltage_enabled ? 1 : 0, g_lnb_voltage_horizontal ? 1 : 0);
    send_control_command(cmd);
}

/* ===================================================================
 * BATC Wideband chat. The BATC endpoint speaks Socket.IO over an
 * Engine.IO v4 WebSocket. Network callbacks only move complete packets
 * through a mutex; all LVGL work remains on the display thread.
 * =================================================================== */

constexpr const char * CHAT_PATH =
    "/wb/chat/socket.io/?EIO=4&transport=websocket&room=eshail-wb";
lws_context * chat_ws_context = nullptr;
lws * chat_wsi = nullptr;
std::thread chat_ws_thread;
std::atomic<bool> chat_ws_running{false};
std::mutex chat_mutex;
std::vector<std::string> chat_packets;
std::deque<std::string> chat_outgoing;
std::string chat_rx_packet;
std::chrono::steady_clock::time_point last_chat_connect_attempt{};

lv_obj_t * chat_status_label = nullptr;
lv_obj_t * chat_viewers_label = nullptr;
lv_obj_t * chat_history_label = nullptr;
lv_obj_t * chat_history_box = nullptr;
lv_obj_t * chat_users_label = nullptr;
lv_obj_t * chat_nick_input = nullptr;
lv_obj_t * chat_message_input = nullptr;
lv_obj_t * chat_keyboard = nullptr;
lv_obj_t * chat_keyboard_mode_btn = nullptr;
lv_obj_t * chat_keyboard_mode_label = nullptr;
lv_font_t * chat_font_16 = nullptr;
struct ChatLine {
    std::string time;
    std::string name;
    std::string message;
};
std::deque<ChatLine> chat_lines;

const lv_font_t * get_chat_font()
{
    if(chat_font_16 != nullptr) return chat_font_16;

    char executable[PATH_MAX]{};
    const ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1U);
    if(length > 0) {
        executable[length] = '\0';
        char * slash = std::strrchr(executable, '/');
        if(slash != nullptr) {
            std::strcpy(slash + 1, "Montserrat-Medium.ttf");
            const std::string lvgl_path = "A:" + std::string(executable);
            chat_font_16 = lv_tiny_ttf_create_file(lvgl_path.c_str(), 16);
            /* Montserrat TTF provides Unicode text but not LVGL's private-use
             * icon codepoints used by Backspace, Shift, Enter, etc. Resolve
             * those missing glyphs through LVGL's symbol-equipped font. */
            if(chat_font_16 != nullptr)
                chat_font_16->fallback = &lv_font_montserrat_16;
        }
    }
    if(chat_font_16 == nullptr)
        std::fprintf(stderr, "[CHAT] Could not load Unicode font; using LVGL fallback\n");
    return chat_font_16 != nullptr ? chat_font_16 : &lv_font_montserrat_16;
}

void chat_queue_packet(const std::string & packet)
{
    {
        std::lock_guard<std::mutex> lock(chat_mutex);
        chat_outgoing.push_back(packet);
    }
    if(chat_ws_context != nullptr) lws_cancel_service(chat_ws_context);
}

int chat_websocket_callback(lws * websocket, lws_callback_reasons reason,
                            void *, void * data, size_t length)
{
    switch(reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            chat_wsi = websocket;
            break;
        case LWS_CALLBACK_CLIENT_RECEIVE: {
            if(lws_is_first_fragment(websocket)) chat_rx_packet.clear();
            chat_rx_packet.append(static_cast<const char *>(data), length);
            if(lws_is_final_fragment(websocket) &&
               lws_remaining_packet_payload(websocket) == 0U) {
                std::lock_guard<std::mutex> lock(chat_mutex);
                chat_packets.push_back(chat_rx_packet);
                chat_rx_packet.clear();
            }
            break;
        }
        case LWS_CALLBACK_CLIENT_WRITEABLE: {
            std::string packet;
            {
                std::lock_guard<std::mutex> lock(chat_mutex);
                if(!chat_outgoing.empty()) {
                    packet = std::move(chat_outgoing.front());
                    chat_outgoing.pop_front();
                }
            }
            if(!packet.empty()) {
                std::vector<unsigned char> buffer(LWS_PRE + packet.size());
                std::memcpy(buffer.data() + LWS_PRE, packet.data(), packet.size());
                lws_write(websocket, buffer.data() + LWS_PRE, packet.size(), LWS_WRITE_TEXT);
            }
            {
                std::lock_guard<std::mutex> lock(chat_mutex);
                if(!chat_outgoing.empty()) lws_callback_on_writable(websocket);
            }
            break;
        }
        case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
            if(chat_wsi != nullptr) lws_callback_on_writable(chat_wsi);
            break;
        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        case LWS_CALLBACK_CLIENT_CLOSED:
            chat_wsi = nullptr;
            {
                std::lock_guard<std::mutex> lock(chat_mutex);
                chat_packets.emplace_back("__DISCONNECTED__");
            }
            break;
        default:
            break;
    }
    return 0;
}

const lws_protocols chat_protocols[] = {
    {"qo100_chat", chat_websocket_callback, 0, 128 * 1024, 0, nullptr, 0},
    LWS_PROTOCOL_LIST_TERM
};

void connect_chat_ws()
{
    lws_client_connect_info info{};
    info.context = chat_ws_context;
    info.address = SERVER_HOST;
    info.port = SERVER_PORT;
    info.path = CHAT_PATH;
    info.host = SERVER_HOST;
    info.origin = "https://eshail.batc.org.uk";
    info.ssl_connection = LCCSCF_USE_SSL;
    info.local_protocol_name = chat_protocols[0].name;
    info.protocol = chat_protocols[0].name;
    info.alpn = "http/1.1";
    lws_client_connect_via_info(&info);
}

void chat_websocket_loop()
{
    lws_context_creation_info context_info{};
    context_info.port = CONTEXT_PORT_NO_LISTEN;
    context_info.protocols = chat_protocols;
    context_info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    chat_ws_context = lws_create_context(&context_info);
    if(chat_ws_context == nullptr) return;

    connect_chat_ws();
    last_chat_connect_attempt = std::chrono::steady_clock::now();
    while(chat_ws_running.load(std::memory_order_relaxed)) {
        lws_service(chat_ws_context, 50);
        const auto now = std::chrono::steady_clock::now();
        if(chat_wsi == nullptr && now - last_chat_connect_attempt > BATC_RECONNECT_INTERVAL) {
            last_chat_connect_attempt = now;
            connect_chat_ws();
        }
    }
    lws_context_destroy(chat_ws_context);
    chat_ws_context = nullptr;
}

void start_chat_websocket()
{
    chat_ws_running = true;
    chat_ws_thread = std::thread(chat_websocket_loop);
}

std::string json_string(json_object * object, const char * key)
{
    json_object * value = nullptr;
    return json_object_object_get_ex(object, key, &value) &&
           json_object_is_type(value, json_type_string)
        ? json_object_get_string(value) : "";
}

void redraw_chat_history()
{
    if(chat_history_box == nullptr) return;
    lv_obj_clean(chat_history_box);

    const lv_font_t * font = get_chat_font();
    constexpr int row_w = 760;
    constexpr int time_w = 68;
    int y = 8;
    for(const ChatLine & line : chat_lines) {
        lv_obj_t * row = lv_obj_create(chat_history_box);
        lv_obj_set_pos(row, 10, y);
        lv_obj_set_width(row, row_w);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t * time_label = make_label(row, line.time.c_str(),
                                           lv_color_hex(0x9aa0a6), font);
        lv_obj_set_pos(time_label, 0, 0);
        lv_obj_set_width(time_label, time_w);

        lv_obj_t * body = make_label(row, "", COLOR_TEXT, font);
        const std::string body_text = "#ffde2d " + line.name + "#  " + line.message;
        lv_label_set_recolor(body, true);
        lv_label_set_text(body, body_text.c_str());
        lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
        lv_obj_set_pos(body, time_w, 0);
        lv_obj_set_width(body, row_w - time_w);
        lv_obj_update_layout(body);

        const int row_h = std::max(22, static_cast<int>(lv_obj_get_height(body)) + 2);
        lv_obj_set_height(row, row_h);
        y += row_h;
    }
    lv_obj_scroll_to_y(chat_history_box, LV_COORD_MAX, LV_ANIM_OFF);
}

void append_chat_line(json_object * item)
{
    std::string time = json_string(item, "time");
    if(time.size() >= 16U && time[10] == 'T') time = time.substr(11, 5);
    else if(time.size() > 5U) time = time.substr(0, 5);
    chat_lines.push_back({time, json_string(item, "name"),
                          json_string(item, "message")});
    if(chat_lines.size() > 500U) chat_lines.pop_front();
}

void update_chat_users(json_object * object)
{
    json_object * nicks = nullptr;
    if(!json_object_object_get_ex(object, "nicks", &nicks) ||
       !json_object_is_type(nicks, json_type_array)) return;
    std::string text;
    const size_t count = json_object_array_length(nicks);
    for(size_t i = 0; i < count; ++i) {
        json_object * nick = json_object_array_get_idx(nicks, i);
        text += "#d8dee9 ";
        text += json_object_get_string(nick);
        text += "#\n";
    }
    lv_label_set_text(chat_users_label, text.c_str());
}

void process_chat_event(const std::string & packet)
{
    if(packet == "__DISCONNECTED__") {
        lv_label_set_text(chat_status_label, "RECONNECTING...");
        lv_obj_set_style_text_color(chat_status_label, COLOR_YELLOW, 0);
        return;
    }
    /* Engine.IO open arrives before the Socket.IO namespace connection. */
    if(!packet.empty() && packet[0] == '0') { chat_queue_packet("40"); return; }
    if(packet == "2") { chat_queue_packet("3"); return; }
    if(packet.rfind("40", 0) == 0) {
        lv_label_set_text(chat_status_label, "CONNECTED");
        lv_obj_set_style_text_color(chat_status_label, COLOR_GREEN, 0);
        return;
    }
    if(packet.rfind("42", 0) != 0) return;

    json_object * root = json_tokener_parse(packet.c_str() + 2);
    if(root == nullptr || !json_object_is_type(root, json_type_array) ||
       json_object_array_length(root) < 2U) {
        if(root != nullptr) json_object_put(root);
        return;
    }
    const char * event = json_object_get_string(json_object_array_get_idx(root, 0));
    json_object * payload = json_object_array_get_idx(root, 1);
    if(std::strcmp(event, "history") == 0) {
        chat_lines.clear();
        json_object * history = nullptr;
        if(json_object_object_get_ex(payload, "history", &history) &&
           json_object_is_type(history, json_type_array)) {
            const size_t count = json_object_array_length(history);
            for(size_t i = 0; i < count; ++i)
                append_chat_line(json_object_array_get_idx(history, i));
        }
        update_chat_users(payload);
    }
    else if(std::strcmp(event, "message") == 0) append_chat_line(payload);
    else if(std::strcmp(event, "nicks") == 0) update_chat_users(payload);
    else if(std::strcmp(event, "viewers") == 0) {
        json_object * num = nullptr;
        if(json_object_object_get_ex(payload, "num", &num)) {
            std::string viewers = "VIEWERS: " + std::string(json_object_get_string(num));
            lv_label_set_text(chat_viewers_label, viewers.c_str());
        }
    }
    redraw_chat_history();
    json_object_put(root);
}

void service_chat_websocket(lv_timer_t *)
{
    std::vector<std::string> packets;
    {
        std::lock_guard<std::mutex> lock(chat_mutex);
        packets.swap(chat_packets);
    }
    for(const std::string & packet : packets) process_chat_event(packet);
}

void chat_send_message()
{
    const char * message = lv_textarea_get_text(chat_message_input);
    if(message == nullptr || *message == '\0') return;
    json_object * data = json_object_new_object();
    json_object_object_add(data, "message", json_object_new_string(message));
    std::string packet = "42[\"message\",";
    packet += json_object_to_json_string_ext(data, JSON_C_TO_STRING_PLAIN);
    packet += "]";
    json_object_put(data);
    chat_queue_packet(packet);
    lv_textarea_set_text(chat_message_input, "");
}

void chat_set_nick_cb(lv_event_t *)
{
    const char * nick = lv_textarea_get_text(chat_nick_input);
    if(nick == nullptr || *nick == '\0') return;
    json_object * data = json_object_new_object();
    json_object_object_add(data, "nick", json_object_new_string(nick));
    std::string packet = "42[\"setnick\",";
    packet += json_object_to_json_string_ext(data, JSON_C_TO_STRING_PLAIN);
    packet += "]";
    json_object_put(data);
    chat_queue_packet(packet);
}

void chat_send_cb(lv_event_t *) { chat_send_message(); }

void chat_textarea_cb(lv_event_t * event)
{
    lv_keyboard_set_textarea(chat_keyboard, static_cast<lv_obj_t *>(lv_event_get_target(event)));
    lv_keyboard_set_mode(chat_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_label_set_text(chat_keyboard_mode_label, "SYM");
    lv_obj_remove_flag(chat_keyboard_mode_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(chat_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(chat_keyboard);
    lv_obj_move_foreground(chat_keyboard_mode_btn);
}

void chat_keyboard_mode_cb(lv_event_t *)
{
    const lv_keyboard_mode_t mode = lv_keyboard_get_mode(chat_keyboard);
    if(mode == LV_KEYBOARD_MODE_SPECIAL) {
        lv_keyboard_set_mode(chat_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
        lv_label_set_text(chat_keyboard_mode_label, "SYM");
    }
    else {
        lv_keyboard_set_mode(chat_keyboard, LV_KEYBOARD_MODE_SPECIAL);
        lv_label_set_text(chat_keyboard_mode_label, "ABC");
    }
}

void chat_keyboard_cb(lv_event_t * event)
{
    if(lv_event_get_code(event) == LV_EVENT_READY) {
        if(lv_keyboard_get_textarea(chat_keyboard) == chat_message_input) chat_send_message();
        else chat_set_nick_cb(nullptr);
        lv_keyboard_set_textarea(chat_keyboard, nullptr);
        lv_obj_add_flag(chat_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(chat_keyboard_mode_btn, LV_OBJ_FLAG_HIDDEN);
    }
    else if(lv_event_get_code(event) == LV_EVENT_CANCEL) {
        lv_keyboard_set_textarea(chat_keyboard, nullptr);
        lv_obj_add_flag(chat_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(chat_keyboard_mode_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

void show_chat_cb(lv_event_t *)
{
    lv_obj_add_flag(normal_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(video_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(chat_page, LV_OBJ_FLAG_HIDDEN);
}

void hide_chat_cb(lv_event_t *)
{
    lv_obj_add_flag(chat_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(chat_keyboard_mode_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(chat_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(normal_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(video_panel, LV_OBJ_FLAG_HIDDEN);
}

void build_chat_page(lv_obj_t * screen)
{
    const lv_font_t * chat_font = get_chat_font();
    chat_page = lv_obj_create(screen);
    lv_obj_set_size(chat_page, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(chat_page, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(chat_page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(chat_page, 0, 0);
    lv_obj_set_style_pad_all(chat_page, 0, 0);
    lv_obj_remove_flag(chat_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(chat_page, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * title = make_label(chat_page, "QO-100 WIDEBAND CHAT", COLOR_CYAN,
                                  &lv_font_montserrat_20);
    lv_obj_set_pos(title, 12, 12);
    chat_status_label = make_label(chat_page, "CONNECTING...", COLOR_YELLOW,
                                   chat_font);
    lv_obj_set_pos(chat_status_label, 330, 17);
    chat_viewers_label = make_label(chat_page, "VIEWERS: --", COLOR_TEXT_DIM,
                                    chat_font);
    lv_obj_align(chat_viewers_label, LV_ALIGN_TOP_RIGHT, -132, 17);

    lv_obj_t * back = lv_button_create(chat_page);
    lv_obj_set_pos(back, 910, 5);
    lv_obj_set_size(back, 106, 38);
    lv_obj_set_style_bg_color(back, COLOR_PANEL, 0);
    lv_obj_set_style_border_color(back, COLOR_CYAN, 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_t * back_label = make_label(back, "BACK", COLOR_CYAN, chat_font);
    lv_obj_center(back_label);
    lv_obj_add_event_cb(back, hide_chat_cb, LV_EVENT_CLICKED, nullptr);

    chat_keyboard_mode_btn = lv_button_create(chat_page);
    lv_obj_set_pos(chat_keyboard_mode_btn, 824, 5);
    lv_obj_set_size(chat_keyboard_mode_btn, 78, 38);
    lv_obj_set_style_bg_color(chat_keyboard_mode_btn, COLOR_PANEL, 0);
    lv_obj_set_style_border_color(chat_keyboard_mode_btn, COLOR_YELLOW, 0);
    lv_obj_set_style_border_width(chat_keyboard_mode_btn, 1, 0);
    lv_obj_set_style_shadow_width(chat_keyboard_mode_btn, 0, 0);
    chat_keyboard_mode_label = make_label(chat_keyboard_mode_btn, "SYM", COLOR_YELLOW,
                                          chat_font);
    lv_obj_center(chat_keyboard_mode_label);
    lv_obj_add_event_cb(chat_keyboard_mode_btn, chat_keyboard_mode_cb,
                        LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(chat_keyboard_mode_btn, LV_OBJ_FLAG_HIDDEN);

    chat_history_box = make_panel(chat_page, 8, 46, 790, 486);
    lv_obj_set_style_bg_color(chat_history_box, lv_color_hex(0x3f464c), 0);
    lv_obj_add_flag(chat_history_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(chat_history_box, LV_DIR_VER);
    chat_history_label = make_label(chat_history_box, "Waiting for chat history...", COLOR_TEXT,
                                    chat_font);
    lv_label_set_recolor(chat_history_label, true);
    lv_obj_set_width(chat_history_label, 760);
    lv_label_set_long_mode(chat_history_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(chat_history_label, 10, 8);

    lv_obj_t * users_box = make_panel(chat_page, 806, 46, 210, 486);
    lv_obj_set_style_bg_color(users_box, lv_color_hex(0x3f464c), 0);
    lv_obj_t * users_title = make_label(users_box, "ONLINE", COLOR_YELLOW,
                                        chat_font);
    lv_obj_set_pos(users_title, 10, 8);
    chat_users_label = make_label(users_box, "---", COLOR_TEXT,
                                  chat_font);
    lv_label_set_recolor(chat_users_label, true);
    lv_obj_set_pos(chat_users_label, 10, 34);

    chat_nick_input = lv_textarea_create(chat_page);
    lv_obj_set_pos(chat_nick_input, 8, 542);
    lv_obj_set_size(chat_nick_input, 150, 48);
    lv_obj_set_style_radius(chat_nick_input, 8, 0);
    lv_textarea_set_one_line(chat_nick_input, true);
    lv_textarea_set_max_length(chat_nick_input, 20);
    lv_textarea_set_placeholder_text(chat_nick_input, "CALLSIGN");
    lv_obj_set_style_text_font(chat_nick_input, chat_font, 0);
    lv_obj_add_event_cb(chat_nick_input, chat_textarea_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t * nick_button = lv_button_create(chat_page);
    lv_obj_set_pos(nick_button, 164, 542);
    lv_obj_set_size(nick_button, 76, 48);
    /* The default button theme adds an outside shadow, making a 52 px button
     * look taller than the adjacent 52 px text area. Keep the visible bounds
     * identical to the input fields. */
    lv_obj_set_style_shadow_width(nick_button, 0, 0);
    lv_obj_set_style_radius(nick_button, 8, 0);
    lv_obj_t * nick_label = make_label(nick_button, "SET", COLOR_TEXT);
    lv_obj_center(nick_label);
    lv_obj_add_event_cb(nick_button, chat_set_nick_cb, LV_EVENT_CLICKED, nullptr);

    chat_message_input = lv_textarea_create(chat_page);
    lv_obj_set_pos(chat_message_input, 248, 542);
    lv_obj_set_size(chat_message_input, 654, 48);
    lv_obj_set_style_radius(chat_message_input, 8, 0);
    lv_textarea_set_one_line(chat_message_input, true);
    lv_textarea_set_max_length(chat_message_input, 300);
    lv_textarea_set_placeholder_text(chat_message_input, "Type a message...");
    lv_obj_set_style_text_font(chat_message_input, chat_font, 0);
    lv_obj_add_event_cb(chat_message_input, chat_textarea_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t * send = lv_button_create(chat_page);
    lv_obj_set_pos(send, 910, 542);
    lv_obj_set_size(send, 106, 48);
    lv_obj_set_style_shadow_width(send, 0, 0);
    lv_obj_set_style_radius(send, 8, 0);
    lv_obj_set_style_bg_color(send, lv_color_hex(0x1b5663), 0);
    lv_obj_t * send_label = make_label(send, "SEND", COLOR_CYAN, chat_font);
    lv_obj_center(send_label);
    lv_obj_add_event_cb(send, chat_send_cb, LV_EVENT_CLICKED, nullptr);

    chat_keyboard = lv_keyboard_create(chat_page);
    lv_obj_set_size(chat_keyboard, SCREEN_W, 250);
    lv_obj_align(chat_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(chat_keyboard, chat_font, LV_PART_ITEMS);
    lv_obj_add_event_cb(chat_keyboard, chat_keyboard_cb, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(chat_keyboard, chat_keyboard_cb, LV_EVENT_CANCEL, nullptr);
    lv_obj_add_flag(chat_keyboard, LV_OBJ_FLAG_HIDDEN);
}

/* ===================================================================
 * Click-to-tune. Converts a touch X position on the spectrum canvas to a
 * QO-100 downlink frequency, shows a marker, and retunes Longmynd there.
 * =================================================================== */

/* Blanks the UI back to "---" the moment a retune is sent, rather than
 * leaving the OLD lock's data on screen until the monitor stream catches
 * up with the new state - Longmynd itself resets rx_state to searching
 * within its own status shortly after, this just avoids the visible lag. */
void reset_rx_status()
{
    rx_state = 0;
    rx_carrier_khz = 0;
    rx_symrate_ksps = 0;
    rx_mer_x10 = 0;
    rx_modcod = -1;
    rx_agc1 = 0;
    rx_agc2 = 0;
    rx_service_provider.clear();
    rx_service_name.clear();
    rx_ber_x100 = 0;
    rx_short_frames = -1;
    rx_pilots = -1;
    rx_ldpc_errors = 0;
    update_status_ui();
}

void show_tune_marker(double downlink_mhz)
{
    if(tune_marker == nullptr) return;

    constexpr int marker_label_clearance = 18;
    const double fraction = (downlink_mhz - START_FREQUENCY_MHZ) / SPAN_MHZ;
    const int marker_x = plot_x + static_cast<int>(fraction * (plot_w - 1));

    lv_obj_set_pos(tune_marker, marker_x, plot_y + marker_label_clearance);
    lv_obj_set_height(tune_marker, plot_h - marker_label_clearance);
    lv_obj_set_style_bg_color(tune_marker, COLOR_CYAN, 0);
    lv_obj_remove_flag(tune_marker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(tune_marker);

    char text[24];
    std::snprintf(text, sizeof(text), "%.3f", downlink_mhz);
    lv_label_set_text(tune_marker_label, text);
    lv_obj_set_style_text_color(tune_marker_label, COLOR_CYAN, 0);
    lv_obj_update_layout(tune_marker_label);
    const int label_w = lv_obj_get_width(tune_marker_label);
    const int label_x = std::clamp(marker_x - label_w / 2, 0, spectrum_panel_w - label_w);
    lv_obj_set_pos(tune_marker_label, label_x, plot_y - 2);
    lv_obj_remove_flag(tune_marker_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(tune_marker_label);
}

/* Click landed where there's no detected transmission - show a dim "no
 * signal" marker instead of the cyan tune marker, and (critically) don't
 * retune at all. We already have direct spectral evidence there's nothing
 * there; cycling Longmynd through candidate symbol rates on empty noise
 * floor would just waste several seconds for no reason. */
void show_no_signal_marker(double downlink_mhz)
{
    if(tune_marker == nullptr) return;

    const double fraction = (downlink_mhz - START_FREQUENCY_MHZ) / SPAN_MHZ;
    const int marker_x = plot_x + static_cast<int>(fraction * (plot_w - 1));

    lv_obj_set_pos(tune_marker, marker_x, plot_y);
    lv_obj_set_style_bg_color(tune_marker, COLOR_TEXT_DIM, 0);
    lv_obj_remove_flag(tune_marker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(tune_marker);

    lv_label_set_text(tune_marker_label, "no signal");
    lv_obj_set_style_text_color(tune_marker_label, COLOR_TEXT_DIM, 0);
    lv_obj_update_layout(tune_marker_label);
    const int label_w = lv_obj_get_width(tune_marker_label);
    const int label_x = std::clamp(marker_x - label_w / 2, 0, spectrum_panel_w - label_w);
    lv_obj_set_pos(tune_marker_label, label_x, plot_y - 2);
    lv_obj_remove_flag(tune_marker_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(tune_marker_label);
}

/* Retunes over the control WebSocket - Longmynd's receiver thread picks
 * up the new frequency/symbol rate live (see longmynd_ws/main.c's
 * config_set_frequency_and_symbolrate()), no process restart, no video
 * gap. Both freq_khz and symrate_ksps use the same units Longmynd's own
 * CLI args do (confirmed against its "Config cycle" log line). */
void retune_exact(double downlink_mhz, long symrate_ksps)
{
    std::fprintf(stderr, "[TUNE] retune_exact(downlink_mhz=%.3f, symrate=%ld kS/s)\n",
                 downlink_mhz, symrate_ksps);

    const auto now = std::chrono::steady_clock::now();
    if(now - last_retune_time < RETUNE_MIN_INTERVAL) {
        std::fprintf(stderr, "[TUNE] ignored - within debounce window (%lld ms since last)\n",
                     static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                         now - last_retune_time).count()));
        return;
    }
    last_retune_time = now;

    const long if_khz = std::lround((downlink_mhz - g_lnb_lo_mhz) * 1000.0);
    std::fprintf(stderr, "[TUNE] computed IF = %ld kHz (LNB LO = %.1f MHz)\n", if_khz, g_lnb_lo_mhz);
    if(if_khz <= 0) {
        std::fprintf(stderr, "[TUNE] ignored - non-positive IF frequency\n");
        return;
    }

    show_tune_marker(downlink_mhz);
    reset_rx_status();
    tuned_to_beacon = false;
    auto_beacon_return_armed = false;

    char cmd[48];
    std::snprintf(cmd, sizeof(cmd), "C%ld,%ld", if_khz, symrate_ksps);
    send_control_command(cmd);

    tuning_pending = true;
    watchdog_timeout_logged = false;
    if(tuning_overlay != nullptr) {
        lv_obj_remove_flag(tuning_overlay, LV_OBJ_FLAG_HIDDEN);
        /* +1: any TS bytes already past the tuner and sitting in the ring
         * buffer right now still belong to the *old* stream, so don't clear
         * on the decode session already in flight - only on the next one,
         * which can only start once decode_loop has torn that down and
         * reopened for whatever the tuner is on after this retune. */
        tuning_overlay_target_session = decode_session_count.load(std::memory_order_relaxed) + 1;
    }
    std::fprintf(stderr, "[TUNE] retune command queued - waiting for lock\n");
}

/* Fires once AUTO_BEACON_RETURN_DELAY has passed with no lock regained after
 * a non-beacon signal disappeared (armed in finish_rx_status_update()).
 * BEACON_FREQ_KHZ is already an IF, unlike retune_exact()'s downlink-MHz
 * input, so this skips the LNB LO conversion and goes straight to the same
 * debounce + control-command + tuning_pending/overlay bookkeeping. */
void return_to_beacon()
{
    const auto now = std::chrono::steady_clock::now();
    if(now - last_retune_time < RETUNE_MIN_INTERVAL) return;
    last_retune_time = now;

    std::fprintf(stderr, "[TUNE] auto-returning to beacon (TX stopped)\n");
    reset_rx_status();
    tuned_to_beacon = true;

    char cmd[48];
    std::snprintf(cmd, sizeof(cmd), "C%ld,%ld", BEACON_FREQ_KHZ, BEACON_SYMRATE_KSPS);
    send_control_command(cmd);

    tuning_pending = true;
    watchdog_timeout_logged = false;
    if(tuning_overlay != nullptr) {
        lv_obj_remove_flag(tuning_overlay, LV_OBJ_FLAG_HIDDEN);
        tuning_overlay_target_session = decode_session_count.load(std::memory_order_relaxed) + 1;
    }
}

void spectrum_click_cb(lv_event_t *)
{
    std::fprintf(stderr, "[CLICK] spectrum_click_cb fired\n");

    lv_indev_t * indev = lv_indev_get_act();
    if(indev == nullptr) {
        std::fprintf(stderr, "[CLICK] lv_indev_get_act() returned null - ignoring\n");
        return;
    }

    lv_point_t point;
    lv_indev_get_point(indev, &point);

    lv_area_t area;
    lv_obj_get_coords(spectrum_canvas, &area);
    const int rel_x = point.x - area.x1;

    std::fprintf(stderr, "[CLICK] touch point=(%d,%d) canvas area=(%d,%d)-(%d,%d) rel_x=%d plot_w=%d\n",
                 point.x, point.y, area.x1, area.y1, area.x2, area.y2, rel_x, plot_w);

    const double fraction = std::clamp(static_cast<double>(rel_x) / plot_w, 0.0, 1.0);
    const double downlink_mhz = START_FREQUENCY_MHZ + fraction * SPAN_MHZ;

    std::fprintf(stderr, "[CLICK] fraction=%.4f -> downlink=%.3f MHz\n", fraction, downlink_mhz);

    const DetectedSignal * hit = nullptr;
    size_t clicked_bin = 0;
    if(!last_bins.empty()) {
        clicked_bin = static_cast<size_t>(fraction * (last_bins.size() - 1));
        const float clicked_displayed_db =
            last_bins[clicked_bin] / SERVER_UNITS_PER_DB - DISPLAY_ZERO_OFFSET_DB;
        std::fprintf(stderr, "[CLICK] bin %zu/%zu raw=%u displayed=%.2f dB\n",
                     clicked_bin, last_bins.size(), last_bins[clicked_bin], clicked_displayed_db);

        for(const DetectedSignal & signal : last_detected_signals) {
            if(clicked_bin >= signal.start_bin && clicked_bin <= signal.end_bin) {
                hit = &signal;
                break;
            }
        }
    }
    else {
        std::fprintf(stderr, "[CLICK] no spectrum data received yet - can't check signal level\n");
    }

    if(hit != nullptr) {
        std::fprintf(stderr,
            "[CLICK] snapped to detected signal: center=%.3f MHz width=%.3f MHz "
            "estimated SR=%.0f kS/s strength=%.0f (bins %zu-%zu)\n",
            hit->frequency_mhz, hit->measured_width_mhz,
            hit->symbol_rate_ms * 1000.0F, hit->strength, hit->start_bin, hit->end_bin);
        retune_exact(hit->frequency_mhz, std::lround(hit->symbol_rate_ms * 1000.0F));
    }
    else {
        std::fprintf(stderr,
            "[CLICK] no detected signal at bin %zu - not tuning (empty noise floor)\n",
            clicked_bin);
        show_no_signal_marker(downlink_mhz);
    }
}

/* Shared by restart_btn_cb() and the SIGTERM/SIGINT handling below. Do not
 * call exit() from either path: its atexit handler joins the libwebsockets
 * workers, and libwebsockets can remain blocked in TLS/context teardown for
 * an unbounded time. Stop our child receiver first and then let the OS
 * atomically tear down this process and all of its threads/sockets. */
void safe_shutdown(int exit_code)
{
    stop_longmynd();
    std::_Exit(exit_code);
}

/* Fullscreen has no window chrome, so this is the only in-app way to
 * recover from a stuck tuner/decoder without physically power-cycling the
 * Pi. The app runs under qo100datv.service (see scripts/setup_autostart.sh)
 * with Restart=on-failure, RestartSec=3 - so exiting with a non-zero status
 * is enough to make systemd relaunch it; no self-exec/fork needed. */
void restart_btn_cb(lv_event_t *)
{
    std::fprintf(stderr, "[CLICK] restart_btn_cb fired\n");
    safe_shutdown(1);
}

/* SDL installs its own SIGTERM/SIGINT handlers (see SDL_QuitInit()) that
 * turn the signal into an SDL_QUIT event instead of terminating the process.
 * LVGL's SDL driver reacts to that event with a plain exit() when
 * LV_SDL_DIRECT_EXIT is set (see lv_sdl_window.c) - the exact hang-prone path
 * safe_shutdown() above avoids. `systemctl stop`/`restart` send SIGTERM, so
 * left alone, every stop can hang for the full TimeoutStopSec and end in a
 * SIGKILL instead of a clean exit. Override SDL's handler (installed after
 * driver_backends_init_backend() in main() so ours wins) and just flag it;
 * signal handlers must stick to async-signal-safe calls, so the actual
 * shutdown happens from termination_signal_check_cb() on the next timer
 * tick, in normal thread context. */
std::atomic<bool> g_termination_signal_received{false};

extern "C" void handle_termination_signal(int)
{
    g_termination_signal_received.store(true, std::memory_order_relaxed);
}

void termination_signal_check_cb(lv_timer_t *)
{
    if(g_termination_signal_received.load(std::memory_order_relaxed))
        safe_shutdown(0);
}

/* Temporary: fullscreen also ate the ctrl+shift+p screenshot shortcut, so
 * this is the only way left to grab and share what's on screen. Same `grim`
 * tool and output path as scripts/screenshot.sh, just triggered from inside
 * the app instead of a keybinding. */
void screenshot_btn_cb(lv_event_t *)
{
    std::fprintf(stderr, "[CLICK] screenshot_btn_cb fired\n");
    system("grim /home/daniel/DATVreceiver/screenshots/latest.png");
}

/* ================================================================= */

void build_spectrum_panel(lv_obj_t * screen)
{
    constexpr int panel_x = 4, panel_y = 4, panel_w = SCREEN_W - 8, panel_h = SPECTRUM_PANEL_H;
    lv_obj_t * panel = make_panel(screen, panel_x, panel_y, panel_w, panel_h);

    /* Top/left/right margins all equal (CONTENT_MARGIN); no dB scale labels
     * on the left anymore, so the left margin no longer needs extra room. */
    plot_x = CONTENT_MARGIN;
    plot_y = CONTENT_MARGIN;
    plot_w = panel_w - 2 * CONTENT_MARGIN;
    /* 36px is the fixed chrome above/below the plot (axis label rows) -
     * everything else goes to the plot itself. */
    plot_h = panel_h - 36;
    spectrum_panel_w = panel_w;

    for(int number = 1; number <= 9; ++number) {
        char text[16];
        std::snprintf(text, sizeof(text), "%d", 10490 + number);
        lv_obj_t * label = make_label(panel, text, COLOR_TEXT_DIM);
        const double fraction = static_cast<double>(number) / 9.0;
        const int x = plot_x + static_cast<int>(fraction * (plot_w - 1)) - 12;
        lv_obj_set_pos(label, std::clamp(x, 0, panel_w - 46), plot_y + plot_h + 4);
    }

    spectrum_pixels = static_cast<uint16_t *>(lv_malloc(plot_w * plot_h * sizeof(uint16_t)));
    std::fill(spectrum_pixels, spectrum_pixels + plot_w * plot_h, rgb565(0, 0, 0));

    spectrum_canvas = lv_canvas_create(panel);
    lv_canvas_set_buffer(spectrum_canvas, spectrum_pixels, plot_w, plot_h, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(spectrum_canvas, plot_x, plot_y);

    spectrum_status_label = make_label(panel, "CONNECTING...", lv_color_hex(0xFFD166));
    lv_obj_set_pos(spectrum_status_label, plot_x + 8, plot_y + 8);
    lv_obj_move_foreground(spectrum_status_label);

    tune_marker = lv_obj_create(panel);
    lv_obj_set_size(tune_marker, 2, plot_h);
    lv_obj_set_style_bg_color(tune_marker, COLOR_CYAN, 0);
    lv_obj_set_style_bg_opa(tune_marker, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tune_marker, 0, 0);
    lv_obj_set_style_radius(tune_marker, 0, 0);
    lv_obj_add_flag(tune_marker, LV_OBJ_FLAG_HIDDEN);

    tune_marker_label = make_label(panel, "", COLOR_CYAN);
    lv_obj_add_flag(tune_marker_label, LV_OBJ_FLAG_HIDDEN);

    for(size_t index = 0; index < MAX_SIGNAL_LABELS; ++index) {
        signal_labels[index] = make_label(panel, "", lv_color_white(), &lv_font_montserrat_14);
        lv_obj_set_width(signal_labels[index], 145);
        lv_obj_set_style_text_align(signal_labels[index], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_add_flag(signal_labels[index], LV_OBJ_FLAG_HIDDEN);

        callsign_labels[index] = make_label(panel, "", COLOR_GREEN, &lv_font_montserrat_14);
        lv_obj_set_width(callsign_labels[index], 145);
        lv_label_set_long_mode(callsign_labels[index], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(callsign_labels[index], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_add_flag(callsign_labels[index], LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_add_flag(spectrum_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(spectrum_canvas, spectrum_click_cb, LV_EVENT_CLICKED, nullptr);
}

constexpr int VIDEO_PANEL_H = BOTTOM_ROW_H;
/* Video canvas target size in the small (normal) view: left/bottom margins
 * match the spectrum plot's (CONTENT_MARGIN); top/right stay the original
 * 1px inset, just enough to clear the panel border. Fullscreen mode (see
 * video_panel_click_cb) targets SCREEN_W x SCREEN_H directly instead. */
constexpr int VIDEO_SMALL_W = VIDEO_PANEL_W - CONTENT_MARGIN - 1;
constexpr int VIDEO_SMALL_H = VIDEO_PANEL_H - 1 - CONTENT_MARGIN;

/* ===================================================================
 * Video decode pipeline - UDP TS in, RGB565 frames out, fully decoupled
 * from the UI thread (see the big comment above start_longmynd() for
 * why: LVGL's lv_ffmpeg widget blocking on a FIFO read froze the whole
 * app, not just the video). Three independent pieces:
 *   1. UDP receiver thread: recv() -> ring buffer. Never touches FFmpeg.
 *   2. Decode thread: ring buffer -> custom AVIOContext -> demux/decode
 *      -> sws_scale, letterboxed, into a shared RGB565 frame buffer.
 *   3. UI thread: a lightweight lv_timer copies the latest ready frame
 *      into an lv_canvas and invalidates it - never touches the network
 *      or FFmpeg, so it can never block.
 * request_video_reset() (called on the lock-regained edge in
 * parse_monitor_json()) just sets a flag the decode thread polls; it
 * tears down and reopens its own AVFormatContext without needing to
 * touch the other two pieces, matching OpenTuner's lock-driven reset.
 * =================================================================== */

class TsRingBuffer {
public:
    /* Whole datagram or nothing. A UDP datagram here is a burst of whole
     * 188-byte TS packets - writing only the part that fits (the old
     * behaviour) would splice the tail of one packet straight into an
     * unrelated one, desyncing the demuxer far worse than just dropping
     * this one late/oversized burst and letting it resync on the next. */
    void push(const uint8_t * data, size_t len)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(len > CAPACITY - size_) {
            ++overflow_count_;
            std::fprintf(stderr, "[TS] ring buffer full, dropping datagram (#%zu)\n", overflow_count_);
            return;
        }
        for(size_t i = 0; i < len; ++i)
            buffer_[(tail_ + size_ + i) % CAPACITY] = data[i];
        size_ += len;
        cv_.notify_one();
    }

    /* Blocks until data is available or wake() is called; returns 0 if
     * woken with nothing buffered (avio_read_cb turns that into an EOF that
     * unsticks a decode session waiting on TS bytes that stopped arriving -
     * see request_video_reset()). */
    size_t pop(uint8_t * out, size_t max_len)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return size_ > 0 || woken_; });
        woken_ = false;
        if(size_ == 0) return 0;
        const size_t n = std::min(max_len, size_);
        for(size_t i = 0; i < n; ++i) out[i] = buffer_[(tail_ + i) % CAPACITY];
        tail_ = (tail_ + n) % CAPACITY;
        size_ -= n;
        return n;
    }

    /* Unblocks a pending pop() without necessarily meaning "no more data
     * ever" - just "stop waiting and check in with the caller now". */
    void wake()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        woken_ = true;
        cv_.notify_one();
    }

    /* Drops whatever's buffered - called after a decoder reset so stale
     * bytes from the previous service don't get fed to the new demux. */
    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tail_ = 0;
        size_ = 0;
    }

private:
    static constexpr size_t CAPACITY = 4 * 1024 * 1024;
    std::vector<uint8_t> buffer_ = std::vector<uint8_t>(CAPACITY);
    size_t tail_ = 0;
    size_t size_ = 0;
    size_t overflow_count_ = 0;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool woken_ = false;
};

TsRingBuffer g_ts_ring;

int udp_sock = -1;
std::thread udp_thread;
std::atomic<bool> udp_thread_running{false};

void udp_receiver_loop()
{
    uint8_t buf[2048];
    while(udp_thread_running.load(std::memory_order_relaxed)) {
        /* SO_RCVTIMEO below bounds this so the loop periodically rechecks
         * udp_thread_running instead of blocking forever - not that we
         * currently stop this thread, but cheap insurance either way. */
        const ssize_t n = recv(udp_sock, buf, sizeof(buf), 0);
        if(n > 0) g_ts_ring.push(buf, static_cast<size_t>(n));
    }
}

void start_udp_receiver()
{
    udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if(udp_sock < 0) {
        std::fprintf(stderr, "[UDP] socket() failed: %s\n", std::strerror(errno));
        return;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(LONGMYND_TS_UDP_PORT);

    /* A restart (see restart_btn_cb()) can occasionally overlap the tail end
     * of the previous process still releasing this socket. Retry briefly
     * instead of giving up outright - failing here silently kills video/audio
     * for the entire session with no recovery until the next restart. */
    constexpr int BIND_RETRY_ATTEMPTS = 10;
    constexpr int BIND_RETRY_DELAY_MS = 200;
    int bind_result = -1;
    for(int attempt = 1; attempt <= BIND_RETRY_ATTEMPTS; ++attempt) {
        bind_result = bind(udp_sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
        if(bind_result == 0) break;
        if(attempt < BIND_RETRY_ATTEMPTS)
            std::this_thread::sleep_for(std::chrono::milliseconds(BIND_RETRY_DELAY_MS));
    }
    if(bind_result < 0) {
        /* No point starting the receiver thread - it would spin on a socket
         * that's never going to see the TS traffic it was meant to catch. */
        std::fprintf(stderr, "[UDP] bind() to port %d failed after %d attempts: %s\n",
                     LONGMYND_TS_UDP_PORT, BIND_RETRY_ATTEMPTS, std::strerror(errno));
        close(udp_sock);
        udp_sock = -1;
        return;
    }

    timeval recv_timeout{1, 0};
    if(setsockopt(udp_sock, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout)) < 0) {
        /* Non-fatal - the receiver loop just won't periodically recheck
         * udp_thread_running and instead blocks in recv() indefinitely. */
        std::fprintf(stderr, "[UDP] setsockopt(SO_RCVTIMEO) failed: %s\n", std::strerror(errno));
    }

    udp_thread_running = true;
    udp_thread = std::thread(udp_receiver_loop);
}

int avio_read_cb(void * opaque, uint8_t * buf, int buf_size)
{
    auto * ring = static_cast<TsRingBuffer *>(opaque);
    const size_t n = ring->pop(buf, static_cast<size_t>(buf_size));
    return n > 0 ? static_cast<int>(n) : AVERROR_EOF;
}

std::thread decode_thread;
std::atomic<bool> decode_thread_running{false};
std::atomic<bool> decode_reset_requested{false};

/* Toggled by video_panel_click_cb (tap to fill the screen, tap again to go
 * back). decode_loop() below re-reads this every frame and rescales into
 * whichever target size is currently selected. */
std::atomic<bool> video_fullscreen{false};

std::mutex frame_mutex;
std::vector<uint16_t> frame_pixels = std::vector<uint16_t>(VIDEO_SMALL_W * VIDEO_SMALL_H, 0);
std::atomic<bool> frame_ready{false};

constexpr int AUDIO_OUTPUT_RATE = 48000;
constexpr int AUDIO_OUTPUT_CHANNELS = 2;
SDL_AudioDeviceID audio_device = 0;
std::atomic<int> audio_peak_percent{0};
lv_obj_t * audio_vu_bar = nullptr;
lv_obj_t * audio_volume_label = nullptr;

void volume_slider_cb(lv_event_t * event)
{
    lv_obj_t * slider = static_cast<lv_obj_t *>(lv_event_get_target(event));
    const int volume = static_cast<int>(lv_slider_get_value(slider));
    audio_volume_percent.store(volume, std::memory_order_relaxed);
    if(audio_volume_label != nullptr) {
        char text[12];
        std::snprintf(text, sizeof(text), "%d%%", volume);
        lv_label_set_text(audio_volume_label, text);
    }
    save_settings();
}

void audio_vu_update_cb(lv_timer_t *)
{
    static int displayed_level = 0;
    const int measured = audio_peak_percent.exchange(0, std::memory_order_relaxed);
    displayed_level = std::max(measured, displayed_level - 5);
    if(audio_vu_bar == nullptr) return;
    lv_bar_set_value(audio_vu_bar, displayed_level, LV_ANIM_OFF);
    const lv_color_t colour = displayed_level >= 90 ? COLOR_RED :
                              displayed_level >= 70 ? COLOR_YELLOW : COLOR_GREEN;
    lv_obj_set_style_bg_color(audio_vu_bar, colour, LV_PART_INDICATOR);
}

void clear_audio_queue()
{
    if(audio_device != 0) SDL_ClearQueuedAudio(audio_device);
}

void start_audio_output()
{
    if(SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        std::fprintf(stderr, "[AUDIO] SDL audio init failed: %s\n", SDL_GetError());
        return;
    }

    SDL_AudioSpec desired{};
    desired.freq = AUDIO_OUTPUT_RATE;
    desired.format = AUDIO_S16SYS;
    desired.channels = AUDIO_OUTPUT_CHANNELS;
    desired.samples = 1024;
    audio_device = SDL_OpenAudioDevice(nullptr, 0, &desired, nullptr, 0);
    if(audio_device == 0) {
        std::fprintf(stderr, "[AUDIO] could not open output: %s\n", SDL_GetError());
        return;
    }
    SDL_PauseAudioDevice(audio_device, 0);
    std::fprintf(stderr, "[AUDIO] output opened: 48 kHz stereo\n");
}

/* Codec names are read straight from FFmpeg's own demux/decode state -
 * simpler and more authoritative than re-deriving them from Longmynd's
 * raw ES-type numeric codes (the old approach, removed earlier and not
 * worth resurrecting now that we already have an open AVFormatContext
 * anyway). The audio name only needs stream probing, not an open codec
 * context - we never decode/play audio, see the "no audio" discussion. */
std::mutex codec_name_mutex;
std::string video_codec_name;
std::string audio_codec_name;

std::string uppercase_codec_name(AVCodecID id)
{
    std::string name = avcodec_get_name(id);
    for(char & c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return name;
}

void decode_loop()
{
    while(decode_thread_running.load(std::memory_order_relaxed)) {
        decode_session_count.fetch_add(1, std::memory_order_relaxed);
        {
            /* Clear stale names from the previous service immediately -
             * otherwise the old codec name would linger on screen while
             * Longmynd is still searching for the new lock. */
            std::lock_guard<std::mutex> lock(codec_name_mutex);
            video_codec_name.clear();
            audio_codec_name.clear();
        }

        constexpr int AVIO_BUF_SIZE = 4096;
        auto * avio_buf = static_cast<uint8_t *>(av_malloc(AVIO_BUF_SIZE));
        if(avio_buf == nullptr) {
            std::fprintf(stderr, "[DECODE] av_malloc failed, retrying\n");
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            continue;
        }

        AVIOContext * avio_ctx = avio_alloc_context(avio_buf, AVIO_BUF_SIZE, 0, &g_ts_ring,
                                                     avio_read_cb, nullptr, nullptr);
        if(avio_ctx == nullptr) {
            std::fprintf(stderr, "[DECODE] avio_alloc_context failed, retrying\n");
            av_freep(&avio_buf);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            continue;
        }

        AVFormatContext * fmt_ctx = avformat_alloc_context();
        if(fmt_ctx == nullptr) {
            std::fprintf(stderr, "[DECODE] avformat_alloc_context failed, retrying\n");
            av_freep(&avio_ctx->buffer);
            avio_context_free(&avio_ctx);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            continue;
        }
        fmt_ctx->pb = avio_ctx;
        fmt_ctx->flags |= AVFMT_FLAG_CUSTOM_IO;

        AVDictionary * open_opts = nullptr;
        av_dict_set(&open_opts, "probesize", "131072", 0);
        const int open_result = avformat_open_input(&fmt_ctx, nullptr, nullptr, &open_opts);
        av_dict_free(&open_opts);

        int video_stream_index = -1;
        int audio_stream_index = -1;
        AVCodecContext * codec_ctx = nullptr;
        AVCodecContext * audio_codec_ctx = nullptr;

        if(open_result >= 0 && avformat_find_stream_info(fmt_ctx, nullptr) >= 0) {
            video_stream_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
            audio_stream_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

            std::lock_guard<std::mutex> lock(codec_name_mutex);
            if(video_stream_index >= 0)
                video_codec_name = uppercase_codec_name(fmt_ctx->streams[video_stream_index]->codecpar->codec_id);
            if(audio_stream_index >= 0)
                audio_codec_name = uppercase_codec_name(fmt_ctx->streams[audio_stream_index]->codecpar->codec_id);
        }

        if(video_stream_index >= 0) {
            /* Diagnostic: tells us whether a "choppy" picture is a slow
             * source encode (e.g. the QO-100 beacon's mostly-static ID
             * card, often sent at a genuinely low frame rate) or an actual
             * decode/render problem here. */
            const AVRational fr = fmt_ctx->streams[video_stream_index]->r_frame_rate;
            std::fprintf(stderr, "[DECODE] source video frame rate: %d/%d (%.2f fps)\n",
                         fr.num, fr.den, fr.den != 0 ? static_cast<double>(fr.num) / fr.den : 0.0);
        }

        if(video_stream_index >= 0) {
            AVCodecParameters * params = fmt_ctx->streams[video_stream_index]->codecpar;
            const AVCodec * codec = avcodec_find_decoder(params->codec_id);
            if(codec != nullptr) {
                codec_ctx = avcodec_alloc_context3(codec);
                avcodec_parameters_to_context(codec_ctx, params);
                if(avcodec_open2(codec_ctx, codec, nullptr) < 0) {
                    avcodec_free_context(&codec_ctx);
                    codec_ctx = nullptr;
                }
            }
        }

        if(audio_stream_index >= 0) {
            AVCodecParameters * params = fmt_ctx->streams[audio_stream_index]->codecpar;
            const AVCodec * codec = avcodec_find_decoder(params->codec_id);
            if(codec != nullptr) {
                audio_codec_ctx = avcodec_alloc_context3(codec);
                if(audio_codec_ctx == nullptr ||
                   avcodec_parameters_to_context(audio_codec_ctx, params) < 0 ||
                   avcodec_open2(audio_codec_ctx, codec, nullptr) < 0) {
                    avcodec_free_context(&audio_codec_ctx);
                }
            }
        }

        if(codec_ctx != nullptr || audio_codec_ctx != nullptr) {
            AVPacket * packet = av_packet_alloc();
            AVFrame * frame = av_frame_alloc();
            AVFrame * audio_frame = av_frame_alloc();
            SwrContext * swr_ctx = nullptr;
            AVChannelLayout stereo_layout{};
            av_channel_layout_default(&stereo_layout, AUDIO_OUTPUT_CHANNELS);
            if(audio_codec_ctx != nullptr && audio_codec_ctx->sample_rate > 0 &&
               audio_codec_ctx->ch_layout.nb_channels > 0 &&
               swr_alloc_set_opts2(&swr_ctx, &stereo_layout, AV_SAMPLE_FMT_S16,
                                   AUDIO_OUTPUT_RATE, &audio_codec_ctx->ch_layout,
                                   audio_codec_ctx->sample_fmt, audio_codec_ctx->sample_rate,
                                   0, nullptr) >= 0) {
                if(swr_init(swr_ctx) < 0) swr_free(&swr_ctx);
            }
            clear_audio_queue();
            SwsContext * sws_ctx = nullptr;
            int sws_src_w = 0, sws_src_h = 0;
            int dst_w = 0, dst_h = 0, dst_x = 0, dst_y = 0;
            /* Size frame_pixels/scratch are currently allocated for - tracked
             * separately from VIDEO_SMALL_W/H since it also needs to follow
             * video_fullscreen toggling to SCREEN_W/H and back. */
            int frame_w = 0, frame_h = 0;
            /* sws_scale() writes here, *not* into frame_pixels directly -
             * scaling takes a few ms, and holding frame_mutex for that whole
             * stretch 50x/second was enough to starve the UI thread out of
             * ever winning the lock (observed: video canvas updates dropped
             * to ~1 every several seconds instead of the intended ~30/s).
             * frame_mutex is now only held for the final memcpy below. */
            std::vector<uint16_t> scratch;

            while(decode_thread_running.load(std::memory_order_relaxed) &&
                  !decode_reset_requested.load(std::memory_order_relaxed)) {
                if(av_read_frame(fmt_ctx, packet) < 0) break;

                if(codec_ctx != nullptr && packet->stream_index == video_stream_index &&
                   avcodec_send_packet(codec_ctx, packet) == 0) {
                    while(avcodec_receive_frame(codec_ctx, frame) == 0) {
                        const bool fullscreen = video_fullscreen.load(std::memory_order_relaxed);
                        const int target_w = fullscreen ? SCREEN_W : VIDEO_SMALL_W;
                        const int target_h = fullscreen ? SCREEN_H : VIDEO_SMALL_H;

                        if(sws_ctx == nullptr || sws_src_w != frame->width || sws_src_h != frame->height ||
                           frame_w != target_w || frame_h != target_h) {
                            if(sws_ctx != nullptr) sws_freeContext(sws_ctx);
                            sws_src_w = frame->width;
                            sws_src_h = frame->height;
                            frame_w = target_w;
                            frame_h = target_h;

                            /* Letterbox: scale to fit frame_w x frame_h,
                             * preserving aspect ratio (matches the old
                             * LV_IMAGE_ALIGN_CONTAIN behaviour). Target size
                             * switches between the small panel and full
                             * screen when video_panel_click_cb toggles
                             * video_fullscreen; video_frame_update_cb skips
                             * copying out until this resize below has caught
                             * up with whatever size the canvas was just set
                             * to, so there's no size mismatch either way. */
                            const double scale = std::min(
                                static_cast<double>(frame_w) / sws_src_w,
                                static_cast<double>(frame_h) / sws_src_h);
                            dst_w = std::max(1, static_cast<int>(sws_src_w * scale));
                            dst_h = std::max(1, static_cast<int>(sws_src_h * scale));
                            dst_x = (frame_w - dst_w) / 2;
                            dst_y = (frame_h - dst_h) / 2;

                            sws_ctx = sws_getContext(sws_src_w, sws_src_h,
                                                     static_cast<AVPixelFormat>(frame->format),
                                                     dst_w, dst_h, AV_PIX_FMT_RGB565,
                                                     SWS_BILINEAR, nullptr, nullptr, nullptr);

                            scratch.assign(static_cast<size_t>(frame_w) * frame_h, 0);
                            std::lock_guard<std::mutex> lock(frame_mutex);
                            frame_pixels.assign(static_cast<size_t>(frame_w) * frame_h, 0);
                        }

                        if(sws_ctx != nullptr) {
                            std::fill(scratch.begin(), scratch.end(), 0);
                            uint8_t * dst_planes[1] = {
                                reinterpret_cast<uint8_t *>(scratch.data() + dst_y * frame_w + dst_x) };
                            int dst_linesize[1] = { frame_w * 2 };
                            sws_scale(sws_ctx, frame->data, frame->linesize, 0, sws_src_h,
                                     dst_planes, dst_linesize);

                            {
                                std::lock_guard<std::mutex> lock(frame_mutex);
                                frame_pixels = scratch;
                                frame_ready.store(true, std::memory_order_release);
                            }

                            /* Diagnostic: gap between successive *produced*
                             * frames, mirroring the consumer-side one in
                             * video_frame_update_cb - tells apart "decode
                             * itself stalls periodically" from "frames are
                             * produced smoothly but something delays handing
                             * them to the UI". */
                            static auto last_produced = std::chrono::steady_clock::now();
                            const auto produced_now = std::chrono::steady_clock::now();
                            const double produced_gap_ms =
                                std::chrono::duration<double, std::milli>(produced_now - last_produced).count();
                            if(produced_gap_ms > 100.0)
                                std::fprintf(stderr, "[DECODE] stutter: %.1fms since previous frame produced\n",
                                             produced_gap_ms);
                            last_produced = produced_now;

                            /* Delivered rate, to compare against the source
                             * rate logged above - if they roughly match, a
                             * "choppy" look is the source content, not
                             * decode/render falling behind. */
                            static auto last_fps_log = std::chrono::steady_clock::now();
                            static int frames_since_log = 0;
                            ++frames_since_log;
                            const auto fps_now = std::chrono::steady_clock::now();
                            const double fps_elapsed =
                                std::chrono::duration<double>(fps_now - last_fps_log).count();
                            if(fps_elapsed >= 5.0) {
                                std::fprintf(stderr, "[DECODE] delivered %.2f fps over last %.1fs (%d frames)\n",
                                             frames_since_log / fps_elapsed, fps_elapsed, frames_since_log);
                                frames_since_log = 0;
                                last_fps_log = fps_now;
                            }
                        }
                    }
                }
                else if(audio_codec_ctx != nullptr && swr_ctx != nullptr &&
                        packet->stream_index == audio_stream_index &&
                        avcodec_send_packet(audio_codec_ctx, packet) == 0) {
                    while(avcodec_receive_frame(audio_codec_ctx, audio_frame) == 0) {
                        const int max_samples = swr_get_out_samples(swr_ctx, audio_frame->nb_samples);
                        if(max_samples <= 0) continue;

                        std::vector<uint8_t> pcm(static_cast<size_t>(max_samples) *
                                                 AUDIO_OUTPUT_CHANNELS * sizeof(int16_t));
                        uint8_t * output[] = { pcm.data() };
                        const int samples = swr_convert(
                            swr_ctx, output, max_samples,
                            const_cast<const uint8_t **>(audio_frame->extended_data),
                            audio_frame->nb_samples);
                        if(samples > 0 && audio_device != 0) {
                            const size_t bytes = static_cast<size_t>(samples) *
                                                 AUDIO_OUTPUT_CHANNELS * sizeof(int16_t);
                            auto * pcm_samples = reinterpret_cast<int16_t *>(pcm.data());
                            const size_t sample_count = static_cast<size_t>(samples) *
                                                        AUDIO_OUTPUT_CHANNELS;
                            const int volume = audio_volume_percent.load(std::memory_order_relaxed);
                            int peak = 0;
                            for(size_t i = 0; i < sample_count; ++i) {
                                const int scaled = static_cast<int>(pcm_samples[i]) * volume / 100;
                                pcm_samples[i] = static_cast<int16_t>(scaled);
                                peak = std::max(peak, std::abs(scaled));
                            }
                            const int peak_percent = std::min(100, peak * 100 / 32767);
                            int previous_peak = audio_peak_percent.load(std::memory_order_relaxed);
                            while(previous_peak < peak_percent &&
                                  !audio_peak_percent.compare_exchange_weak(
                                      previous_peak, peak_percent, std::memory_order_relaxed)) {}

                            /* Avoid building seconds of latency if FFmpeg
                             * processes a burst of buffered TS packets. */
                            constexpr Uint32 MAX_QUEUED_AUDIO =
                                AUDIO_OUTPUT_RATE * AUDIO_OUTPUT_CHANNELS * sizeof(int16_t) / 2;
                            if(SDL_GetQueuedAudioSize(audio_device) < MAX_QUEUED_AUDIO &&
                               SDL_QueueAudio(audio_device, pcm.data(), static_cast<Uint32>(bytes)) < 0) {
                                std::fprintf(stderr, "[AUDIO] queue failed: %s\n", SDL_GetError());
                            }
                        }
                    }
                }
                av_packet_unref(packet);
            }

            if(sws_ctx != nullptr) sws_freeContext(sws_ctx);
            swr_free(&swr_ctx);
            av_channel_layout_uninit(&stereo_layout);
            av_frame_free(&audio_frame);
            av_frame_free(&frame);
            av_packet_free(&packet);
            avcodec_free_context(&audio_codec_ctx);
            avcodec_free_context(&codec_ctx);
        }
        else {
            /* Nothing decodable yet (still searching, or between
             * services) - brief backoff so this doesn't spin. */
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }

        if(open_result >= 0) avformat_close_input(&fmt_ctx);
        else avformat_free_context(fmt_ctx);
        /* AVFMT_FLAG_CUSTOM_IO means avformat_close_input() never touches
         * pb - we own avio_ctx and its buffer regardless of open/close
         * outcome above, so free them unconditionally here. */
        av_freep(&avio_ctx->buffer);
        avio_context_free(&avio_ctx);

        decode_reset_requested.store(false, std::memory_order_relaxed);
        g_ts_ring.clear();
    }
}

void request_video_reset()
{
    decode_reset_requested.store(true, std::memory_order_relaxed);
    clear_audio_queue();
    /* Drop whatever's still buffered from the old stream *here*, not just in
     * decode_loop()'s own end-of-session cleanup - otherwise the old decode
     * session keeps draining its backlog (still showing the old picture)
     * for as long as that takes before it ever notices decode_reset_requested,
     * instead of stopping the moment we know a new stream is coming. Clear
     * before wake() so the pop() it unblocks sees an empty buffer and takes
     * the EOF path immediately rather than returning more stale bytes. */
    g_ts_ring.clear();
    /* Setting the flag alone isn't enough - decode_loop() only checks it
     * between reads, so a decode session with no more TS arriving would
     * otherwise sit blocked in TsRingBuffer::pop() forever, never getting
     * back around to see it. */
    g_ts_ring.wake();
}

void start_video_pipeline()
{
    start_audio_output();
    start_udp_receiver();
    decode_thread_running = true;
    decode_thread = std::thread(decode_loop);
}

/* Registered with atexit() rather than run after driver_backends_run_loop()
 * - that loop never actually returns (see run_loop_sdl()'s while(true)); LVGL's
 * SDL driver calls exit() directly on quit, and our own exit button does the
 * same. exit() still destroys these global std::threads (std::terminate() if
 * still joinable - see where they're declared) and, more subtly, still
 * destroys g_ts_ring's mutex/condition_variable regardless of whether
 * decode_thread is done with them. A detached thread doesn't protect against
 * that second part - it can still be mid-TsRingBuffer::pop() when the main
 * thread gets there, which was observed hanging the whole process on exit.
 * join() is what actually waits for it to be safe, and it's bounded now:
 * SO_RCVTIMEO caps udp_thread's recv() wait (closing the socket first cuts
 * that further), and g_ts_ring.wake() breaks decode_thread out of the only
 * unbounded wait in its loop. */
void stop_background_threads()
{
    chat_ws_running = false;
    if(chat_ws_context != nullptr) lws_cancel_service(chat_ws_context);
    if(chat_ws_thread.joinable()) chat_ws_thread.join();

    batc_ws_thread_running = false;
    if(websocket_context != nullptr) lws_cancel_service(websocket_context);
    if(batc_ws_thread.joinable()) batc_ws_thread.join();

    local_ws_thread_running = false;
    if(longmynd_ws_context != nullptr) lws_cancel_service(longmynd_ws_context);
    if(local_ws_thread.joinable()) local_ws_thread.join();

    udp_thread_running = false;
    decode_thread_running = false;
    if(udp_sock >= 0) {
        close(udp_sock);
        udp_sock = -1;
    }
    g_ts_ring.wake();
    if(udp_thread.joinable()) udp_thread.join();
    if(decode_thread.joinable()) decode_thread.join();

    if(audio_device != 0) {
        SDL_CloseAudioDevice(audio_device);
        audio_device = 0;
    }

}

lv_obj_t * video_canvas = nullptr;
uint16_t * video_canvas_pixels = nullptr;
int video_canvas_w = VIDEO_SMALL_W;
int video_canvas_h = VIDEO_SMALL_H;

void video_frame_update_cb(lv_timer_t *)
{
    static bool first_call = true;
    if(first_call) {
        first_call = false;
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - g_boot_t0).count();
        std::fprintf(stderr, "[STARTUP] first video_frame_update_cb call at +%.1fms\n", ms);
    }

    if(frame_ready.load(std::memory_order_acquire) && video_canvas != nullptr) {
        std::lock_guard<std::mutex> lock(frame_mutex);
        /* video_panel_click_cb resizes video_canvas_pixels synchronously on
         * tap, but decode_loop's frame_pixels only catches up to the new
         * target size on its own schedule (next decoded frame) - skip the
         * copy until both agree on a size, instead of risking a copy sized
         * for the old buffer into the new (possibly smaller) one. */
        if(frame_pixels.size() == static_cast<size_t>(video_canvas_w) * video_canvas_h) {
            std::memcpy(video_canvas_pixels, frame_pixels.data(), frame_pixels.size() * sizeof(uint16_t));
            lv_obj_invalidate(video_canvas);
            /* Consume it - otherwise this timer redoes the same copy and
             * redraw every 33ms even when decode_loop hasn't produced a new
             * frame, for no visible benefit. */
            frame_ready.store(false, std::memory_order_relaxed);
            if(tuning_overlay != nullptr &&
               decode_session_count.load(std::memory_order_relaxed) >= tuning_overlay_target_session)
                lv_obj_add_flag(tuning_overlay, LV_OBJ_FLAG_HIDDEN);

            /* Diagnostic: gap since the previous successful render - an
             * aggregate rate can hide "many small stalls" behind an OK-
             * looking average; this catches an individual stutter directly. */
            static auto last_render = std::chrono::steady_clock::now();
            const auto render_now = std::chrono::steady_clock::now();
            const double gap_ms = std::chrono::duration<double, std::milli>(render_now - last_render).count();
            if(gap_ms > 100.0)
                std::fprintf(stderr, "[RENDER] stutter: %.1fms since previous frame\n", gap_ms);
            last_render = render_now;
        }
    }

    std::string video_name, audio_name;
    {
        std::lock_guard<std::mutex> lock(codec_name_mutex);
        video_name = video_codec_name;
        audio_name = audio_codec_name;
    }
    if(g_video_codec_value != nullptr) {
        lv_label_set_text(g_video_codec_value, video_name.empty() ? "---" : video_name.c_str());
        lv_obj_set_style_text_color(g_video_codec_value, video_name.empty() ? COLOR_TEXT_DIM : COLOR_TEXT, 0);
    }
    if(g_audio_codec_value != nullptr) {
        lv_label_set_text(g_audio_codec_value, audio_name.empty() ? "---" : audio_name.c_str());
        lv_obj_set_style_text_color(g_audio_codec_value, audio_name.empty() ? COLOR_TEXT_DIM : COLOR_TEXT, 0);
    }
}

/* Tap the video to fill the screen with it (hiding spectrum/status/status2,
 * see normal_view), tap again to go back. decode_loop() notices the
 * video_fullscreen flip on its own and rescales into the new target size -
 * this just handles the UI side: canvas buffer/position and panel geometry. */
void video_panel_click_cb(lv_event_t *)
{
    const bool now_fullscreen = !video_fullscreen.load(std::memory_order_relaxed);
    video_fullscreen.store(now_fullscreen, std::memory_order_relaxed);

    const int new_w = now_fullscreen ? SCREEN_W : VIDEO_SMALL_W;
    const int new_h = now_fullscreen ? SCREEN_H : VIDEO_SMALL_H;

    video_canvas_pixels = static_cast<uint16_t *>(
        lv_realloc(video_canvas_pixels, static_cast<size_t>(new_w) * new_h * sizeof(uint16_t)));
    std::fill(video_canvas_pixels, video_canvas_pixels + new_w * new_h, 0);
    video_canvas_w = new_w;
    video_canvas_h = new_h;
    lv_canvas_set_buffer(video_canvas, video_canvas_pixels, new_w, new_h, LV_COLOR_FORMAT_RGB565);

    if(now_fullscreen) {
        lv_obj_set_pos(video_panel, 0, 0);
        lv_obj_set_size(video_panel, SCREEN_W, SCREEN_H);
        lv_obj_set_pos(video_canvas, 0, 0);
        lv_obj_add_flag(normal_view, LV_OBJ_FLAG_HIDDEN);
    }
    else {
        lv_obj_set_pos(video_panel, 4, BOTTOM_ROW_Y);
        lv_obj_set_size(video_panel, VIDEO_PANEL_W, VIDEO_PANEL_H);
        lv_obj_set_pos(video_canvas, CONTENT_MARGIN, 1);
        lv_obj_remove_flag(normal_view, LV_OBJ_FLAG_HIDDEN);
    }
}

void build_video_panel(lv_obj_t * screen)
{
    constexpr int panel_x = 4, panel_y = BOTTOM_ROW_Y, panel_w = VIDEO_PANEL_W, panel_h = VIDEO_PANEL_H;
    lv_obj_t * panel = make_panel(screen, panel_x, panel_y, panel_w, panel_h);
    lv_obj_set_style_pad_all(panel, 0, 0);
    video_panel = panel;

    video_canvas_pixels = static_cast<uint16_t *>(lv_malloc(VIDEO_SMALL_W * VIDEO_SMALL_H * sizeof(uint16_t)));
    std::fill(video_canvas_pixels, video_canvas_pixels + VIDEO_SMALL_W * VIDEO_SMALL_H, 0);
    video_canvas_w = VIDEO_SMALL_W;
    video_canvas_h = VIDEO_SMALL_H;

    video_canvas = lv_canvas_create(panel);
    lv_canvas_set_buffer(video_canvas, video_canvas_pixels, VIDEO_SMALL_W, VIDEO_SMALL_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(video_canvas, CONTENT_MARGIN, 1);

    /* Created after video_canvas so it draws on top; LV_PCT sizing tracks
     * video_panel_click_cb's fullscreen/small resizing automatically, no
     * extra handling needed there. Not clickable, so taps fall through to
     * panel's own click handler below (tap video to fill screen) even while
     * this is showing. */
    tuning_overlay = lv_obj_create(panel);
    lv_obj_set_pos(tuning_overlay, 0, 0);
    lv_obj_set_size(tuning_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(tuning_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(tuning_overlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(tuning_overlay, 0, 0);
    lv_obj_set_style_radius(tuning_overlay, 0, 0);
    lv_obj_remove_flag(tuning_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(tuning_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(tuning_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t * tuning_overlay_label = make_label(tuning_overlay, "Please wait,\nTuning...",
                                                 COLOR_TEXT, &lv_font_montserrat_32);
    lv_obj_set_style_text_align(tuning_overlay_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(tuning_overlay_label);

    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(panel, video_panel_click_cb, LV_EVENT_CLICKED, nullptr);

    start_video_pipeline();
    /* Panel refreshes at ~58Hz (xrandr) and decode targets 50fps - sampling
     * at the old 33ms/~30Hz meant up to ~40% of already-decoded frames were
     * silently overwritten in frame_pixels before this timer ever looked at
     * them. 17ms comfortably covers both; this timer already skips all real
     * work (the memcpy/redraw below) whenever decode hasn't produced a new
     * frame, so firing it more often costs next to nothing. */
    lv_timer_create(video_frame_update_cb, 17, nullptr);
}

/* ===================================================================
 * Settings page (gear button on the main view). LNB LO offset and bias-tee
 * voltage only - see the "Settings persistence" section above for how
 * these are stored/applied.
 * =================================================================== */

lv_obj_t * settings_lo_spinbox = nullptr;
lv_obj_t * settings_voltage_btns[3] = {nullptr, nullptr, nullptr}; /* None, 13V, 18V */
lv_obj_t * settings_saved_label = nullptr;
lv_obj_t * settings_tuner_value = nullptr;
lv_obj_t * settings_link_value = nullptr;
int settings_voltage_choice = 0; /* 0=None, 1=13V, 2=18V - synced from g_lnb_voltage_* on page show */

/* Scans /sys/bus/usb/devices for an attached FTDI FT2232H reporting the
 * MiniTiouner's VID:PID (0403:6010), and returns its USB "product" string
 * (e.g. "MiniTiouner_Pro_TS2", "MiniTiouner") - the exact string
 * longmynd_ws/minitiouner.rules must match for the device node to be
 * readable/writable without root. Empty if none is attached. */
std::string detect_tuner_product_string()
{
    DIR * dir = opendir("/sys/bus/usb/devices");
    if(dir == nullptr) return {};

    std::string result;
    struct dirent * entry;
    while((entry = readdir(dir)) != nullptr) {
        const std::string path = std::string("/sys/bus/usb/devices/") + entry->d_name;

        char vid[8] = {0};
        FILE * vf = std::fopen((path + "/idVendor").c_str(), "r");
        if(vf == nullptr) continue;
        const bool got_vid = std::fgets(vid, sizeof(vid), vf) != nullptr;
        std::fclose(vf);
        if(!got_vid || std::strncmp(vid, "0403", 4) != 0) continue;

        char pid[8] = {0};
        FILE * pf = std::fopen((path + "/idProduct").c_str(), "r");
        if(pf == nullptr) continue;
        const bool got_pid = std::fgets(pid, sizeof(pid), pf) != nullptr;
        std::fclose(pf);
        if(!got_pid || std::strncmp(pid, "6010", 4) != 0) continue;

        FILE * prod_f = std::fopen((path + "/product").c_str(), "r");
        if(prod_f == nullptr) continue;
        char product[64] = {0};
        if(std::fgets(product, sizeof(product), prod_f) != nullptr) {
            result = product;
            while(!result.empty() && (result.back() == '\n' || result.back() == '\r'))
                result.pop_back();
        }
        std::fclose(prod_f);
        break;
    }
    closedir(dir);
    return result;
}

void update_settings_voltage_btn_styles()
{
    for(int i = 0; i < 3; ++i) {
        const bool selected = (i == settings_voltage_choice);
        lv_obj_set_style_bg_color(settings_voltage_btns[i], selected ? lv_color_hex(0x1f4d33) : COLOR_PANEL, 0);
        lv_obj_set_style_border_color(settings_voltage_btns[i], selected ? COLOR_GREEN : COLOR_BORDER, 0);
    }
}

void settings_voltage_btn_cb(lv_event_t * event)
{
    settings_voltage_choice = static_cast<int>(
        reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    update_settings_voltage_btn_styles();
}

void settings_lo_inc_cb(lv_event_t *) { lv_spinbox_increment(settings_lo_spinbox); }
void settings_lo_dec_cb(lv_event_t *) { lv_spinbox_decrement(settings_lo_spinbox); }

void show_settings_cb(lv_event_t *)
{
    lv_spinbox_set_value(settings_lo_spinbox, static_cast<int32_t>(std::lround(g_lnb_lo_mhz)));
    settings_voltage_choice = !g_lnb_voltage_enabled ? 0 : (g_lnb_voltage_horizontal ? 2 : 1);
    update_settings_voltage_btn_styles();
    lv_obj_add_flag(settings_saved_label, LV_OBJ_FLAG_HIDDEN);

    const std::string tuner_product = detect_tuner_product_string();
    if(tuner_product.empty()) {
        lv_label_set_text(settings_tuner_value, "Not detected");
        lv_obj_set_style_text_color(settings_tuner_value, COLOR_RED, 0);
    } else {
        lv_label_set_text(settings_tuner_value, tuner_product.c_str());
        lv_obj_set_style_text_color(settings_tuner_value, COLOR_TEXT, 0);
    }

    if(g_monitor_ws_connected.load(std::memory_order_relaxed)) {
        lv_label_set_text(settings_link_value, "Connected");
        lv_obj_set_style_text_color(settings_link_value, COLOR_GREEN, 0);
    } else {
        lv_label_set_text(settings_link_value, "Not connected");
        lv_obj_set_style_text_color(settings_link_value, COLOR_RED, 0);
    }

    lv_obj_add_flag(normal_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(video_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(settings_page, LV_OBJ_FLAG_HIDDEN);
}

void hide_settings_cb(lv_event_t *)
{
    lv_obj_add_flag(settings_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(normal_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(video_panel, LV_OBJ_FLAG_HIDDEN);
}

void settings_save_btn_cb(lv_event_t *)
{
    g_lnb_lo_mhz = static_cast<double>(lv_spinbox_get_value(settings_lo_spinbox));
    g_lnb_voltage_enabled = (settings_voltage_choice != 0);
    g_lnb_voltage_horizontal = (settings_voltage_choice == 2);
    save_settings();
    apply_lnb_voltage();
    lv_obj_remove_flag(settings_saved_label, LV_OBJ_FLAG_HIDDEN);
}

void build_settings_page(lv_obj_t * screen)
{
    settings_page = lv_obj_create(screen);
    lv_obj_set_size(settings_page, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(settings_page, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(settings_page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(settings_page, 0, 0);
    lv_obj_set_style_pad_all(settings_page, 0, 0);
    lv_obj_remove_flag(settings_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(settings_page, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * title = make_label(settings_page, "SETTINGS", COLOR_CYAN, &lv_font_montserrat_20);
    lv_obj_set_pos(title, 12, 12);

    lv_obj_t * back = lv_button_create(settings_page);
    lv_obj_set_pos(back, 910, 5);
    lv_obj_set_size(back, 106, 38);
    lv_obj_set_style_bg_color(back, COLOR_PANEL, 0);
    lv_obj_set_style_border_color(back, COLOR_CYAN, 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_t * back_label = make_label(back, "BACK", COLOR_CYAN, &lv_font_montserrat_16);
    lv_obj_center(back_label);
    lv_obj_add_event_cb(back, hide_settings_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t * card = make_panel(settings_page, 40, 70, SCREEN_W - 80, 280);

    /* LNB LO offset row: spinbox with +/- buttons either side (touch-friendly,
     * no need for a full on-screen keyboard for a 4-5 digit MHz value). */
    lv_obj_t * lo_label = make_label(card, "LNB LO Offset (MHz)", COLOR_TEXT, &lv_font_montserrat_16);
    lv_obj_set_pos(lo_label, 24, 24);

    lv_obj_t * lo_dec = lv_button_create(card);
    lv_obj_set_pos(lo_dec, 24, 60);
    lv_obj_set_size(lo_dec, 48, 44);
    lv_obj_set_style_bg_color(lo_dec, COLOR_PANEL, 0);
    lv_obj_set_style_border_color(lo_dec, COLOR_BORDER, 0);
    lv_obj_set_style_border_width(lo_dec, 1, 0);
    lv_obj_t * lo_dec_label = make_label(lo_dec, "-", COLOR_TEXT, &lv_font_montserrat_20);
    lv_obj_center(lo_dec_label);
    lv_obj_add_event_cb(lo_dec, settings_lo_dec_cb, LV_EVENT_CLICKED, nullptr);

    settings_lo_spinbox = lv_spinbox_create(card);
    lv_obj_set_pos(settings_lo_spinbox, 80, 60);
    lv_obj_set_size(settings_lo_spinbox, 140, 44);
    lv_spinbox_set_range(settings_lo_spinbox, 1000, 20000);
    lv_spinbox_set_digit_format(settings_lo_spinbox, 5, 0);
    lv_spinbox_set_step(settings_lo_spinbox, 1);
    lv_spinbox_set_value(settings_lo_spinbox, 9750);
    lv_obj_set_style_text_font(settings_lo_spinbox, &lv_font_montserrat_20, 0);

    lv_obj_t * lo_inc = lv_button_create(card);
    lv_obj_set_pos(lo_inc, 228, 60);
    lv_obj_set_size(lo_inc, 48, 44);
    lv_obj_set_style_bg_color(lo_inc, COLOR_PANEL, 0);
    lv_obj_set_style_border_color(lo_inc, COLOR_BORDER, 0);
    lv_obj_set_style_border_width(lo_inc, 1, 0);
    lv_obj_t * lo_inc_label = make_label(lo_inc, "+", COLOR_TEXT, &lv_font_montserrat_20);
    lv_obj_center(lo_inc_label);
    lv_obj_add_event_cb(lo_inc, settings_lo_inc_cb, LV_EVENT_CLICKED, nullptr);

    /* LNB bias-tee voltage row: 3-way selector styled like the main page's
     * SNAP/CHAT/EXIT buttons. */
    lv_obj_t * v_label = make_label(card, "LNB Bias Voltage", COLOR_TEXT, &lv_font_montserrat_16);
    lv_obj_set_pos(v_label, 24, 132);

    const char * voltage_texts[3] = {"NONE", "13V", "18V"};
    for(int i = 0; i < 3; ++i) {
        lv_obj_t * btn = lv_button_create(card);
        lv_obj_set_pos(btn, 24 + i * 116, 168);
        lv_obj_set_size(btn, 104, 48);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_t * label = make_label(btn, voltage_texts[i], COLOR_TEXT, &lv_font_montserrat_16);
        lv_obj_center(label);
        lv_obj_add_event_cb(btn, settings_voltage_btn_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        settings_voltage_btns[i] = btn;
    }

    lv_obj_t * save_btn = lv_button_create(card);
    lv_obj_set_pos(save_btn, 24, 236);
    lv_obj_set_size(save_btn, 140, 48);
    lv_obj_set_style_bg_color(save_btn, COLOR_PANEL, 0);
    lv_obj_set_style_border_color(save_btn, COLOR_GREEN, 0);
    lv_obj_set_style_border_width(save_btn, 1, 0);
    lv_obj_t * save_label = make_label(save_btn, "SAVE", COLOR_GREEN, &lv_font_montserrat_16);
    lv_obj_center(save_label);
    lv_obj_add_event_cb(save_btn, settings_save_btn_cb, LV_EVENT_CLICKED, nullptr);

    settings_saved_label = make_label(card, "Saved - applied live", COLOR_GREEN, &lv_font_montserrat_16);
    lv_obj_set_pos(settings_saved_label, 180, 250);
    lv_obj_add_flag(settings_saved_label, LV_OBJ_FLAG_HIDDEN);

    /* Read-only tuner diagnostics, refreshed on every page show (see
     * show_settings_cb()) - not user-editable, just a quicker way to see
     * "which tuner is plugged in" and "is longmynd actually talking to it"
     * than a dmesg/journalctl dive. */
    lv_obj_t * tuner_label = make_label(card, "Tuner (USB)", COLOR_TEXT, &lv_font_montserrat_16);
    lv_obj_set_pos(tuner_label, 340, 24);
    settings_tuner_value = make_label(card, "Not detected", COLOR_RED, &lv_font_montserrat_16);
    lv_obj_set_pos(settings_tuner_value, 340, 56);

    lv_obj_t * link_label = make_label(card, "Longmynd Link", COLOR_TEXT, &lv_font_montserrat_16);
    lv_obj_set_pos(link_label, 340, 132);
    settings_link_value = make_label(card, "Not connected", COLOR_RED, &lv_font_montserrat_16);
    lv_obj_set_pos(settings_link_value, 340, 164);
}

void build_status_panel(lv_obj_t * screen)
{
    constexpr int panel_w = SCREEN_W - STATUS_PANEL_X - PANEL_MARGIN;
    constexpr int left_x = 10;
    constexpr int right_x = panel_w / 2 + 8;
    constexpr int value_offset = 90;
    lv_obj_t * panel = make_panel(screen, STATUS_PANEL_X, BOTTOM_ROW_Y, panel_w, VIDEO_PANEL_H);

    struct Row { const char * label; lv_obj_t ** value_out; };
    const Row left_rows[] = {
        {"Mode", &g_mode_value},
        {"MER", &g_mer_value},
        {"Quality", &g_quality_value},
        {"Margin", &g_margin_value},
        {"AGC", &g_agc_value},
        {"FEC", &g_fec_value},
        {"MOD", &g_mod_value},
    };
    const Row right_rows[] = {
        {"Service", &g_service_value},
        {"Video", &g_video_codec_value},
        {"Audio", &g_audio_codec_value},
        {"BER", &g_ber_value},
        {"LDPC", &g_ldpc_value},
        {"Frames", &g_shortframes_value},
        {"Pilots", &g_pilots_value},
    };

    auto build_column = [&](const Row * rows, size_t count, int x) {
        int y = 8;
        for(size_t i = 0; i < count; ++i) {
            lv_obj_t * label = make_label(panel, rows[i].label, COLOR_TEXT_DIM);
            lv_obj_set_pos(label, x, y);
            lv_obj_t * value = make_label(panel, "---", COLOR_TEXT_DIM);
            lv_obj_set_pos(value, x + value_offset, y);
            *rows[i].value_out = value;
            y += 24;
        }
    };
    build_column(left_rows, sizeof(left_rows) / sizeof(left_rows[0]), left_x);
    build_column(right_rows, sizeof(right_rows) / sizeof(right_rows[0]), right_x);

    constexpr int bottom_btn_y = VIDEO_PANEL_H - 54 - 11;
    lv_obj_t * volume_title = make_label(panel, "VOL", COLOR_TEXT_DIM);
    lv_obj_set_pos(volume_title, 10, bottom_btn_y - 52);

    char volume_text[12];
    std::snprintf(volume_text, sizeof(volume_text), "%d%%", audio_volume_percent.load());
    audio_volume_label = make_label(panel, volume_text, COLOR_TEXT);
    lv_obj_set_pos(audio_volume_label, panel_w - 46, bottom_btn_y - 52);

    lv_obj_t * volume_slider = lv_slider_create(panel);
    lv_obj_set_pos(volume_slider, 50, bottom_btn_y - 47);
    lv_obj_set_size(volume_slider, panel_w - 105, 7);
    lv_slider_set_range(volume_slider, 0, 100);
    lv_slider_set_value(volume_slider, audio_volume_percent.load(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(volume_slider, COLOR_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(volume_slider, lv_color_hex(0x2b8ea3), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(volume_slider, lv_color_hex(0x65b7c7), LV_PART_KNOB);
    lv_obj_set_style_width(volume_slider, 15, LV_PART_KNOB);
    lv_obj_set_style_height(volume_slider, 15, LV_PART_KNOB);
    lv_obj_add_event_cb(volume_slider, volume_slider_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    audio_vu_bar = lv_bar_create(panel);
    lv_obj_set_pos(audio_vu_bar, 50, bottom_btn_y - 27);
    lv_obj_set_size(audio_vu_bar, panel_w - 105, 5);
    lv_bar_set_range(audio_vu_bar, 0, 100);
    lv_bar_set_value(audio_vu_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(audio_vu_bar, COLOR_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(audio_vu_bar, COLOR_GREEN, LV_PART_INDICATOR);
    lv_timer_create(audio_vu_update_cb, 50, nullptr);

    constexpr int button_gap = 8;
    constexpr int button_margin = 8;
    constexpr int button_w = (panel_w - 2 * button_margin - 3 * button_gap) / 4;
    lv_obj_t * screenshot_btn = lv_button_create(panel);
    lv_obj_set_pos(screenshot_btn, button_margin, bottom_btn_y);
    lv_obj_set_size(screenshot_btn, button_w, 54);
    lv_obj_set_style_bg_color(screenshot_btn, COLOR_PANEL, 0);
    lv_obj_set_style_border_color(screenshot_btn, COLOR_GREEN, 0);
    lv_obj_set_style_border_width(screenshot_btn, 1, 0);
    lv_obj_t * screenshot_label = make_label(screenshot_btn, "SNAP", COLOR_GREEN, &lv_font_montserrat_16);
    lv_obj_center(screenshot_label);
    lv_obj_add_event_cb(screenshot_btn, screenshot_btn_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t * chat_btn = lv_button_create(panel);
    lv_obj_set_pos(chat_btn, button_margin + button_w + button_gap, bottom_btn_y);
    lv_obj_set_size(chat_btn, button_w, 54);
    lv_obj_set_style_bg_color(chat_btn, COLOR_PANEL, 0);
    lv_obj_set_style_border_color(chat_btn, COLOR_CYAN, 0);
    lv_obj_set_style_border_width(chat_btn, 1, 0);
    lv_obj_t * chat_label = make_label(chat_btn, LV_SYMBOL_ENVELOPE, COLOR_CYAN, &lv_font_montserrat_20);
    lv_obj_center(chat_label);
    lv_obj_add_event_cb(chat_btn, show_chat_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t * settings_btn = lv_button_create(panel);
    lv_obj_set_pos(settings_btn, button_margin + 2 * (button_w + button_gap), bottom_btn_y);
    lv_obj_set_size(settings_btn, button_w, 54);
    lv_obj_set_style_bg_color(settings_btn, COLOR_PANEL, 0);
    lv_obj_set_style_border_color(settings_btn, COLOR_YELLOW, 0);
    lv_obj_set_style_border_width(settings_btn, 1, 0);
    lv_obj_t * settings_label = make_label(settings_btn, LV_SYMBOL_SETTINGS, COLOR_YELLOW, &lv_font_montserrat_20);
    lv_obj_center(settings_label);
    lv_obj_add_event_cb(settings_btn, show_settings_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t * restart_btn = lv_button_create(panel);
    lv_obj_set_pos(restart_btn, button_margin + 3 * (button_w + button_gap), bottom_btn_y);
    lv_obj_set_size(restart_btn, button_w, 54);
    lv_obj_set_style_bg_color(restart_btn, COLOR_PANEL, 0);
    lv_obj_set_style_border_color(restart_btn, COLOR_RED, 0);
    lv_obj_set_style_border_width(restart_btn, 1, 0);
    lv_obj_t * restart_label = make_label(restart_btn, LV_SYMBOL_REFRESH, COLOR_RED, &lv_font_montserrat_20);
    lv_obj_center(restart_label);
    lv_obj_add_event_cb(restart_btn, restart_btn_cb, LV_EVENT_CLICKED, nullptr);
}

void auto_close_msgbox_cb(lv_timer_t * timer)
{
    lv_msgbox_close(static_cast<lv_obj_t *>(lv_timer_get_user_data(timer)));
}

} // namespace

int main()
{
    auto boot_mark = [&](const char * label) {
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - g_boot_t0).count();
        std::fprintf(stderr, "[STARTUP] %s at +%.1fms\n", label, ms);
    };

    settings.window_width = SCREEN_W;
    settings.window_height = SCREEN_H;
    settings.fullscreen = true;

    driver_backends_register();
    lv_init();
    boot_mark("lv_init done");

    char sdl_backend[] = "SDL";
    if(driver_backends_init_backend(sdl_backend) == -1) {
        fprintf(stderr, "Failed to initialize display backend\n");
        return 1;
    }
    boot_mark("display backend init done");

    /* Must come after driver_backends_init_backend() (SDL_Init) so this
     * overrides SDL's own SIGTERM/SIGINT handlers - see handle_termination_
     * signal()'s comment for why. */
    signal(SIGTERM, handle_termination_signal);
    signal(SIGINT, handle_termination_signal);
    lv_timer_create(termination_signal_check_cb, 100, nullptr);

    lv_obj_t * screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    load_settings();
    boot_mark("load_settings done");

    start_longmynd(BEACON_FREQ_KHZ, BEACON_SYMRATE_KSPS);
    boot_mark("start_longmynd done");
    atexit(stop_longmynd);
    atexit(stop_background_threads);

    /* Everything except the video panel lives in normal_view, so going
     * fullscreen (video_panel_click_cb) just hides this one container
     * instead of tracking three separate panels. */
    normal_view = lv_obj_create(screen);
    lv_obj_set_pos(normal_view, 0, 0);
    lv_obj_set_size(normal_view, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_opa(normal_view, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(normal_view, 0, 0);
    lv_obj_set_style_pad_all(normal_view, 0, 0);
    lv_obj_remove_flag(normal_view, LV_OBJ_FLAG_SCROLLABLE);

    build_spectrum_panel(normal_view);
    boot_mark("build_spectrum_panel done");
    build_video_panel(screen);
    boot_mark("build_video_panel done");
    build_status_panel(normal_view);
    boot_mark("status panels done");
    build_chat_page(screen);
    boot_mark("chat page done");
    build_settings_page(screen);
    boot_mark("settings page done");

    if(detect_tuner_product_string().empty()) {
        lv_obj_t * no_tuner_msgbox = lv_msgbox_create(nullptr);
        lv_obj_set_style_bg_color(no_tuner_msgbox, COLOR_PANEL, 0);
        lv_obj_t * no_tuner_title = lv_msgbox_add_title(no_tuner_msgbox, "No Tuner Found");
        lv_obj_set_style_text_color(no_tuner_title, COLOR_RED, 0);
        lv_obj_set_flex_grow(no_tuner_title, 1);
        lv_obj_set_style_text_align(no_tuner_title, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_t * no_tuner_header = lv_msgbox_get_header(no_tuner_msgbox);
        if(no_tuner_header != nullptr) {
            lv_obj_set_style_bg_color(no_tuner_header, COLOR_PANEL, 0);
            lv_obj_set_style_bg_opa(no_tuner_header, LV_OPA_COVER, 0);
        }
        lv_obj_t * no_tuner_text = lv_msgbox_add_text(no_tuner_msgbox,
            "No MiniTiouner detected on USB.\nCheck the cable and power, then restart the app.");
        lv_obj_set_style_text_color(no_tuner_text, COLOR_TEXT, 0);
        lv_obj_set_style_text_align(no_tuner_text, LV_TEXT_ALIGN_CENTER, 0);
        lv_msgbox_add_close_button(no_tuner_msgbox);
    }
    else {
        const std::string tuner_product = detect_tuner_product_string();
        lv_obj_t * tuner_msgbox = lv_msgbox_create(nullptr);
        lv_obj_set_style_bg_color(tuner_msgbox, COLOR_PANEL, 0);
        lv_obj_t * tuner_title = lv_msgbox_add_title(tuner_msgbox, "Tuner Detected");
        lv_obj_set_style_text_color(tuner_title, COLOR_GREEN, 0);
        lv_obj_set_flex_grow(tuner_title, 1);
        lv_obj_set_style_text_align(tuner_title, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_t * tuner_header = lv_msgbox_get_header(tuner_msgbox);
        if(tuner_header != nullptr) {
            lv_obj_set_style_bg_color(tuner_header, COLOR_PANEL, 0);
            lv_obj_set_style_bg_opa(tuner_header, LV_OPA_COVER, 0);
        }
        lv_obj_t * tuner_text = lv_msgbox_add_text(tuner_msgbox, tuner_product.c_str());
        lv_obj_set_style_text_color(tuner_text, COLOR_TEXT, 0);
        lv_obj_set_style_text_align(tuner_text, LV_TEXT_ALIGN_CENTER, 0);
        lv_timer_t * auto_close_timer = lv_timer_create(auto_close_msgbox_cb, 3000, tuner_msgbox);
        lv_timer_set_repeat_count(auto_close_timer, 1);
    }
    boot_mark("tuner presence check done");

    start_websocket();
    boot_mark("start_websocket done");
    lv_timer_create(service_websocket, 10, nullptr);

    start_local_websocket();
    boot_mark("start_local_websocket done");
    lv_timer_create(service_local_websocket, 10, nullptr);
    lv_timer_create(service_status_fifo, 20, nullptr);
    apply_lnb_voltage();

    start_chat_websocket();
    boot_mark("start_chat_websocket done");
    lv_timer_create(service_chat_websocket, 50, nullptr);

    boot_mark("entering run loop");
    driver_backends_run_loop();

    /* Dead code for the SDL backend (run_loop_sdl() never returns - see
     * stop_background_threads()'s comment for how shutdown actually happens),
     * kept in case a future backend's run loop does return normally. */
    stop_background_threads();

    if(websocket_context != nullptr) lws_context_destroy(websocket_context);
    if(longmynd_ws_context != nullptr) lws_context_destroy(longmynd_ws_context);
    return 0;
}
