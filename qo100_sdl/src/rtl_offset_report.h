/* Parser for the output of `rtl-sdr-server --measure-offset` (no processes, no
 * sockets, no drawing - just text in, numbers out, so it can be unit-tested).
 *
 * WHAT IS BEING MEASURED
 * ----------------------
 * The local RTL-SDR spectrum is built around the NOMINAL LNB oscillator
 * (9750 MHz). The beacon is at exactly 10491.500 MHz, but on that display it
 * appears a little lower or higher: by the LNB's own error plus the stick's
 * crystal error. The server's --measure-offset mode captures the beacon
 * directly from the stick (fine 0.6 kHz bins, two overlapping captures, the
 * beacon's centre taken from the midpoint of its two 50% edges - the same
 * method as scripts/calibrate_beacon.py) and reports that shift as a
 * "correction" in kHz: positive means the beacon appears LOWER than it
 * should, and the very same number is what `--correction-khz` takes to move
 * the display back onto the true frequency.
 *
 * THE PROTOCOL (one line per event, see rtl-sdr-server-source/main.py)
 * ------------------------------------------------------------------
 *   MEASURE start captures=<n> gain=<db>
 *   CAPTURE <i>/<n> ok correction_khz=<+x.xx> width_khz=<x> contrast_db=<x>
 *   CAPTURE <i>/<n> rejected reason=<text>
 *   RESULT ok accepted=<a> attempts=<n> median_khz=<+x.xx> mean_khz=<+x.xx> stdev_khz=<x.xx>
 *   RESULT failed reason=<text>
 *   RESULT cancelled
 * Anything else (library chatter such as "Found Rafael Micro R820T tuner")
 * is ignored.
 */
#pragma once

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

namespace qo100 {

class RtlOffsetReport {
public:
    enum class Outcome { None, Ok, Failed, Cancelled };

    void reset() { *this = RtlOffsetReport{}; }

    /* Feed one line of the server's output. */
    void feed_line(const std::string & line)
    {
        if(starts_with(line, "MEASURE start")) {
            total_ = static_cast<int>(number_after(line, "captures=", 0.0));
        }
        else if(starts_with(line, "CAPTURE ")) {
            /* "CAPTURE 12/30 ok ..." - the leading "12/30" is progress. */
            char * end = nullptr;
            const int index = static_cast<int>(std::strtol(line.c_str() + 8, &end, 10));
            if(end != nullptr && *end == '/') total_ = static_cast<int>(std::strtol(end + 1, nullptr, 10));
            done_ = std::max(done_, index);
            if(line.find(" ok ") != std::string::npos)
                accepted_.push_back(number_after(line, "correction_khz=", 0.0));
            else
                ++rejected_;
        }
        else if(starts_with(line, "RESULT ok")) {
            outcome_ = Outcome::Ok;
            median_khz_ = number_after(line, "median_khz=", 0.0);
            stdev_khz_ = number_after(line, "stdev_khz=", 0.0);
        }
        else if(starts_with(line, "RESULT failed")) {
            outcome_ = Outcome::Failed;
            const size_t at = line.find("reason=");
            reason_ = at == std::string::npos ? "the measurement failed" : line.substr(at + 7);
        }
        else if(starts_with(line, "RESULT cancelled")) {
            outcome_ = Outcome::Cancelled;
        }
    }

    /* The process ended without ever printing a RESULT line (crashed, killed,
     * binary missing...). */
    void fail_unexpectedly(const std::string & why)
    {
        if(outcome_ != Outcome::None) return;
        outcome_ = Outcome::Failed;
        reason_ = why;
    }

    Outcome outcome() const { return outcome_; }
    bool finished() const { return outcome_ != Outcome::None; }
    int total() const { return total_; }          /* captures planned (0 until known) */
    int done() const { return done_; }            /* captures finished so far */
    int accepted() const { return static_cast<int>(accepted_.size()); }
    int rejected() const { return rejected_; }
    /* Final correction, kHz. Meaningful when outcome() == Ok. */
    double median_khz() const { return median_khz_; }
    double stdev_khz() const { return stdev_khz_; }
    const std::string & reason() const { return reason_; }

    /* Median of the readings accepted so far, for the live display (0 when
     * none yet). */
    double running_median_khz() const
    {
        if(accepted_.empty()) return 0.0;
        std::vector<double> sorted = accepted_;
        std::sort(sorted.begin(), sorted.end());
        const size_t n = sorted.size();
        return n % 2 == 1 ? sorted[n / 2] : (sorted[n / 2 - 1] + sorted[n / 2]) / 2.0;
    }

private:
    static bool starts_with(const std::string & text, const char * prefix)
    {
        return text.rfind(prefix, 0) == 0;
    }
    /* The number written right after `key` ("median_khz=+26.47"), or fallback. */
    static double number_after(const std::string & text, const char * key, double fallback)
    {
        const size_t at = text.find(key);
        if(at == std::string::npos) return fallback;
        char * end = nullptr;
        const char * begin = text.c_str() + at + std::string(key).size();
        const double value = std::strtod(begin, &end);
        return end == begin ? fallback : value;
    }

    Outcome outcome_ = Outcome::None;
    int total_ = 0;
    int done_ = 0;
    int rejected_ = 0;
    double median_khz_ = 0.0;
    double stdev_khz_ = 0.0;
    std::string reason_;
    std::vector<double> accepted_;
};

} // namespace qo100
