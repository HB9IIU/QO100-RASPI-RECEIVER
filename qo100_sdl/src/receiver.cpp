#include "receiver.h"

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

namespace qo100 {
namespace {

constexpr int kWebsocketPort = 8765;
constexpr auto kReconnectInterval = std::chrono::milliseconds(1000);
constexpr size_t kMaxPendingCommands = 16;

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

ReceiverSettings load_receiver_settings(const std::string & repository_root)
{
    ReceiverSettings settings;
    const std::string path = repository_root + "/qo100_lvgl/settings.json";
    json_object * root = json_object_from_file(path.c_str());
    if(root == nullptr) {
        std::fprintf(stderr, "[SETTINGS] using defaults; could not read %s\n", path.c_str());
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
    json_object_put(root);
    std::fprintf(stderr, "[SETTINGS] LO=%.1fMHz voltage=%s/%s volume=%d%%\n",
        settings.lnb_lo_mhz, settings.lnb_voltage_enabled ? "on" : "off",
        settings.lnb_voltage_horizontal ? "18V" : "13V", settings.audio_volume_percent);
    return settings;
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
        std::fprintf(stderr, "[LONGMYND] binary is not executable: %s\n", binary_.c_str());
        return false;
    }

    FILE * stale_file = std::fopen(pid_path_.c_str(), "r");
    if(stale_file != nullptr) {
        int stale_pid = -1;
        const bool read_pid = std::fscanf(stale_file, "%d", &stale_pid) == 1;
        std::fclose(stale_file);
        if(read_pid && stale_pid > 0 && pid_matches_binary(stale_pid, binary_)) {
            std::fprintf(stderr, "[LONGMYND] stopping stale owned process %d\n", stale_pid);
            kill(stale_pid, SIGTERM);
        }
        std::remove(pid_path_.c_str());
    }

    const pid_t child = fork();
    if(child < 0) {
        std::fprintf(stderr, "[LONGMYND] fork failed: %s\n", std::strerror(errno));
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
        std::snprintf(udp_port, sizeof(udp_port), "%d", 5600);
        std::snprintf(frequency, sizeof(frequency), "%ld", frequency_khz);
        std::snprintf(symbol_rate, sizeof(symbol_rate), "%ld", symbol_rate_ksps);
        execl("/usr/bin/stdbuf", "stdbuf", "-oL", "-eL", binary_.c_str(),
              "-W", port, "-i", "127.0.0.1", udp_port,
              frequency, symbol_rate, static_cast<char *>(nullptr));
        _exit(127);
    }

    pid_ = static_cast<int>(child);
    FILE * pid_file = std::fopen(pid_path_.c_str(), "w");
    if(pid_file != nullptr) {
        std::fprintf(pid_file, "%d\n", pid_);
        std::fclose(pid_file);
    }
    std::fprintf(stderr, "[LONGMYND] started pid=%d frequency=%ldkHz sr=%ldkS/s\n",
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
            std::fprintf(stderr, "[LONGMYND] stopped pid=%d\n", child);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::fprintf(stderr, "[LONGMYND] pid=%d did not stop in time; terminating\n", child);
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
    std::atomic<lws_context *> context{nullptr};
    lws * monitor_wsi = nullptr;
    lws * control_wsi = nullptr;
    std::atomic<bool> monitor_is_connected{false};
    std::atomic<bool> control_is_connected{false};

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
        (void)data;
        (void)length;
        Impl * self = from(websocket);
        return self != nullptr ? self->on_control(websocket, reason) : 0;
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
                std::fprintf(stderr, "[LONGMYND-WS] monitor connected\n");
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
            case LWS_CALLBACK_CLIENT_CLOSED:
                if(monitor_wsi == websocket) monitor_wsi = nullptr;
                monitor_is_connected = false;
                break;
            default:
                break;
        }
        return 0;
    }

    int on_control(lws * websocket, lws_callback_reasons reason)
    {
        switch(reason) {
            case LWS_CALLBACK_CLIENT_ESTABLISHED: {
                control_wsi = websocket;
                control_is_connected = true;
                std::fprintf(stderr, "[LONGMYND-WS] control connected\n");
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
                    if(written < 0) std::fprintf(stderr, "[LONGMYND-WS] control write failed\n");
                    else std::fprintf(stderr, "[LONGMYND-WS] sent %s\n", command.c_str());
                }
                if(more) lws_callback_on_writable(websocket);
                break;
            }
            case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
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
        lws_client_connect_info info{};
        info.context = active_context;
        info.address = "127.0.0.1";
        info.port = kWebsocketPort;
        info.path = "/";
        info.host = info.address;
        info.origin = info.address;
        info.local_protocol_name = "monitor";
        info.protocol = "monitor";
        lws_client_connect_via_info(&info);
    }

    void connect_control(lws_context * active_context)
    {
        lws_client_connect_info info{};
        info.context = active_context;
        info.address = "127.0.0.1";
        info.port = kWebsocketPort;
        info.path = "/";
        info.host = info.address;
        info.origin = info.address;
        info.local_protocol_name = "control";
        info.protocol = "control";
        lws_client_connect_via_info(&info);
    }

    void run()
    {
        lws_context_creation_info info{};
        info.port = CONTEXT_PORT_NO_LISTEN;
        info.protocols = protocols();
        info.user = this;
        lws_context * active_context = lws_create_context(&info);
        if(active_context == nullptr) return;
        context = active_context;
        connect_monitor(active_context);
        connect_control(active_context);
        auto last_attempt = std::chrono::steady_clock::now();
        while(running.load(std::memory_order_relaxed)) {
            lws_service(active_context, 50);
            const auto now = std::chrono::steady_clock::now();
            if(now - last_attempt >= kReconnectInterval) {
                last_attempt = now;
                if(monitor_wsi == nullptr) connect_monitor(active_context);
                if(control_wsi == nullptr) connect_control(active_context);
            }
            bool command_waiting = false;
            {
                std::lock_guard<std::mutex> lock(handoff_mutex);
                command_waiting = !pending_commands.empty();
            }
            if(command_waiting && control_wsi != nullptr)
                lws_callback_on_writable(control_wsi);
        }
        context = nullptr;
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
        if(lws_context * active_context = context.load()) lws_cancel_service(active_context);
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
    if(lws_context * active_context = impl_->context.load()) lws_cancel_service(active_context);
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
