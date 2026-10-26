#include "http_stream.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app_log.h"
#include "ts_address.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace qo100 {

namespace {

/* A viewer more than this far behind (a stalled or very slow connection) is
 * dropped rather than buffered without limit - at the ~2-4 Mbit/s of a
 * QO-100 stream this is several seconds. */
constexpr size_t kMaxPendingBytes = 4 * 1024 * 1024;

constexpr char kResponseHeader[] =
    "HTTP/1.0 200 OK\r\n"
    "Content-Type: video/mp2t\r\n"
    "Cache-Control: no-cache\r\n"
    "Connection: close\r\n"
    "\r\n";

struct Viewer {
    int fd = -1;
    std::string pending;
};

void set_nonblocking(int fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    if(flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

bool is_multicast(const std::string & address)
{
    in_addr parsed{};
    return inet_pton(AF_INET, address.c_str(), &parsed) == 1 &&
           IN_MULTICAST(ntohl(parsed.s_addr));
}

/* Joins the stream's multicast group; -1 if that fails. SO_REUSEADDR so it
 * shares the port with the app's own decoder - each gets every datagram. */
int open_stream_socket(const std::string & group, int port)
{
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if(fd < 0) return -1;
    const int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, group.c_str(), &address.sin_addr);
    ip_mreq membership{};
    membership.imr_multiaddr = address.sin_addr;
    membership.imr_interface.s_addr = htonl(INADDR_ANY);
    if(bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0 ||
       setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership)) < 0) {
        close(fd);
        return -1;
    }
    set_nonblocking(fd);
    return fd;
}

/* Sends as much of the viewer's pending data as the socket takes. false =
 * the viewer is gone (or hopelessly behind) and should be dropped. */
bool flush_viewer(Viewer & viewer)
{
    while(!viewer.pending.empty()) {
        const ssize_t sent = send(viewer.fd, viewer.pending.data(), viewer.pending.size(),
                                  MSG_NOSIGNAL | MSG_DONTWAIT);
        if(sent > 0) {
            viewer.pending.erase(0, static_cast<size_t>(sent));
            continue;
        }
        if(sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        return false;
    }
    return viewer.pending.size() <= kMaxPendingBytes;
}

} // namespace

struct HttpStreamServer::Impl {
    std::atomic<bool> running{false};
    std::thread thread;

