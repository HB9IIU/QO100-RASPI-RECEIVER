/* Unit test for the LNB calibration state machine (src/lnb_calibration.h).
 *
 * It needs no receiver and no display: a fake "receiver" produces the status
 * a real MiniTiouner would (unlocked while away from the beacon, locked on
 * it with a fixed carrier IF) and simulated time is advanced by hand.
 *
 * Build and run (from qo100_sdl/):
 *   g++ -std=c++17 -Wall -Wextra -Isrc tests/lnb_calibration_test.cpp -o /tmp/lnb_cal_test && /tmp/lnb_cal_test
 */
#include "lnb_calibration.h"

#include <cstdio>
#include <functional>

using qo100::LnbCalibration;
using qo100::ReceiverStatus;
using Clock = LnbCalibration::Clock;

namespace {

int failures = 0;

void check(bool condition, const char * what)
{
    std::printf("  [%s] %s\n", condition ? "ok" : "FAIL", what);
    if(!condition) ++failures;
}

/* A pretend MiniTiouner + longmynd. `beacon_if_khz` is where the beacon
 * really arrives; the demodulator can lock on it whenever the requested
 * frequency is within `window_khz` of it (like the real ~+/-1.2 MHz search
 * window), and then reports that true IF. */
struct FakeReceiver {
    long beacon_if_khz = 741471;
    long window_khz = 1200;
    bool beacon_present = true;
    long requested_khz = 0;
    /* Extra unlocked statuses after every tune, like the real re-init time. */
    int settle_updates = 3;
    int updates_since_tune = 0;

    void tune(long khz) { requested_khz = khz; updates_since_tune = 0; }

