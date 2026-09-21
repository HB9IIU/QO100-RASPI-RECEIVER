#pragma once

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

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

inline std::FILE *& log_file()
{
    static std::FILE * file = nullptr;
    return file;
}

/* Also writes every log line to <directory>/qo100_<date>_<time>.log (and points
 * <directory>/latest.log at it), so a run started by the service, with no
 * terminal, still leaves a log. Only the newest `keep` files are kept. */
struct LogFileState {
    std::string directory;
    size_t keep = 10;
    size_t written = 0;
};

inline LogFileState & log_file_state()
{
    static LogFileState state;
    return state;
}

/* A run that goes on for days must not grow one file without bound: past this
 * size the log continues in a fresh file (the retention below still applies). */
constexpr size_t kLogFileMaxBytes = 20u * 1024u * 1024u;

/* Caller holds log_mutex(). */
inline void open_log_file_locked(const std::string & directory, size_t keep)
{
    if(log_file() != nullptr) { std::fclose(log_file()); log_file() = nullptr; }
    log_file_state() = {directory, keep, 0};
    mkdir(directory.c_str(), 0755);
    std::vector<std::string> old_logs;
    if(DIR * dir = opendir(directory.c_str())) {
        while(const dirent * entry = readdir(dir)) {
            const std::string name = entry->d_name;
            if(name.rfind("qo100_", 0) == 0 && name.size() > 4 &&
               name.compare(name.size() - 4, 4, ".log") == 0)
                old_logs.push_back(name);
        }
        closedir(dir);
    }
    std::sort(old_logs.begin(), old_logs.end());
    while(old_logs.size() >= keep) {
        std::remove((directory + "/" + old_logs.front()).c_str());
        old_logs.erase(old_logs.begin());
    }
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &local);
    const std::string path = directory + "/qo100_" + stamp + ".log";
    log_file() = std::fopen(path.c_str(), "w");
    if(log_file() != nullptr) {
        const std::string link = directory + "/latest.log";
        unlink(link.c_str());
        if(symlink(("qo100_" + std::string(stamp) + ".log").c_str(), link.c_str()) != 0) {}
    }
}

inline void open_log_file(const std::string & directory, size_t keep = 10)
{
    std::lock_guard<std::mutex> lock(log_mutex());
    open_log_file_locked(directory, keep);
}

inline void log(const char * format, ...)
{
    std::lock_guard<std::mutex> lock(log_mutex());
    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - log_epoch()).count();
    char line[4096];
    int used = std::snprintf(line, sizeof(line), "[+%08.3fs] ", elapsed);
    if(used < 0) used = 0;
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(line + used, sizeof(line) - static_cast<size_t>(used), format, arguments);
    va_end(arguments);
    std::fputs(line, stderr);
    std::fflush(stderr);
    if(log_file() != nullptr) {
        std::fputs(line, log_file());
        std::fflush(log_file());
        log_file_state().written += std::strlen(line);
        if(log_file_state().written >= kLogFileMaxBytes) {
            const LogFileState state = log_file_state();
            open_log_file_locked(state.directory, state.keep);
        }
    }
}

} // namespace qo100
