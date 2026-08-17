#include "chat_client.h"

#include <libwebsockets.h>
#include <json-c/json.h>
#ifndef LWS_PROTOCOL_LIST_TERM
#define LWS_PROTOCOL_LIST_TERM {nullptr, nullptr, 0, 0, 0, nullptr, 0}
#endif

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "app_log.h"

namespace qo100 {
namespace {

using Clock = std::chrono::steady_clock;

constexpr const char * kChatHost = "eshail.batc.org.uk";
constexpr const char * kChatPath =
    "/wb/chat/socket.io/?EIO=4&transport=websocket&room=eshail-wb";

std::string json_string(json_object * object, const char * key)
{
    json_object * value = nullptr;
    return object != nullptr && json_object_object_get_ex(object, key, &value) &&
           json_object_is_type(value, json_type_string)
        ? json_object_get_string(value) : "";
}

std::string event_packet(const char * event, const char * key,
                         const std::string & value)
{
    json_object * object = json_object_new_object();
    json_object_object_add(object, key, json_object_new_string(value.c_str()));
    std::string packet = "42[\"" + std::string(event) + "\",";
    packet += json_object_to_json_string_ext(object, JSON_C_TO_STRING_PLAIN);
    packet += "]";
    json_object_put(object);
    return packet;
}

} // namespace

struct ChatClient::Impl {
    std::atomic<bool> running{false};
    std::thread thread;
    /* Guards `context` against a use-after-free race: run() destroys the
     * context right after its loop exits, while stop()/queue_outgoing()
     * (called from the main thread) read `context` and call
     * lws_cancel_service() on it to wake the loop up early. A plain atomic
     * pointer only makes the *read* safe, not the use of what it points
     * to - see the identical fix and longer explanation in receiver.cpp's
     * LongmyndClient::Impl, where this exact race was caught causing a
     * libwebsockets internal assertion failure (SIGABRT) during EXIT's
     * shutdown. */
    std::mutex context_mutex;
    lws_context * context = nullptr;
    lws * websocket = nullptr;
    std::mutex mutex;
    std::vector<std::string> incoming;
    std::deque<std::string> outgoing;
    std::string receive_buffer;
    ChatState state;

    static int callback(lws * websocket, lws_callback_reasons reason,
                        void *, void * data, size_t length)
    {
        auto * self = static_cast<Impl *>(
            lws_context_user(lws_get_context(websocket)));
        return self != nullptr ? self->on_event(websocket, reason, data, length) : 0;
    }

    int on_event(lws * active_websocket, lws_callback_reasons reason,
                 void * data, size_t length)
    {
        switch(reason) {
            case LWS_CALLBACK_CLIENT_ESTABLISHED:
                websocket = active_websocket;
                qo100::log("[CHAT] websocket established\n");
                break;
            case LWS_CALLBACK_CLIENT_RECEIVE:
                if(lws_is_first_fragment(active_websocket)) receive_buffer.clear();
                receive_buffer.append(static_cast<const char *>(data), length);
                if(lws_is_final_fragment(active_websocket) &&
                   lws_remaining_packet_payload(active_websocket) == 0U) {
                    std::lock_guard<std::mutex> lock(mutex);
                    incoming.push_back(std::move(receive_buffer));
                    receive_buffer.clear();
                }
                break;
            case LWS_CALLBACK_CLIENT_WRITEABLE: {
                std::string packet;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if(!outgoing.empty()) {
                        packet = std::move(outgoing.front());
                        outgoing.pop_front();
                    }
                }
                if(!packet.empty()) {
                    std::vector<unsigned char> buffer(LWS_PRE + packet.size());
                    std::memcpy(buffer.data() + LWS_PRE, packet.data(), packet.size());
                    lws_write(active_websocket, buffer.data() + LWS_PRE,
                              packet.size(), LWS_WRITE_TEXT);
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if(!outgoing.empty()) lws_callback_on_writable(active_websocket);
                }
                break;
            }
            case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
                if(websocket != nullptr) lws_callback_on_writable(websocket);
                break;
            case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
                qo100::log("[CHAT] connection error: %.*s\n",
                    static_cast<int>(length),
                    data != nullptr ? static_cast<const char *>(data) : "");
                websocket = nullptr;
                queue_incoming("__DISCONNECTED__");
                break;
            case LWS_CALLBACK_CLIENT_CLOSED:
                qo100::log("[CHAT] disconnected\n");
                websocket = nullptr;
                queue_incoming("__DISCONNECTED__");
                break;
            default:
                break;
        }
        return 0;
    }

    void queue_incoming(std::string packet)
    {
        std::lock_guard<std::mutex> lock(mutex);
        incoming.push_back(std::move(packet));
    }

