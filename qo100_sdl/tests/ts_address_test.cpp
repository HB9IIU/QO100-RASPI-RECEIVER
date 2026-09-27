/* Unit test for the transport stream's multicast group (src/ts_address.h):
 * one group per device, always a valid 239.1.x.y, stable for a given id.
 *
 * Build and run (from qo100_sdl/):
 *   g++ -std=c++17 -Wall -Wextra -Isrc tests/ts_address_test.cpp -o /tmp/ts_address_test && /tmp/ts_address_test
 */
#include "ts_address.h"

#include <cstdio>
#include <set>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const char * what)
{
    std::printf("  [%s] %s\n", condition ? "ok" : "FAIL", what);
    if(!condition) ++failures;
}

/* x and y of "239.1.x.y" both in 1..254. */
bool valid_group(const std::string & address)
{
    unsigned a = 0, b = 0, x = 0, y = 0;
    char extra = 0;
    if(std::sscanf(address.c_str(), "%u.%u.%u.%u%c", &a, &b, &x, &y, &extra) != 4) return false;
    return a == 239 && b == 1 && x >= 1 && x <= 254 && y >= 1 && y <= 254;
}

}  // namespace

int main()
{
    using qo100::ts_multicast_address_for_id;
    std::printf("multicast group per device\n");
    const std::string pi_a = ts_multicast_address_for_id("7ff7ddfa0dd7f222");
    const std::string pi_b = ts_multicast_address_for_id("7ff7ddfa0dd7f223");
    std::printf("  (7ff7ddfa0dd7f222 -> %s, ...223 -> %s)\n", pi_a.c_str(), pi_b.c_str());
    check(valid_group(pi_a) && valid_group(pi_b), "a valid 239.1.x.y group");
    check(pi_a == ts_multicast_address_for_id("7ff7ddfa0dd7f222"), "same id, same group");
    check(pi_a != pi_b, "serial numbers one apart get different groups");

    /* 1000 made-up serial numbers: all valid, and nearly all distinct
     * (about 64,500 groups, so a handful of shared ones is expected). */
    std::set<std::string> groups;
    bool all_valid = true;
    for(int i = 0; i < 1000; ++i) {
        char serial[32];
        std::snprintf(serial, sizeof(serial), "10000000%08x", 0x3a5c0000U + i * 7919U);
        const std::string group = ts_multicast_address_for_id(serial);
        all_valid = all_valid && valid_group(group);
        groups.insert(group);
    }
    std::printf("  (1000 serials -> %zu distinct groups)\n", groups.size());
    check(all_valid, "1000 serials all give valid groups");
    check(groups.size() >= 980, "1000 serials spread over (nearly) as many groups");

    std::printf(failures == 0 ? "ALL PASSED\n" : "%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
