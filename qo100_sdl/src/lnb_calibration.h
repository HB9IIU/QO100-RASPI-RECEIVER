/* LNB local-oscillator calibration - the logic only (no drawing, no sockets).
 *
 * WHY THIS EXISTS
 * ---------------
 * The app converts every RF frequency into an IF for the MiniTiouner with
 *     IF = RF - LNB LO,
 * and the LO is nominally 9750 MHz. A real LNB's oscillator is a few tens of
 * kHz away from that (yours: about +29 kHz). The receiver's demodulator can
 * search around the requested frequency and lock anyway, so a wrong LO is
 * invisible on wide signals - but the narrowest QO-100 signals (35 kS/s and
 * up) have a lock window of only tens of kHz, so an uncorrected LNB error can
 * stop them locking at all.
 *
 * HOW THE LO IS MEASURED
 * ----------------------
 * The QO-100 wideband beacon is taken to be at exactly 10491.500 MHz. The
 * MiniTiouner locks onto it and longmynd reports the carrier's IF that the
 * demodulator actually measured. Therefore
 *
 *     real LNB LO = 10491.500 MHz - measured beacon IF.
 *
 * (Example: measured IF 741471 kHz -> real LO 9750.029 MHz, i.e. +29 kHz.)
 * The IF longmynd reports is the requested frequency PLUS the offset the
 * demodulator had to correct, so the frequency we ask for while acquiring
 * ("acquisition IF") only has to be close enough to obtain lock - it does not
 * determine the result. longmynd rounds that offset to whole kHz, so the
 * measurement is good to about +/-1 kHz, which is far finer than the
 * narrowest signal's lock window needs.
 *
 * HOW ONE MEASUREMENT WORKS (repeated kAttempts times)
 * ----------------------------------------------------
 *   Hopping    tune 3 MHz away from the beacon and wait until the receiver has
 *              been UNLOCKED for a full second. This breaks any existing lock,
 *              so the next step is a genuine fresh acquisition - not merely
 *              the demodulator still tracking the beacon it already had.
 *              (Only ~1.2 MHz away the demodulator still finds the beacon by
 *              itself, which is why the hop has to be this large.)
 *   Acquiring  tune the beacon and wait for lock.
 *   Settling   let the lock settle for kSettle; a lock loss here rejects the
 *              attempt.
 *   Measuring  for kWindow, record the carrier IF of every status update; the
 *              attempt's value is the MEDIAN of those. Any lock loss rejects
 *              the attempt.
 *   Pause      one second, then the next attempt.
 * The final answer is the median over the accepted attempts, so a single odd
 * reading cannot move it.
 *
 * WHAT THIS DOES NOT PROVE
 * ------------------------
 * The result is a MiniTiouner-referenced operational estimate of the LNB LO,
 * not an independently verified absolute-frequency measurement: it assumes the
 * beacon really is at 10491.500 MHz and it includes any error of the
 * MiniTiouner's own reference. That is exactly what tap-to-tune needs (the
 * IF at which signals appear on THIS receiver), but it is not a lab number.
 *
 * HOW IT IS DRIVEN
 * ----------------
 * The class owns no timers or threads. The main loop calls
 *     on_status(now, status)   whenever a NEW receiver status has arrived,
 *     tick(now)                once per frame (drives the timeouts),
 * and after either call asks take_command() whether a tune has to be sent.
 * That keeps every decision here deterministic and unit-testable with
 * simulated status (see qo100_sdl/tests/lnb_calibration_test.cpp).
 */
#pragma once

#include "receiver.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace qo100 {

class LnbCalibration {
public:
    using Clock = std::chrono::steady_clock;

    /* The beacon's nominal RF frequency: the reference everything is
     * measured against. */
    static constexpr double kBeaconRfMhz = 10491.500;

    /* --- tuning constants (every one of these was checked on real
     * hardware with the standalone script scripts/measure_lnb_lo.py) --- */

