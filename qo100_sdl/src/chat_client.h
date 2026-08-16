#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace qo100 {

struct ChatLine {
    std::string time;
    std::string name;
    std::string message;
};

struct ChatState {
    enum class Connection { Connecting, Connected, Reconnecting };

    Connection connection = Connection::Connecting;
    std::string viewers = "--";
    std::deque<ChatLine> lines;
    std::vector<std::string> users;
};

class ChatClient {
public:
    ChatClient();
    ~ChatClient();

    ChatClient(const ChatClient &) = delete;
    ChatClient & operator=(const ChatClient &) = delete;

    void start();
    void stop();
    bool consume();
    void set_nick(const std::string & nick);
    void send_message(const std::string & message);
    const ChatState & state() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qo100