    void queue_outgoing(std::string packet)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            outgoing.push_back(std::move(packet));
        }
        std::lock_guard<std::mutex> lock(context_mutex);
        if(context != nullptr) lws_cancel_service(context);
    }

    void connect(lws_context * active_context, const char * protocol)
    {
        lws_client_connect_info info{};
        info.context = active_context;
        info.address = kChatHost;
        info.port = 443;
        info.path = kChatPath;
        info.host = kChatHost;
        info.origin = "https://eshail.batc.org.uk";
        info.ssl_connection = LCCSCF_USE_SSL;
        info.local_protocol_name = protocol;
        info.protocol = protocol;
        info.alpn = "http/1.1";
        websocket = lws_client_connect_via_info(&info);
    }

    void run()
    {
        static const lws_protocols protocols[] = {
            {"qo100_chat", &Impl::callback, 0, 128 * 1024, 0, nullptr, 0},
            LWS_PROTOCOL_LIST_TERM
        };
        lws_context_creation_info info{};
        info.port = CONTEXT_PORT_NO_LISTEN;
        info.protocols = protocols;
        info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
        info.user = this;
        lws_context * active_context = lws_create_context(&info);
        if(active_context == nullptr) {
            queue_incoming("__DISCONNECTED__");
            return;
        }
        {
            std::lock_guard<std::mutex> lock(context_mutex);
            context = active_context;
        }
        connect(active_context, protocols[0].name);
        auto last_attempt = Clock::now();
        while(running.load(std::memory_order_relaxed)) {
            lws_service(active_context, 50);
            const auto now = Clock::now();
            if(websocket == nullptr && now - last_attempt > std::chrono::seconds(3)) {
                last_attempt = now;
                connect(active_context, protocols[0].name);
            }
        }
        {
            std::lock_guard<std::mutex> lock(context_mutex);
            context = nullptr;
        }
        lws_context_destroy(active_context);
        websocket = nullptr;
    }

    void append_line(json_object * item)
    {
        std::string time = json_string(item, "time");
        if(time.size() >= 16U && time[10] == 'T') time = time.substr(11, 5);
        else if(time.size() > 5U) time = time.substr(0, 5);
        state.lines.push_back({time, json_string(item, "name"),
                              json_string(item, "message")});
        if(state.lines.size() > 500U) state.lines.pop_front();
    }

    void update_users(json_object * object)
    {
        json_object * nicks = nullptr;
        if(!json_object_object_get_ex(object, "nicks", &nicks) ||
           !json_object_is_type(nicks, json_type_array)) return;
        state.users.clear();
        const size_t count = json_object_array_length(nicks);
        state.users.reserve(count);
        for(size_t index = 0; index < count; ++index) {
            json_object * nick = json_object_array_get_idx(nicks, index);
            state.users.emplace_back(json_object_get_string(nick));
        }
    }

    void process_packet(const std::string & packet)
    {
        if(packet == "__DISCONNECTED__") {
            state.connection = ChatState::Connection::Reconnecting;
            return;
        }
        if(!packet.empty() && packet[0] == '0') {
            queue_outgoing("40");
            return;
        }
        if(packet == "2") {
            queue_outgoing("3");
            return;
        }
        if(packet.rfind("40", 0) == 0) {
            state.connection = ChatState::Connection::Connected;
            qo100::log("[CHAT] connected to QO-100 wideband chat\n");
            return;
        }
        if(packet.rfind("42", 0) != 0) return;

        json_object * root = json_tokener_parse(packet.c_str() + 2);
        if(root == nullptr || !json_object_is_type(root, json_type_array) ||
           json_object_array_length(root) < 2U) {
            if(root != nullptr) json_object_put(root);
            return;
        }
        const char * event =
            json_object_get_string(json_object_array_get_idx(root, 0));
        json_object * payload = json_object_array_get_idx(root, 1);
        if(std::strcmp(event, "history") == 0) {
            state.lines.clear();
            json_object * history = nullptr;
            if(json_object_object_get_ex(payload, "history", &history) &&
               json_object_is_type(history, json_type_array)) {
                const size_t count = json_object_array_length(history);
                for(size_t index = 0; index < count; ++index)
                    append_line(json_object_array_get_idx(history, index));
            }
            update_users(payload);
        }
        else if(std::strcmp(event, "message") == 0) append_line(payload);
        else if(std::strcmp(event, "nicks") == 0) update_users(payload);
        else if(std::strcmp(event, "viewers") == 0) {
            json_object * number = nullptr;
            if(json_object_object_get_ex(payload, "num", &number))
                state.viewers = json_object_get_string(number);
        }
        json_object_put(root);
    }

    bool consume()
    {
        std::vector<std::string> packets;
        {
            std::lock_guard<std::mutex> lock(mutex);
            packets.swap(incoming);
        }
        for(const std::string & packet : packets) process_packet(packet);
        return !packets.empty();
    }
};

ChatClient::ChatClient() : impl_(std::make_unique<Impl>()) {}
ChatClient::~ChatClient() { stop(); }

void ChatClient::start()
{
    if(impl_->running.exchange(true)) return;
    impl_->thread = std::thread([this] { impl_->run(); });
}

void ChatClient::stop()
{
    if(!impl_->running.exchange(false)) return;
    {
        std::lock_guard<std::mutex> lock(impl_->context_mutex);
        if(impl_->context != nullptr) lws_cancel_service(impl_->context);
    }
    if(impl_->thread.joinable()) impl_->thread.join();
}

bool ChatClient::consume() { return impl_->consume(); }

void ChatClient::set_nick(const std::string & nick)
{
    if(!nick.empty()) impl_->queue_outgoing(event_packet("setnick", "nick", nick));
}

void ChatClient::send_message(const std::string & message)
{
    if(!message.empty())
        impl_->queue_outgoing(event_packet("message", "message", message));
}

const ChatState & ChatClient::state() const { return impl_->state; }

} // namespace qo100