    /* Measurements per run. Each independent hop-and-acquire cycle takes about
     * 9 s, so ten take about a minute and a half. Real-hardware runs were
     * repeatable to within the 1 kHz resolution already with fewer; ten make
     * the median more robust against an occasional odd reading. */
    static constexpr int kAttempts = 10;
    /* At least this many of them must be usable (60%), otherwise the run
     * fails instead of reporting a number nobody should trust. */
    static constexpr int kMinValid = 6;
    /* How far above the beacon the hop goes. It must be outside the
     * demodulator's search window (which reaches at least 1.2 MHz). */
    static constexpr long kHopKhz = 3000;
    /* The receiver must stay unlocked this long after the hop before we
     * believe the old lock is really gone (also discards any stale status). */
    static constexpr std::chrono::milliseconds kHopUnlockedHold{1000};
    /* Give up waiting for the hop to take effect (continue anyway). */
    static constexpr std::chrono::seconds kHopTimeout{8};
    /* No beacon lock after this long means no beacon: the whole run fails
     * rather than burning the remaining attempts on the same problem. */
    static constexpr std::chrono::seconds kLockTimeout{20};
    /* Time to let a fresh lock settle before reading, and how long to read. */
    static constexpr std::chrono::milliseconds kSettle{3000};
    static constexpr std::chrono::milliseconds kWindow{2000};
    /* Status arrives about five times a second; fewer than this many usable
     * packets inside the window means the attempt is not trustworthy. */
    static constexpr size_t kMinSamplesPerAttempt = 3;
    static constexpr std::chrono::milliseconds kPause{1000};
    /* A result further than this from the nominal LO is rejected as
     * implausible (wrong LO setting, or something else than the beacon
     * locked). Real LNBs are tens of kHz off, not hundreds. */
    static constexpr double kMaxPlausibleDeviationMhz = 0.6;

    /* What the machine is doing right now (drives the on-screen step text). */
    enum class Step { Idle, Hopping, Acquiring, Settling, Measuring, Pausing, Finished };
    /* How a run ended. None while it has not ended (or never started). */
    enum class Outcome { None, Success, Failed, Cancelled };

    /* A tune the caller must send to longmynd. */
    struct Command {
        long if_khz;
        long symbol_rate_ksps;
    };

    /* One finished measurement, for the live list on screen. */
    struct Attempt {
        bool valid = false;
        long if_khz = 0;        /* median measured IF; meaningful when valid */
        std::string note;       /* why it was rejected, when it was */
    };

    /* Begin a run. acquisition_if_khz / symbol_rate_ksps are only used to
     * obtain lock (see the file comment); nominal_lo_mhz is the nominal
     * LO (9750 MHz), used solely for the plausibility check. */
    void start(Clock::time_point now, long acquisition_if_khz,
               long symbol_rate_ksps, double nominal_lo_mhz)
    {
        acquisition_if_khz_ = acquisition_if_khz;
        symbol_rate_ksps_ = symbol_rate_ksps;
        nominal_lo_mhz_ = nominal_lo_mhz;
        attempts_.clear();
        samples_.clear();
        outcome_ = Outcome::None;
        failure_reason_.clear();
        result_lo_mhz_ = 0.0;
        pending_.reset();
        begin_attempt(now);
    }

    /* Stop immediately. Nothing is stored; the caller retunes the beacon. */
    void cancel()
    {
        if(!running()) return;
        step_ = Step::Finished;
        outcome_ = Outcome::Cancelled;
        pending_.reset();
    }

    /* Forget a finished run (its result has been shown/stored), returning to
     * the idle state the page shows before a run. */
    void reset()
    {
        step_ = Step::Idle;
        outcome_ = Outcome::None;
        failure_reason_.clear();
        attempts_.clear();
        samples_.clear();
        pending_.reset();
    }

