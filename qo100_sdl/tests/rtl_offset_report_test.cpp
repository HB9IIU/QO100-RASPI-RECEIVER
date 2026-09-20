/* Unit test for the --measure-offset output parser (src/rtl_offset_report.h).
 * Build and run (from qo100_sdl/):
 *   g++ -std=c++17 -Wall -Wextra -Isrc tests/rtl_offset_report_test.cpp -o /tmp/rtl_report_test && /tmp/rtl_report_test
 * The sample lines below are real output of the bundled binary on the Pi. */
#include "rtl_offset_report.h"

#include <cmath>
#include <cstdio>

using qo100::RtlOffsetReport;

static int failures = 0;
static void check(bool condition, const char * what)
{
    std::printf("  [%s] %s\n", condition ? "ok" : "FAIL", what);
    if(!condition) ++failures;
}

int main()
{
    std::printf("a normal run\n");
    RtlOffsetReport report;
    for(const char * line : {
            "Found Rafael Micro R820T tuner",                        /* library chatter: ignored */
            "MEASURE start captures=30 gain=10",
            "CAPTURE 1/30 ok correction_khz=+33.93 width_khz=1445.7 contrast_db=9.2",
            "CAPTURE 2/30 rejected reason=Left edge is not clear",
            "CAPTURE 3/30 ok correction_khz=+26.73 width_khz=1417.3 contrast_db=9.2",
            "CAPTURE 4/30 ok correction_khz=+30.78 width_khz=1440.0 contrast_db=9.0"})
        report.feed_line(line);
    check(report.total() == 30, "total captures read from the header");
    check(report.done() == 4, "four captures done");
    check(report.accepted() == 3 && report.rejected() == 1, "3 accepted, 1 rejected");
    check(std::fabs(report.running_median_khz() - 30.78) < 1e-9, "running median of the accepted ones");
    check(!report.finished(), "not finished before the RESULT line");
    report.feed_line("RESULT ok accepted=29 attempts=30 median_khz=+34.21 mean_khz=+35.05 stdev_khz=3.87");
    check(report.outcome() == RtlOffsetReport::Outcome::Ok, "outcome Ok");
    check(std::fabs(report.median_khz() - 34.21) < 1e-9, "median +34.21 kHz");
    check(std::fabs(report.stdev_khz() - 3.87) < 1e-9, "stdev 3.87 kHz");

    std::printf("negative corrections keep their sign\n");
    RtlOffsetReport negative;
    negative.feed_line("RESULT ok accepted=20 attempts=30 median_khz=-18.40 mean_khz=-18.0 stdev_khz=2.10");
    check(std::fabs(negative.median_khz() + 18.40) < 1e-9, "median -18.40 kHz");

    std::printf("a failure carries its reason\n");
    RtlOffsetReport failed;
    failed.feed_line("RESULT failed reason=cannot open the RTL-SDR: usb_claim_interface error -6");
    check(failed.outcome() == RtlOffsetReport::Outcome::Failed, "outcome Failed");
    check(failed.reason() == "cannot open the RTL-SDR: usb_claim_interface error -6", "reason kept whole");

    std::printf("cancel\n");
    RtlOffsetReport cancelled;
    cancelled.feed_line("RESULT cancelled");
    check(cancelled.outcome() == RtlOffsetReport::Outcome::Cancelled, "outcome Cancelled");

    std::printf("a process that dies without a RESULT line\n");
    RtlOffsetReport dead;
    dead.feed_line("MEASURE start captures=30 gain=10");
    dead.fail_unexpectedly("the measurement process ended unexpectedly");
    check(dead.outcome() == RtlOffsetReport::Outcome::Failed, "reported as failed");
    dead.feed_line("RESULT ok accepted=1 attempts=1 median_khz=+1 mean_khz=+1 stdev_khz=0");
    dead.fail_unexpectedly("again");
    check(dead.reason() == "the measurement process ended unexpectedly", "first reason is kept");

    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILED",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
