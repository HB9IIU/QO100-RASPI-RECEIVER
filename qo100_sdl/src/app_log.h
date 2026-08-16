#pragma once

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace qo100 {

inline std::chrono::steady_clock::time_point & log_epoch()
{
    static auto epoch = std::chrono::steady_clock::now();
    return epoch;
}

inline std::mutex & log_mutex()
{
    static std::mutex mutex;
    return mutex;
}

inline void reset_log_clock()
{
    std::lock_guard<std::mutex> lock(log_mutex());
    log_epoch() = std::chrono::steady_clock::now();
}

inline void log(const char * format, ...)
{
    std::lock_guard<std::mutex> lock(log_mutex());
    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - log_epoch()).count();
    std::fprintf(stderr, "[+%08.3fs] ", elapsed);
    va_list arguments;
    va_start(arguments, format);
    std::vfprintf(stderr, format, arguments);
    va_end(arguments);
    std::fflush(stderr);
}

} // namespace qo100
