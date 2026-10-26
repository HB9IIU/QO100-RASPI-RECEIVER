#pragma once

#include <memory>
#include <string>

namespace qo100 {

/* Serves the transport stream over HTTP, for VLC on another computer
 * (Media > Open Network Stream, http://<this Pi>:8091/). The stream itself
 * is the multicast that longmynd sends (ts_address.h); a home router or Wi-Fi
 * often doesn't pass multicast on to other devices, while a plain TCP
 * connection goes through anywhere. The multicast group is only joined while
 * at least one viewer is connected. In local RTL-SDR mode the stream is
 * unicast to 127.0.0.1, which only one socket can receive - the app's own
 * decoder - so nothing is served then. */
constexpr int kHttpStreamPort = 8091;

class HttpStreamServer {
public:
    HttpStreamServer();
    ~HttpStreamServer();
    HttpStreamServer(const HttpStreamServer &) = delete;
    HttpStreamServer & operator=(const HttpStreamServer &) = delete;

    void start();
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/* This Pi's IPv4 address on the LAN (the one other computers reach it on),
 * "" if it has none. Cached for a few seconds - cheap to call every frame. */
std::string lan_ipv4_address();

} // namespace qo100