    void run()
    {
        /* SOCK_CLOEXEC throughout: the app starts longmynd, which must not
         * inherit these and keep the port open after the app has gone. */
        const int listener = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if(listener < 0) {
            qo100::log("[HTTP_TS] socket failed: %s\n", std::strerror(errno));
            return;
        }
        const int on = 1;
        const int off = 0;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        setsockopt(listener, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
        sockaddr_in6 address{};
        address.sin6_family = AF_INET6;
        address.sin6_port = htons(kHttpStreamPort);
        address.sin6_addr = in6addr_any;
        if(bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0 ||
           listen(listener, 8) < 0) {
            qo100::log("[HTTP_TS] port %d unavailable: %s\n", kHttpStreamPort,
                       std::strerror(errno));
            close(listener);
            return;
        }
        set_nonblocking(listener);
        qo100::log("[HTTP_TS] serving on port %d\n", kHttpStreamPort);

        std::vector<Viewer> viewers;
        int stream = -1;
        std::vector<char> datagram(65536);
        char discard[1024];

        while(running.load()) {
            /* Join the group only while someone watches, and leave it when
             * the stream stops being multicast (local RTL-SDR mode). */
            const std::string group = ts_stream_address();
            const bool servable = is_multicast(group);
            if(!servable && !viewers.empty()) {
                for(Viewer & viewer : viewers) close(viewer.fd);
                viewers.clear();
            }
            if(stream < 0 && servable && !viewers.empty()) {
                stream = open_stream_socket(group, ts_stream_port());
                if(stream < 0)
                    qo100::log("[HTTP_TS] can't join %s: %s\n", group.c_str(),
                               std::strerror(errno));
            }
            else if(stream >= 0 && viewers.empty()) {
                close(stream);
                stream = -1;
            }

            std::vector<pollfd> fds;
            fds.push_back({listener, POLLIN, 0});
            if(stream >= 0) fds.push_back({stream, POLLIN, 0});
            const size_t first_viewer = fds.size();
            for(const Viewer & viewer : viewers)
                fds.push_back({viewer.fd, static_cast<short>(
                    POLLIN | (viewer.pending.empty() ? 0 : POLLOUT)), 0});
            if(poll(fds.data(), fds.size(), 250) < 0 && errno != EINTR) break;

            if(fds[0].revents & POLLIN) {
                sockaddr_storage peer{};
                socklen_t peer_length = sizeof(peer);
                const int client = accept4(listener, reinterpret_cast<sockaddr *>(&peer),
                                           &peer_length, SOCK_CLOEXEC);
                if(client >= 0) {
                    if(servable) {
                        set_nonblocking(client);
                        viewers.push_back({client, kResponseHeader});
                        qo100::log("[HTTP_TS] viewer connected (%zu watching)\n",
                                   viewers.size());
                    }
                    else {
                        close(client);
                    }
                }
            }

            if(stream >= 0 && (fds[1].revents & POLLIN)) {
                /* Drain everything waiting, as one append per viewer. */
                std::string chunk;
                while(true) {
                    const ssize_t length = recv(stream, datagram.data(), datagram.size(), 0);
                    if(length <= 0) break;
                    chunk.append(datagram.data(), static_cast<size_t>(length));
                }
                for(Viewer & viewer : viewers) viewer.pending += chunk;
            }

            /* Viewers only ever send their request; read and ignore it, and
             * notice when they hang up. */
            std::vector<bool> drop(viewers.size(), false);
            for(size_t index = 0; index < viewers.size(); ++index) {
                const pollfd & entry = fds[first_viewer + index];
                if(entry.revents & (POLLERR | POLLHUP | POLLNVAL)) drop[index] = true;
                else if(entry.revents & POLLIN) {
                    const ssize_t length = recv(viewers[index].fd, discard, sizeof(discard), 0);
                    if(length == 0 || (length < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
                        drop[index] = true;
                }
            }
            for(size_t index = 0; index < viewers.size(); ++index)
                if(!drop[index] && !flush_viewer(viewers[index])) drop[index] = true;
            size_t kept = 0;
            for(size_t index = 0; index < viewers.size(); ++index) {
                if(drop[index]) close(viewers[index].fd);
                else viewers[kept++] = std::move(viewers[index]);
            }
            if(kept != viewers.size()) {
                viewers.resize(kept);
                qo100::log("[HTTP_TS] viewer left (%zu watching)\n", viewers.size());
            }
        }

        for(Viewer & viewer : viewers) close(viewer.fd);
        if(stream >= 0) close(stream);
        close(listener);
    }
};

HttpStreamServer::HttpStreamServer() : impl_(std::make_unique<Impl>()) {}

HttpStreamServer::~HttpStreamServer() { stop(); }

void HttpStreamServer::start()
{
    if(impl_->running.exchange(true)) return;
    impl_->thread = std::thread([this] { impl_->run(); });
}

void HttpStreamServer::stop()
{
    if(!impl_->running.exchange(false)) return;
    if(impl_->thread.joinable()) impl_->thread.join();
}

std::string lan_ipv4_address()
{
    static std::mutex mutex;
    static std::string cached;
    static std::chrono::steady_clock::time_point checked_at{};
    std::lock_guard<std::mutex> lock(mutex);
    const auto now = std::chrono::steady_clock::now();
    if(checked_at.time_since_epoch().count() != 0 &&
       now - checked_at < std::chrono::seconds(5))
        return cached;
    checked_at = now;

    /* The address of the interface with the default route - what a
     * "connect" to an outside address would use (a UDP connect sends
     * nothing). Without a default route (field use, no internet), the first
     * non-loopback interface that is up. */
    std::string found;
    const int probe = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if(probe >= 0) {
        sockaddr_in outside{};
        outside.sin_family = AF_INET;
        outside.sin_port = htons(53);
        inet_pton(AF_INET, "1.1.1.1", &outside.sin_addr);
        sockaddr_in local{};
        socklen_t length = sizeof(local);
        char text[INET_ADDRSTRLEN]{};
        if(connect(probe, reinterpret_cast<sockaddr *>(&outside), sizeof(outside)) == 0 &&
           getsockname(probe, reinterpret_cast<sockaddr *>(&local), &length) == 0 &&
           local.sin_addr.s_addr != htonl(INADDR_ANY) &&
           inet_ntop(AF_INET, &local.sin_addr, text, sizeof(text)) != nullptr)
            found = text;
        close(probe);
    }
    if(found.empty()) {
        ifaddrs * interfaces = nullptr;
        if(getifaddrs(&interfaces) == 0) {
            for(const ifaddrs * entry = interfaces; entry != nullptr; entry = entry->ifa_next) {
                if(entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_INET) continue;
                if((entry->ifa_flags & IFF_LOOPBACK) || !(entry->ifa_flags & IFF_UP)) continue;
                char text[INET_ADDRSTRLEN]{};
                const auto * address = reinterpret_cast<const sockaddr_in *>(entry->ifa_addr);
                if(inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text)) != nullptr) {
                    found = text;
                    break;
                }
            }
            freeifaddrs(interfaces);
        }
    }
    cached = found;
    return cached;
}

} // namespace qo100
