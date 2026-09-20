#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace qo100 {

struct ReceiverSettings {
    /* The NOMINAL LNB local-oscillator frequency, 9750.0 MHz. It is no longer
     * editable on the settings screen (the real LO is measured by the automatic
     * calibration instead); it stays in settings.json only so that an unusual
     * LNB can still be set by editing the file by hand. The local RTL-SDR
     * spectrum is built around it, and it is what tuning uses until a
     * calibration exists. */
    double lnb_lo_mhz = 9750.0;
    /* The LNB's REAL oscillator frequency, as measured by the automatic LNB
     * calibration (lnb_calibration.h): the beacon is at exactly 10491.500 MHz
     * and the MiniTiouner reports the IF it actually finds it at, so
     *     real LO = 10491.500 MHz - measured beacon IF.
     * 0 = never calibrated. Used instead of lnb_lo_mhz when tuning from the
     * remote BATC spectrum (whose frequency axis is true RF). Deliberately a
     * separate field from lnb_lo_mhz: the local RTL-SDR display is built
     * with the nominal LO, so it must keep using that one until the RTL
     * correction is calibrated too (see effective_lo_mhz in main.cpp). */
    double lnb_lo_calibrated_mhz = 0.0;
    /* When that measurement was made, "YYYY-MM-DD HH:MM" local time. Empty =
     * never calibrated. Its presence is what "calibration was done" means at
     * startup. */
    std::string lnb_lo_calibrated_at;
    bool lnb_voltage_enabled = false;
    bool lnb_voltage_horizontal = false;
    int audio_volume_percent = 50;
    bool display_800x480 = false;
    /* false (default): EXIT just exits cleanly, which Restart=always picks
     * back up a few seconds later - a quick restart, the more useful
     * default since novices are unlikely to know they need the desktop
     * icon to bring it back otherwise. true: EXIT calls
     * `systemctl --user stop` on itself, a real stop that Restart=always
     * doesn't override - for anyone who deliberately wants EXIT to close
     * it for good. */
    bool exit_full_stop = false;
};

/* True when a completed LNB calibration is stored. */
inline bool lnb_calibrated(const ReceiverSettings & settings)
{
    return settings.lnb_lo_calibrated_mhz > 0.0 && !settings.lnb_lo_calibrated_at.empty();
}

ReceiverSettings load_receiver_settings(const std::string & repository_root);
bool save_receiver_settings(const std::string & repository_root,
                            const ReceiverSettings & settings);

/* A saved Manual Tune frequency/symbol-rate. No name - the button that
 * loads it is just labelled with its own frequency (see draw_tune_page
 * in main.cpp), so there's nothing to type or store beyond the two
 * values a retune actually needs. Capped at a small fixed count
 * (kMaxTunePresets there) since the sidebar card they live in doesn't
 * scroll. */
struct TunePreset {
    long if_khz = 0;
    long symbol_rate_ksps = 0;
};

std::vector<TunePreset> load_tune_presets(const std::string & repository_root);
bool save_tune_presets(const std::string & repository_root,
                       const std::vector<TunePreset> & presets);

/* Network stream URL to paste into VLC (Media > Open Network Stream) to
 * watch the same feed the app is decoding, e.g. "udp://@239.1.1.1:5600".
 * Reflects QO100_TS_ADDR/QO100_TS_PORT if set, otherwise the defaults. */
std::string ts_stream_vlc_url();

struct ReceiverStatus {
    int demod_state = 0;
    long carrier_khz = 0;
    long symbol_rate_ksps = 0;
    int mer_x10 = 0;
    int modcod = -1;
    long agc1 = 0;
    long agc2 = 0;
    std::string service_provider;
    std::string service_name;
    int ber_x100 = 0;
    int short_frames = -1;
    int pilots = -1;
    long ldpc_errors = 0;
    int null_packet_percent = -1;

    bool locked() const { return demod_state == 3 || demod_state == 4; }
    void reset();
};

class LongmyndProcess {
public:
    explicit LongmyndProcess(std::string repository_root);
    ~LongmyndProcess();

    LongmyndProcess(const LongmyndProcess &) = delete;
    LongmyndProcess & operator=(const LongmyndProcess &) = delete;

    bool start(long frequency_khz, long symbol_rate_ksps);
    void stop();
    bool running() const;

private:
    std::string repository_root_;
    std::string directory_;
    std::string binary_;
    std::string log_path_;
    std::string pid_path_;
    int pid_ = -1;
};

/* Manages the rtl-sdr-server subprocess (qo100_sdl/tools/rtl-sdr-server), the
 * local RTL-SDR alternative to the BATC spectrum feed. Mirrors
 * LongmyndProcess's lifecycle: fork/exec, PID file for stale-instance
 * cleanup, SIGTERM then SIGKILL to stop. */
class RtlSdrProcess {
public:
    explicit RtlSdrProcess(std::string repository_root);
    ~RtlSdrProcess();

    RtlSdrProcess(const RtlSdrProcess &) = delete;
    RtlSdrProcess & operator=(const RtlSdrProcess &) = delete;

    bool start();
    void stop();
    bool running();

private:
    std::string repository_root_;
    std::string binary_;
    std::string log_path_;
    std::string pid_path_;
    int pid_ = -1;
};

class LongmyndClient {
public:
    LongmyndClient();
    ~LongmyndClient();

    LongmyndClient(const LongmyndClient &) = delete;
    LongmyndClient & operator=(const LongmyndClient &) = delete;

    void start();
    void stop();
    bool consume_status(ReceiverStatus & status);
    void send_tune(long frequency_khz, long symbol_rate_ksps);
    void send_voltage(bool enabled, bool horizontal);
    bool monitor_connected() const;
    bool control_connected() const;
    uint64_t received_updates() const;
    uint64_t replaced_updates() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qo100