    bool running() const { return step_ != Step::Idle && step_ != Step::Finished; }
    Step step() const { return step_; }
    Outcome outcome() const { return outcome_; }
    const std::string & failure_reason() const { return failure_reason_; }
    /* The measured real LO. Meaningful only when outcome() == Success. */
    double result_lo_mhz() const { return result_lo_mhz_; }
    /* Deviation of the result from the nominal LO, in kHz. */
    double deviation_khz() const { return (result_lo_mhz_ - nominal_lo_mhz_) * 1000.0; }
    /* 1-based number of the attempt in progress (kAttempts once finished). */
    int attempt_number() const
    {
        return std::min(static_cast<int>(attempts_.size()) + 1, kAttempts);
    }
    const std::vector<Attempt> & attempts() const { return attempts_; }

    /* Running estimate from the accepted attempts so far (0 = none yet), for
     * the live display. Same rule as the final answer: median IF. */
    double running_lo_mhz() const
    {
        const std::optional<double> median_if = median_valid_if_khz();
        return median_if ? kBeaconRfMhz - *median_if / 1000.0 : 0.0;
    }

    /* The tune that has to go out now, if any. Returned once, then cleared. */
    std::optional<Command> take_command()
    {
        std::optional<Command> command = pending_;
        pending_.reset();
        return command;
    }

    /* Feed a NEW receiver status (one that has just arrived, not the same
     * one twice - the caller only invokes this when a fresh status was
     * consumed). */
    void on_status(Clock::time_point now, const ReceiverStatus & status)
    {
        switch(step_) {
        case Step::Hopping:
            /* Wait for one continuous second without lock. A locked status
             * resets the clock: it is either the old lock (a stale message
             * still in flight) or the hop landed on something. */
            if(status.locked()) {
                unlocked_since_.reset();
            }
            else {
                if(!unlocked_since_) unlocked_since_ = now;
                if(now - *unlocked_since_ >= kHopUnlockedHold) start_acquiring(now);
            }
            break;
        case Step::Acquiring:
            if(status.locked()) {
                locked_at_ = now;
                step_ = Step::Settling;
            }
            break;
        case Step::Settling:
            if(!status.locked()) reject(now, "lock lost while settling");
            break;
        case Step::Measuring:
            if(!status.locked()) {
                reject(now, "lock lost while measuring");
            }
            else {
                /* carrier_khz is longmynd's requested frequency plus the
                 * carrier offset the demodulator measured: the absolute IF
                 * at which the beacon really arrives. */
                samples_.push_back(status.carrier_khz);
            }
            break;
        default:
            break;
        }
    }

    /* Advance the timeouts. Call once per frame. */
    void tick(Clock::time_point now)
    {
        switch(step_) {
        case Step::Hopping:
            /* The hop never produced a clean unlocked second (e.g. status
             * stalled). Carry on: the acquisition below still works, and a
             * stale reading would show up as a rejected/odd attempt. */
            if(now - step_started_ >= kHopTimeout) start_acquiring(now);
            break;
        case Step::Acquiring:
            if(now - step_started_ >= kLockTimeout)
                fail("The beacon did not lock within 20 seconds. Check the dish "
                     "pointing and that the LNB is powered.");
            break;
        case Step::Settling:
            if(now - locked_at_ >= kSettle) {
                samples_.clear();
                step_ = Step::Measuring;
                step_started_ = now;
            }
            break;
        case Step::Measuring:
            if(now - step_started_ >= kWindow) finish_measurement(now);
            break;
        case Step::Pausing:
            if(now - step_started_ >= kPause) next_attempt_or_finish(now);
            break;
        default:
            break;
        }
    }

