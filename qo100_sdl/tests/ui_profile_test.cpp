/* Unit test for the screen-size profile table (src/ui_profile.h): which row
 * a screen of a given size gets, and that the table itself is consistent.
 *
 * Build and run (from qo100_sdl/):
 *   g++ -std=c++17 -Wall -Wextra -Isrc tests/ui_profile_test.cpp -o /tmp/ui_profile_test && /tmp/ui_profile_test
 */
#include "ui_profile.h"

#include <cstdio>
#include <cstring>

namespace {

int failures = 0;

void check(bool condition, const char * what)
{
    std::printf("  [%s] %s\n", condition ? "ok" : "FAIL", what);
    if(!condition) ++failures;
}

bool picks(int width, int height, const char * expected)
{
    return std::strcmp(qo100::ui_profile_for(width, height).name, expected) == 0;
}

}  // namespace

int main()
{
    std::printf("profile selection\n");
    check(picks(800, 480, "800x480"), "800x480 screen gets its own row");
    check(picks(1024, 600, "1024x600"), "1024x600 screen gets its own row");
    check(picks(1280, 720, "1024x600"), "bigger screen gets the largest row that fits");
    check(picks(1024, 768, "1024x600"), "taller screen gets the row that fits");
    check(picks(1023, 600, "800x480"), "one pixel too narrow falls back a row");
    check(picks(640, 480, "800x480"), "smaller than every row gets the smallest");

    std::printf("table\n");
    bool names_match = true;
    for(const qo100::UiProfile & profile : qo100::kUiProfiles) {
        char expected[32];
        std::snprintf(expected, sizeof(expected), "%dx%d", profile.width, profile.height);
        if(std::strcmp(profile.name, expected) != 0) names_match = false;
    }
    check(names_match, "every row's name is its size");
    bool sizes_unique = true;
    for(size_t i = 0; i < qo100::kUiProfileCount; ++i)
        for(size_t j = i + 1; j < qo100::kUiProfileCount; ++j)
            if(qo100::kUiProfiles[i].width == qo100::kUiProfiles[j].width &&
               qo100::kUiProfiles[i].height == qo100::kUiProfiles[j].height)
                sizes_unique = false;
    check(sizes_unique, "no two rows for the same size");

    std::printf(failures == 0 ? "ALL PASSED\n" : "%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
