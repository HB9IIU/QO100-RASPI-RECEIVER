#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace qo100 {

struct ReceiverSettings {
    double lnb_lo_mhz = 9750.0;
    bool lnb_voltage_enabled = false;
    bool lnb_voltage_horizontal = false;
    int audio_volume_percent = 50;
};

ReceiverSettings load_receiver_settings(const std::string & repository_root);

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