    /* Human-readable description of the current step, for the screen. */
    const char * step_text() const
    {
        switch(step_) {
        case Step::Hopping:    return "Tuning away from the beacon...";
        case Step::Acquiring:  return "Waiting for the beacon to lock...";
        case Step::Settling:   return "Beacon locked - letting it settle...";
        case Step::Measuring:  return "Reading the carrier frequency...";
        case Step::Pausing:    return "Measurement recorded.";
        default:               return "";
        }
    }

private:
    /* Median of the accepted attempts' IFs. */
    std::optional<double> median_valid_if_khz() const
    {
        std::vector<long> values;
        for(const Attempt & attempt : attempts_)
            if(attempt.valid) values.push_back(attempt.if_khz);
        if(values.empty()) return std::nullopt;
        std::sort(values.begin(), values.end());
        const size_t n = values.size();
        return n % 2 == 1 ? static_cast<double>(values[n / 2])
                          : (values[n / 2 - 1] + values[n / 2]) / 2.0;
    }

    void begin_attempt(Clock::time_point now)
    {
        step_ = Step::Hopping;
        step_started_ = now;
        unlocked_since_.reset();
        samples_.clear();
        /* Hop away from the beacon (upwards; the band is empty enough there
         * that the demodulator stays unlocked - checked on real hardware). */
        pending_ = Command{acquisition_if_khz_ + kHopKhz, symbol_rate_ksps_};
    }

    void start_acquiring(Clock::time_point now)
    {
        step_ = Step::Acquiring;
        step_started_ = now;
        pending_ = Command{acquisition_if_khz_, symbol_rate_ksps_};
    }

    /* The reading window is over: turn the collected IFs into an attempt. */
    void finish_measurement(Clock::time_point now)
    {
        if(samples_.size() < kMinSamplesPerAttempt) {
            reject(now, "too few status updates in the window");
            return;
        }
        std::vector<long> sorted = samples_;
        std::sort(sorted.begin(), sorted.end());
        Attempt attempt;
        attempt.valid = true;
        /* Median of the window (an even count takes the lower middle value,
         * so the stored IF is always a whole kHz like longmynd reports). */
        attempt.if_khz = sorted[(sorted.size() - 1) / 2];
        attempts_.push_back(attempt);
        step_ = Step::Pausing;
        step_started_ = now;
    }

    void reject(Clock::time_point now, const char * why)
    {
        Attempt attempt;
        attempt.note = why;
        attempts_.push_back(attempt);
        step_ = Step::Pausing;
        step_started_ = now;
    }

    void next_attempt_or_finish(Clock::time_point now)
    {
        if(static_cast<int>(attempts_.size()) < kAttempts) {
            begin_attempt(now);
            return;
        }
        int valid = 0;
        for(const Attempt & attempt : attempts_) if(attempt.valid) ++valid;
        if(valid < kMinValid) {
            fail("Only " + std::to_string(valid) + " of " + std::to_string(kAttempts) +
                 " measurements were usable. The signal may be too weak or unstable.");
            return;
        }
        const double lo_mhz = kBeaconRfMhz - *median_valid_if_khz() / 1000.0;
        if(std::fabs(lo_mhz - nominal_lo_mhz_) > kMaxPlausibleDeviationMhz) {
            fail("The measured LO is implausibly far from the configured one. Check the "
                 "LNB LO setting, and that the locked signal really is the beacon.");
            return;
        }
        result_lo_mhz_ = lo_mhz;
        outcome_ = Outcome::Success;
        step_ = Step::Finished;
    }

    void fail(std::string reason)
    {
        failure_reason_ = std::move(reason);
        outcome_ = Outcome::Failed;
        step_ = Step::Finished;
        pending_.reset();
    }

    Step step_ = Step::Idle;
    Outcome outcome_ = Outcome::None;
    long acquisition_if_khz_ = 0;
    long symbol_rate_ksps_ = 0;
    double nominal_lo_mhz_ = 9750.0;
    double result_lo_mhz_ = 0.0;
    std::string failure_reason_;
    std::vector<Attempt> attempts_;
    std::vector<long> samples_;           /* IFs seen inside the current window */
    std::optional<Command> pending_;
    std::optional<Clock::time_point> unlocked_since_;
    Clock::time_point step_started_{};
    Clock::time_point locked_at_{};
};

} // namespace qo100