    ReceiverStatus status()
    {
        ReceiverStatus s;
        ++updates_since_tune;
        const bool in_window = std::labs(requested_khz - beacon_if_khz) <= window_khz;
        if(beacon_present && in_window && updates_since_tune > settle_updates) {
            s.demod_state = 4;               /* DVB-S2 lock */
            s.carrier_khz = beacon_if_khz;   /* absolute IF the demod measured */
        }
        else {
            s.demod_state = 1;               /* hunting */
            s.carrier_khz = requested_khz;
        }
        return s;
    }
};

/* Runs the machine against the fake receiver: 200 ms status cadence, 50 ms
 * ticks, until it stops running or `limit` simulated seconds pass. Every tune
 * the machine asks for is recorded. */
struct Run {
    std::vector<long> tunes;
    double seconds = 0;
};

Run drive(LnbCalibration & cal, FakeReceiver & rx, Clock::time_point t0,
          double limit_s = 600,
          const std::function<void(double)> & each_tick = {})
{
    Run run;
    auto now = t0;
    long tick = 0;
    while(cal.running() && run.seconds < limit_s) {
        now += std::chrono::milliseconds(50);
        run.seconds += 0.05;
        ++tick;
        if(each_tick) each_tick(run.seconds);
        if(tick % 4 == 0) cal.on_status(now, rx.status());   /* 200 ms status */
        cal.tick(now);
        if(auto command = cal.take_command()) {
            rx.tune(command->if_khz);
            run.tunes.push_back(command->if_khz);
        }
    }
    return run;
}

void test_success()
{
    std::printf("success on a normal beacon\n");
    LnbCalibration cal;
    FakeReceiver rx;                      /* beacon at IF 741471 -> LO 9750.029 */
    const auto t0 = Clock::now();
    cal.start(t0, 741474, 1500, 9750.0);
    /* start() queues the very first hop; take it here so it is counted too. */
    long first_tune = 0;
    if(auto c = cal.take_command()) { rx.tune(c->if_khz); first_tune = c->if_khz; }
    Run run = drive(cal, rx, t0);
    run.tunes.insert(run.tunes.begin(), first_tune);
    check(cal.outcome() == LnbCalibration::Outcome::Success, "outcome is Success");
    check(std::fabs(cal.result_lo_mhz() - 9750.029) < 1e-6, "LO = 10491.5 - 741.471 = 9750.029");
    check(std::fabs(cal.deviation_khz() - 29.0) < 1e-6, "deviation +29 kHz from nominal");
    check(cal.attempts().size() == LnbCalibration::kAttempts, "all ten attempts were made");
    bool all_valid = true;
    for(const auto & a : cal.attempts()) all_valid = all_valid && a.valid && a.if_khz == 741471;
    check(all_valid, "every attempt valid, IF 741471");
    /* 5 attempts x (hop, acquire) = 10 tunes, alternating far / near. */
    check(run.tunes.size() == 2 * LnbCalibration::kAttempts, "twenty tune commands (hop + acquire, ten times)");
    bool alternate = true;
    for(size_t i = 0; i < run.tunes.size(); ++i)
        alternate = alternate && run.tunes[i] == (i % 2 == 0 ? 741474 + 3000 : 741474);
    check(alternate, "commands alternate: hop to IF+3000, then back to 741474");
    check(run.seconds > 60 && run.seconds < 150, "takes roughly a minute and a half of simulated time");
}

void test_median_rejects_outlier()
{
    std::printf("one odd reading does not move the result\n");
    LnbCalibration cal;
    FakeReceiver rx;
    const auto t0 = Clock::now();
    cal.start(t0, 741474, 1500, 9750.0);
    if(auto c = cal.take_command()) rx.tune(c->if_khz);
    int attempt_seen = 0;
    long last_tunes = 0;
    /* Move the beacon by 20 kHz during the third attempt only. */
    drive(cal, rx, t0, 600, [&](double) {
        if(rx.requested_khz != last_tunes) { last_tunes = rx.requested_khz;
            if(last_tunes == 741474) ++attempt_seen; }
        rx.beacon_if_khz = attempt_seen == 3 ? 741451 : 741471;
    });
    check(cal.outcome() == LnbCalibration::Outcome::Success, "still a Success");
    check(std::fabs(cal.result_lo_mhz() - 9750.029) < 1e-6, "median ignores the 20 kHz outlier");
}

void test_no_beacon()
{
    std::printf("no beacon -> the run fails quickly, with a clear reason\n");
    LnbCalibration cal;
    FakeReceiver rx;
    rx.beacon_present = false;
    const auto t0 = Clock::now();
    cal.start(t0, 741474, 1500, 9750.0);
    if(auto c = cal.take_command()) rx.tune(c->if_khz);
    const Run run = drive(cal, rx, t0);
    check(cal.outcome() == LnbCalibration::Outcome::Failed, "outcome is Failed");
    check(cal.failure_reason().find("did not lock") != std::string::npos, "reason mentions no lock");
    check(run.seconds < 40, "gave up within ~30 s, did not burn all ten attempts");
}

void test_implausible_lo()
{
    std::printf("something far from nominal locked -> rejected as implausible\n");
    LnbCalibration cal;
    FakeReceiver rx;
    rx.beacon_if_khz = 742300;            /* would mean LO 9749.2: 0.8 MHz off */
    rx.window_khz = 2000;
    const auto t0 = Clock::now();
    cal.start(t0, 741474, 1500, 9750.0);
    if(auto c = cal.take_command()) rx.tune(c->if_khz);
    drive(cal, rx, t0);
    check(cal.outcome() == LnbCalibration::Outcome::Failed, "outcome is Failed");
    check(cal.failure_reason().find("implausibly") != std::string::npos, "reason says implausible");
}

void test_cancel()
{
    std::printf("cancel stops immediately and stores nothing\n");
    LnbCalibration cal;
    FakeReceiver rx;
    const auto t0 = Clock::now();
    cal.start(t0, 741474, 1500, 9750.0);
    cal.cancel();
    check(!cal.running(), "no longer running");
    check(cal.outcome() == LnbCalibration::Outcome::Cancelled, "outcome is Cancelled");
    check(!cal.take_command().has_value(), "no command left to send");
}

void test_stale_lock_is_ignored_while_hopping()
{
    std::printf("a stale 'locked' status right after the hop restarts the wait\n");
    LnbCalibration cal;
    const auto t0 = Clock::now();
    cal.start(t0, 741474, 1500, 9750.0);
    (void)cal.take_command();             /* hop command */
    ReceiverStatus locked; locked.demod_state = 4; locked.carrier_khz = 741471;
    ReceiverStatus unlocked; unlocked.demod_state = 1;
    auto now = t0;
    cal.on_status(now += std::chrono::milliseconds(200), unlocked);
    cal.on_status(now += std::chrono::milliseconds(700), unlocked);   /* 0.7 s unlocked */
    cal.on_status(now += std::chrono::milliseconds(200), locked);     /* stale lock: reset */
    cal.on_status(now += std::chrono::milliseconds(200), unlocked);
    cal.on_status(now += std::chrono::milliseconds(900), unlocked);   /* only 0.9 s since reset */
    check(cal.step() == LnbCalibration::Step::Hopping, "still hopping after only 0.9 s clean");
    cal.on_status(now += std::chrono::milliseconds(200), unlocked);   /* now > 1 s */
    check(cal.step() == LnbCalibration::Step::Acquiring, "acquiring once a full clean second passed");
    check(cal.take_command().has_value(), "acquisition tune queued");
}

} // namespace

int main()
{
    test_success();
    test_median_rejects_outlier();
    test_no_beacon();
    test_implausible_lo();
    test_cancel();
    test_stale_lock_is_ignored_while_hopping();
    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILED",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
