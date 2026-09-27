#pragma once

/* Where the MiniTiouner's transport stream goes: longmynd sends it there as
 * UDP (receiver.cpp) and the app's video decoder reads it from there
 * (video_decoder.cpp), so both ends take it from here.
 *
 * It is a multicast group, so another device on the LAN can watch the same
 * feed in VLC (udp://@<address>:<port>, shown on the SET page). Every Pi
 * gets a group of its own, 239.1.x.y with x and y worked out from the board's
 * serial number: with one shared group (239.1.1.1, as it used to be), two
 * Pis on one network each received the other's stream as well - a mixed-up
 * picture, and video on a Pi with no MiniTiouner at all.
 *
 * The serial number is the board's own, so the group stays the same across
 * reboots and reinstalls, and a cloned SD card doesn't copy it to another
 * Pi. Off a Pi (no serial number) /etc/machine-id stands in; with neither,
 * the old 239.1.1.1.
 *
 * QO100_TS_ADDR / QO100_TS_PORT override the address and port - read on
 * every call, as main() switches to 127.0.0.1 at runtime for the local
 * RTL-SDR spectrum source. */

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>

namespace qo100 {

/* 239.1.x.y for a device id, x and y in 1..254 - a pure function of the
 * id (FNV-1a hash), about 64,500 possible groups. */
inline std::string ts_multicast_address_for_id(const std::string & id)
{
    uint32_t hash = 2166136261U;
    for(const unsigned char c : id) {
        hash ^= c;
        hash *= 16777619U;
    }
    const unsigned x = 1U + (hash >> 16) % 254U;
    const unsigned y = 1U + (hash & 0xffffU) % 254U;
    return "239.1." + std::to_string(x) + "." + std::to_string(y);
}

/* The first line of a file, without trailing NULs/newlines ("" if none). */
inline std::string ts_read_id_file(const char * path)
{
    std::ifstream file(path);
    std::string id;
    std::getline(file, id);
    while(!id.empty() && (id.back() == '\0' || id.back() == '\n' || id.back() == ' '))
        id.pop_back();
    return id;
}

inline std::string ts_stream_address()
{
    if(const char * value = std::getenv("QO100_TS_ADDR")) return value;
    static const std::string derived = [] {
        std::string id = ts_read_id_file("/sys/firmware/devicetree/base/serial-number");
        if(id.empty()) id = ts_read_id_file("/etc/machine-id");
        return id.empty() ? std::string("239.1.1.1") : ts_multicast_address_for_id(id);
    }();
    return derived;
}

inline int ts_stream_port()
{
    const char * value = std::getenv("QO100_TS_PORT");
    return value != nullptr ? std::atoi(value) : 5600;
}

}  // namespace qo100
