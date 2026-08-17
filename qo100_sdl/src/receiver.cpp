#include "receiver.h"
#include "app_log.h"

#include <json-c/json.h>
#include <libwebsockets.h>
#ifndef LWS_PROTOCOL_LIST_TERM
#define LWS_PROTOCOL_LIST_TERM {nullptr, nullptr, 0, 0, 0, nullptr, 0}
#endif

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <mutex>
#include <signal.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <sys/socket.h>

namespace qo100 {
namespace {

constexpr int kWebsocketPort = 8765;
constexpr auto kReconnectInterval = std::chrono::milliseconds(250);
constexpr size_t kMaxPendingCommands = 16;

/* Destination the transport stream is sent to. Defaults to a multicast
 * group so a second device on the LAN can watch the same feed in VLC
 * (udp://@<addr>:<port>); override via env if that clashes with something
 * else on the network, or set back to 127.0.0.1 for loopback-only. Must
 * match video_decoder.cpp's kInputUrl on the receiving end. */
std::string ts_destination_address()
{
    const char * value = std::getenv("QO100_TS_ADDR");
    return value != nullptr ? value : "239.1.1.1";
}

int ts_destination_port()
{
    const char * value = std::getenv("QO100_TS_PORT");
    return value != nullptr ? std::atoi(value) : 5600;
}

bool websocket_server_ready()
{
    const int descriptor = socket(AF_INET, SOCK_STREAM, 0);
    if(descriptor < 0) return false;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kWebsocketPort);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool ready = connect(descriptor,
        reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0;
    close(descriptor);
    return ready;
}

int json_int(json_object * object, const char * key, int fallback)
{
    json_object * value = nullptr;
    return object != nullptr && json_object_object_get_ex(object, key, &value)
        ? json_object_get_int(value) : fallback;
}

long json_long(json_object * object, const char * key, long fallback)
{
    json_object * value = nullptr;
    return object != nullptr && json_object_object_get_ex(object, key, &value)
        ? std::lround(json_object_get_double(value)) : fallback;
}

int json_bool_int(json_object * object, const char * key, int fallback)
{
    json_object * value = nullptr;
    return object != nullptr && json_object_object_get_ex(object, key, &value)
        ? (json_object_get_boolean(value) ? 1 : 0) : fallback;
}

std::string json_string(json_object * object, const char * key,
                        const std::string & fallback)
{
    json_object * value = nullptr;
    if(object == nullptr || !json_object_object_get_ex(object, key, &value)) return fallback;
    const char * text = json_object_get_string(value);
    return text != nullptr ? text : fallback;
}

bool parse_monitor_json(const std::string & text, ReceiverStatus & status)
{
    json_object * root = json_tokener_parse(text.c_str());
    if(root == nullptr) return false;
    json_object * packet = nullptr;
    json_object * rx = nullptr;
    json_object * ts = nullptr;
    json_object_object_get_ex(root, "packet", &packet);
    if(packet != nullptr) {
        json_object_object_get_ex(packet, "rx", &rx);
        json_object_object_get_ex(packet, "ts", &ts);
    }
    if(rx != nullptr) {
        status.demod_state = json_int(rx, "demod_state", status.demod_state);
        status.carrier_khz = json_long(rx, "frequency", status.carrier_khz);
        status.symbol_rate_ksps =
            (json_long(rx, "symbolrate", status.symbol_rate_ksps * 1000L) + 500L) / 1000L;
        status.ber_x100 = json_int(rx, "ber", status.ber_x100);
        status.mer_x10 = json_int(rx, "mer", status.mer_x10);
        status.modcod = json_int(rx, "modcod", status.modcod);
        status.short_frames = json_bool_int(rx, "short_frame", status.short_frames);
        status.pilots = json_bool_int(rx, "pilot_symbols", status.pilots);
        status.ldpc_errors = json_long(rx, "errors_ldpc_count", status.ldpc_errors);
        status.agc1 = json_long(rx, "agc1", status.agc1);
        status.agc2 = json_long(rx, "agc2", status.agc2);
    }
    if(ts != nullptr) {
        status.service_name = json_string(ts, "service_name", status.service_name);
        status.service_provider = json_string(ts, "service_provider_name", status.service_provider);
        /* longmynd recomputes this fresh from scratch on every ~54-packet
         * window (see ts_parse() resetting its counters each call) with no
         * smoothing at all, so the raw reading jumps around a lot for a
         * given signal. Settle it with an EMA rather than showing that
         * noise directly - first sample seeds it outright. */
        const int raw_null_percent = json_int(ts, "null_ratio", -1);
        if(raw_null_percent >= 0) {
            constexpr double kNullPercentEmaAlpha = 0.2;
            status.null_packet_percent = status.null_packet_percent < 0
                ? raw_null_percent
                : static_cast<int>(std::lround(
                      kNullPercentEmaAlpha * raw_null_percent +
                      (1.0 - kNullPercentEmaAlpha) * status.null_packet_percent));
        }
    }
    json_object_put(root);
    return rx != nullptr || ts != nullptr;
}

bool pid_matches_binary(int pid, const std::string & expected_binary)
{
    char link_path[64]{};
    std::snprintf(link_path, sizeof(link_path), "/proc/%d/exe", pid);
    char actual[4096]{};
    const ssize_t length = readlink(link_path, actual, sizeof(actual) - 1U);
    if(length <= 0) return false;
    actual[length] = '\0';
    return expected_binary == actual;
}

} // namespace

std::string ts_stream_vlc_url()
{
    return "udp://@" + ts_destination_address() + ":" +
        std::to_string(ts_destination_port());
}

ReceiverSettings load_receiver_settings(const std::string & repository_root)
{
    ReceiverSettings settings;
    const std::string path = repository_root + "/qo100_sdl/settings.json";
    json_object * root = json_object_from_file(path.c_str());
    if(root == nullptr) {
        qo100::log( "[SETTINGS] using defaults; could not read %s\n", path.c_str());
        return settings;
    }
    json_object * value = nullptr;
    if(json_object_object_get_ex(root, "lnb_lo_mhz", &value))
        settings.lnb_lo_mhz = json_object_get_double(value);
    if(json_object_object_get_ex(root, "lnb_voltage_enabled", &value))
        settings.lnb_voltage_enabled = json_object_get_boolean(value);
    if(json_object_object_get_ex(root, "lnb_voltage_horizontal", &value))
        settings.lnb_voltage_horizontal = json_object_get_boolean(value);
    if(json_object_object_get_ex(root, "audio_volume_percent", &value))
        settings.audio_volume_percent = json_object_get_int(value);
    if(json_object_object_get_ex(root, "display_800x480", &value))
        settings.display_800x480 = json_object_get_boolean(value);
    if(json_object_object_get_ex(root, "exit_full_stop", &value))
        settings.exit_full_stop = json_object_get_boolean(value);
    json_object_put(root);
    qo100::log( "[SETTINGS] LO=%.1fMHz voltage=%s/%s volume=%d%% display=%s exit=%s\n",
        settings.lnb_lo_mhz, settings.lnb_voltage_enabled ? "on" : "off",
        settings.lnb_voltage_horizontal ? "18V" : "13V", settings.audio_volume_percent,
        settings.display_800x480 ? "800x480" : "1024x600",
        settings.exit_full_stop ? "full-stop" : "restart");
    return settings;
}

bool save_receiver_settings(const std::string & repository_root,
                            const ReceiverSettings & settings)
{
    const std::string path = repository_root + "/qo100_sdl/settings.json";
    json_object * root = json_object_new_object();
    if(root == nullptr) return false;
    json_object_object_add(root, "lnb_lo_mhz",
                           json_object_new_double(settings.lnb_lo_mhz));
    json_object_object_add(root, "lnb_voltage_enabled",
                           json_object_new_boolean(settings.lnb_voltage_enabled));
    json_object_object_add(root, "lnb_voltage_horizontal",
                           json_object_new_boolean(settings.lnb_voltage_horizontal));
    json_object_object_add(root, "audio_volume_percent",
                           json_object_new_int(settings.audio_volume_percent));
    json_object_object_add(root, "display_800x480",
                           json_object_new_boolean(settings.display_800x480));
    json_object_object_add(root, "exit_full_stop",
                           json_object_new_boolean(settings.exit_full_stop));
    const bool saved = json_object_to_file_ext(
        path.c_str(), root, JSON_C_TO_STRING_PRETTY) == 0;
    json_object_put(root);
    qo100::log("[SETTINGS] %s %s\n", saved ? "saved" : "could not save", path.c_str());
    return saved;
}

void ReceiverStatus::reset()
{
    *this = ReceiverStatus{};
}

LongmyndProcess::LongmyndProcess(std::string repository_root)
    : repository_root_(std::move(repository_root)),
      directory_(repository_root_ + "/longmynd_ws"),
      binary_(directory_ + "/longmynd"),
      log_path_(repository_root_ + "/qo100_sdl/longmynd.log"),
      pid_path_(repository_root_ + "/qo100_sdl/longmynd.pid")
{}

LongmyndProcess::~LongmyndProcess()
{
    stop();
}

bool LongmyndProcess::start(long frequency_khz, long symbol_rate_ksps)
{
    if(running()) return true;
    if(access(binary_.c_str(), X_OK) != 0) {
        qo100::log( "[LONGMYND] binary is not executable: %s\n", binary_.c_str());
        return false;
    }

    FILE * stale_file = std::fopen(pid_path_.c_str(), "r");
    if(stale_file != nullptr) {
        int stale_pid = -1;
        const bool read_pid = std::fscanf(stale_file, "%d", &stale_pid) == 1;
        std::fclose(stale_file);
        if(read_pid && stale_pid > 0 && pid_matches_binary(stale_pid, binary_)) {
            qo100::log( "[LONGMYND] stopping stale owned process %d\n", stale_pid);
            kill(stale_pid, SIGTERM);
        }
        std::remove(pid_path_.c_str());
    }

    const std::string ts_address = ts_destination_address();
    const int ts_port = ts_destination_port();

    const pid_t child = fork();
    if(child < 0) {
        qo100::log( "[LONGMYND] fork failed: %s\n", std::strerror(errno));
        return false;
    }
    if(child == 0) {
        chdir(directory_.c_str());
        const int descriptor = open(log_path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if(descriptor >= 0) {
            dup2(descriptor, STDOUT_FILENO);
            dup2(descriptor, STDERR_FILENO);
            close(descriptor);
        }
        char port[16]{};
        char udp_port[16]{};
        char frequency[24]{};
        char symbol_rate[24]{};
        std::snprintf(port, sizeof(port), "%d", kWebsocketPort);
        std::snprintf(udp_port, sizeof(udp_port), "%d", ts_port);
        std::snprintf(frequency, sizeof(frequency), "%ld", frequency_khz);
        std::snprintf(symbol_rate, sizeof(symbol_rate), "%ld", symbol_rate_ksps);
        execl("/usr/bin/stdbuf", "stdbuf", "-oL", "-eL", binary_.c_str(),
              "-W", port, "-i", ts_address.c_str(), udp_port,
              frequency, symbol_rate, static_cast<char *>(nullptr));
        _exit(127);
    }
    qo100::log("[LONGMYND] TS destination=%s:%d\n", ts_address.c_str(), ts_port);

    pid_ = static_cast<int>(child);
    FILE * pid_file = std::fopen(pid_path_.c_str(), "w");
    if(pid_file != nullptr) {
        std::fprintf(pid_file, "%d\n", pid_);
        std::fclose(pid_file);
    }
    qo100::log( "[LONGMYND] started pid=%d frequency=%ldkHz sr=%ldkS/s\n",
                 pid_, frequency_khz, symbol_rate_ksps);
    return true;
}

void LongmyndProcess::stop()
{
    if(pid_ <= 0) return;
    const int child = pid_;
    pid_ = -1;
    kill(child, SIGTERM);
    for(int attempt = 0; attempt < 30; ++attempt) {
        int status = 0;
        const pid_t result = waitpid(child, &status, WNOHANG);
        if(result == child || result < 0) {
            std::remove(pid_path_.c_str());
            qo100::log( "[LONGMYND] stopped pid=%d\n", child);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    qo100::log( "[LONGMYND] pid=%d did not stop in time; terminating\n", child);
    kill(child, SIGKILL);
    waitpid(child, nullptr, 0);
    std::remove(pid_path_.c_str());
}

bool LongmyndProcess::running() const
{
    return pid_ > 0 && kill(pid_, 0) == 0;
}

struct LongmyndClient::Impl {
    std::atomic<bool> running{false};
    std::thread thread;
    /* Guards `context` against a genuine use-after-free race: run() (its
     * owning thread) destroys the context right after the run loop exits,
     * while stop()/queue() (called from the main thread) read `context` and
     * call lws_cancel_service() on it to wake the loop up early. A plain
     * atomic pointer only makes the *read* of the pointer safe, not the use
     * of what it points to - a load() on the main thread can return a
     * still-non-null pointer microseconds before run()'s thread starts
     * lws_context_destroy() on that same object, so lws_cancel_service()
     * and lws_context_destroy() can execute concurrently on the same
     * context (observed in practice as a libwebsockets internal refcount
     * assertion failure/SIGABRT during EXIT's shutdown). Holding this mutex
     * across both "null out and hand off to destroy" (run()) and "read and
     * call lws_cancel_service" (stop()/queue()) makes the two mutually
     * exclusive. */
    std::mutex context_mutex;
    lws_context * context = nullptr;
    lws * monitor_wsi = nullptr;
    lws * control_wsi = nullptr;
    std::atomic<bool> monitor_is_connected{false};
    std::atomic<bool> control_is_connected{false};
    uint32_t monitor_connect_attempts = 0;
    uint32_t control_connect_attempts = 0;

    std::mutex handoff_mutex;
    std::string pending_json;
    std::deque<std::string> pending_commands;
    std::vector<uint8_t> monitor_buffer;
    bool monitor_binary = false;
    std::atomic<uint64_t> received{0};
    std::atomic<uint64_t> replaced{0};

    static Impl * from(lws * websocket)
    {
        return static_cast<Impl *>(lws_context_user(lws_get_context(websocket)));
    }

    static int monitor_callback(lws * websocket, lws_callback_reasons reason,
                                void *, void * data, size_t length)
    {
        Impl * self = from(websocket);
        return self != nullptr ? self->on_monitor(websocket, reason, data, length) : 0;
    }

    static int control_callback(lws * websocket, lws_callback_reasons reason,
                                void *, void * data, size_t length)
    {
        Impl * self = from(websocket);
        return self != nullptr ? self->on_control(websocket, reason, data, length) : 0;
    }

    static const lws_protocols * protocols()
    {
        static const lws_protocols value[] = {
            {"monitor", &Impl::monitor_callback, 0, 16 * 1024, 0, nullptr, 0},
            {"control", &Impl::control_callback, 0, 256, 0, nullptr, 0},
            LWS_PROTOCOL_LIST_TERM
        };
        return value;
    }

    int on_monitor(lws * websocket, lws_callback_reasons reason, void * data, size_t length)
    {
        switch(reason) {
            case LWS_CALLBACK_CLIENT_ESTABLISHED:
                monitor_wsi = websocket;
                monitor_is_connected = true;
                qo100::log( "[LONGMYND-WS] monitor connected after %u attempt%s\n",
                            monitor_connect_attempts,
                            monitor_connect_attempts == 1 ? "" : "s");
                break;
            case LWS_CALLBACK_CLIENT_RECEIVE: {
                if(lws_is_first_fragment(websocket)) {
                    monitor_buffer.clear();
                    monitor_binary = lws_frame_is_binary(websocket) != 0;
                }
                const auto * bytes = static_cast<const uint8_t *>(data);
                monitor_buffer.insert(monitor_buffer.end(), bytes, bytes + length);
                if(lws_is_final_fragment(websocket) &&
                   lws_remaining_packet_payload(websocket) == 0U) {
                    if(!monitor_binary) {
                        std::lock_guard<std::mutex> lock(handoff_mutex);
                        if(!pending_json.empty()) ++replaced;
                        pending_json.assign(monitor_buffer.begin(), monitor_buffer.end());
                        ++received;
                    }
                    monitor_buffer.clear();
                }
                break;
            }
            case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
                if(monitor_connect_attempts == 1 || monitor_connect_attempts % 20 == 0) {
                    qo100::log( "[LONGMYND-WS] monitor connection failed: %.*s\n",
                                static_cast<int>(length),
                                data != nullptr ? static_cast<const char *>(data) : "unknown");
                }
                if(monitor_wsi == websocket) monitor_wsi = nullptr;
                monitor_is_connected = false;
                break;
            case LWS_CALLBACK_CLIENT_CLOSED:
                if(monitor_wsi == websocket) monitor_wsi = nullptr;
                monitor_is_connected = false;
                break;
            default:
                break;
        }
        return 0;
    }

    int on_control(lws * websocket, lws_callback_reasons reason,
                   void * data, size_t length)
    {
        switch(reason) {
            case LWS_CALLBACK_CLIENT_ESTABLISHED: {
                control_wsi = websocket;
                control_is_connected = true;
                qo100::log( "[LONGMYND-WS] control connected after %u attempt%s\n",
                            control_connect_attempts,
                            control_connect_attempts == 1 ? "" : "s");
                std::lock_guard<std::mutex> lock(handoff_mutex);
                if(!pending_commands.empty()) lws_callback_on_writable(websocket);
                break;
            }
            case LWS_CALLBACK_CLIENT_WRITEABLE: {
                std::string command;
                bool more = false;
                {
                    std::lock_guard<std::mutex> lock(handoff_mutex);
                    if(!pending_commands.empty()) {
                        command = std::move(pending_commands.front());
                        pending_commands.pop_front();
                        more = !pending_commands.empty();
                    }
                }
                if(!command.empty()) {
                    std::vector<uint8_t> buffer(LWS_PRE + command.size());
                    std::memcpy(buffer.data() + LWS_PRE, command.data(), command.size());
                    const int written = lws_write(websocket, buffer.data() + LWS_PRE,
                                                  command.size(), LWS_WRITE_TEXT);
                    if(written < 0) qo100::log( "[LONGMYND-WS] control write failed\n");
                    else qo100::log( "[LONGMYND-WS] sent %s\n", command.c_str());
                }
                if(more) lws_callback_on_writable(websocket);
                break;
            }
            case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
                if(control_connect_attempts == 1 || control_connect_attempts % 20 == 0) {
                    qo100::log( "[LONGMYND-WS] control connection failed: %.*s\n",
                                static_cast<int>(length),
                                data != nullptr ? static_cast<const char *>(data) : "unknown");
                }
                if(control_wsi == websocket) control_wsi = nullptr;
                control_is_connected = false;
                break;
            case LWS_CALLBACK_CLIENT_CLOSED:
                if(control_wsi == websocket) control_wsi = nullptr;
                control_is_connected = false;
                break;
            default:
                break;
        }
        return 0;
    }

    void connect_monitor(lws_context * active_context)
    {
        ++monitor_connect_attempts;
        lws_client_connect_info info{};
        info.context = active_context;
        info.address = "127.0.0.1";
        info.port = kWebsocketPort;
        info.path = "/";
        info.host = info.address;
        info.origin = info.address;
        info.local_protocol_name = "monitor";
        info.protocol = "monitor";
        info.pwsi = &monitor_wsi;
        monitor_wsi = lws_client_connect_via_info(&info);
    }

    void connect_control(lws_context * active_context)
    {
        ++control_connect_attempts;
        lws_client_connect_info info{};
        info.context = active_context;
        info.address = "127.0.0.1";
        info.port = kWebsocketPort;
        info.path = "/";
        info.host = info.address;
        info.origin = info.address;
        info.local_protocol_name = "control";
        info.protocol = "control";
        info.pwsi = &control_wsi;
        control_wsi = lws_client_connect_via_info(&info);
    }

    void run()
    {
        lws_context_creation_info info{};
        info.port = CONTEXT_PORT_NO_LISTEN;
        info.protocols = protocols();
        info.user = this;
        info.timeout_secs = 1;
        info.connect_timeout_secs = 1;
        lws_context * active_context = lws_create_context(&info);
        if(active_context == nullptr) return;
        {
            std::lock_guard<std::mutex> lock(context_mutex);
            context = active_context;
        }
        const auto wait_started = std::chrono::steady_clock::now();
        qo100::log( "[LONGMYND-WS] waiting for server port %d\n", kWebsocketPort);
        auto last_attempt = wait_started - kReconnectInterval;
        auto last_wait_report = wait_started;
        bool server_ready_reported = false;
        while(running.load(std::memory_order_relaxed)) {
            const auto now = std::chrono::steady_clock::now();
            if((monitor_wsi == nullptr || control_wsi == nullptr) &&
               now - last_attempt >= kReconnectInterval) {
                last_attempt = now;
                if(websocket_server_ready()) {
                    if(!server_ready_reported) {
                        const auto ready_ms = std::chrono::duration_cast<
                            std::chrono::milliseconds>(now - wait_started).count();
                        qo100::log( "[LONGMYND-WS] server ready after %lldms\n",
                                    static_cast<long long>(ready_ms));
                        server_ready_reported = true;
                    }
                    if(monitor_wsi == nullptr) connect_monitor(active_context);
                    if(control_wsi == nullptr) connect_control(active_context);
                }
                else if(now - last_wait_report >= std::chrono::seconds(5)) {
                    last_wait_report = now;
                    qo100::log( "[LONGMYND-WS] still waiting for server\n");
                }
            }
            bool command_waiting = false;
            {
                std::lock_guard<std::mutex> lock(handoff_mutex);
                command_waiting = !pending_commands.empty();
            }
            if(command_waiting && control_wsi != nullptr)
                lws_callback_on_writable(control_wsi);
            if(monitor_wsi != nullptr || control_wsi != nullptr)
                lws_service(active_context, 50);
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        {
            std::lock_guard<std::mutex> lock(context_mutex);
            context = nullptr;
        }
        lws_context_destroy(active_context);
        monitor_wsi = nullptr;
        control_wsi = nullptr;
        monitor_is_connected = false;
        control_is_connected = false;
    }

    void queue(std::string command)
    {
        {
            std::lock_guard<std::mutex> lock(handoff_mutex);
            while(pending_commands.size() >= kMaxPendingCommands) pending_commands.pop_front();
            pending_commands.push_back(std::move(command));
        }
        std::lock_guard<std::mutex> lock(context_mutex);
        if(context != nullptr) lws_cancel_service(context);
    }
};

LongmyndClient::LongmyndClient() : impl_(std::make_unique<Impl>()) {}
LongmyndClient::~LongmyndClient() { stop(); }

void LongmyndClient::start()
{
    if(impl_->running.exchange(true)) return;
    impl_->thread = std::thread([this] { impl_->run(); });
}

void LongmyndClient::stop()
{
    if(!impl_->running.exchange(false)) return;
    {
        std::lock_guard<std::mutex> lock(impl_->context_mutex);
        if(impl_->context != nullptr) lws_cancel_service(impl_->context);
    }
    if(impl_->thread.joinable()) impl_->thread.join();
}

bool LongmyndClient::consume_status(ReceiverStatus & status)
{
    std::string json;
    {
        std::lock_guard<std::mutex> lock(impl_->handoff_mutex);
        json.swap(impl_->pending_json);
    }
    return !json.empty() && parse_monitor_json(json, status);
}

void LongmyndClient::send_tune(long frequency_khz, long symbol_rate_ksps)
{
    impl_->queue("C" + std::to_string(frequency_khz) + "," +
                 std::to_string(symbol_rate_ksps));
}

void LongmyndClient::send_voltage(bool enabled, bool horizontal)
{
    impl_->queue("V" + std::to_string(enabled ? 1 : 0) + "," +
                 std::to_string(horizontal ? 1 : 0));
}

bool LongmyndClient::monitor_connected() const { return impl_->monitor_is_connected.load(); }
bool LongmyndClient::control_connected() const { return impl_->control_is_connected.load(); }
uint64_t LongmyndClient::received_updates() const { return impl_->received.load(); }
uint64_t LongmyndClient::replaced_updates() const { return impl_->replaced.load(); }

} // namespace qo100
