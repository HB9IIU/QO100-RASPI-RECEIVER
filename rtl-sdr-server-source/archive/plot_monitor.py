"""Sweep the QO-100 WB transponder in overlapping RTL-SDR captures.

Bandplan: https://amsat-dl.org/en/p4-a-wb-transponder-bandplan-and-operating-guidelines/
LNB must be powered and configured for WB horizontal polarization.
The trace combines different capture times; this is not a DATV decoder.
"""

from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from time import monotonic

import matplotlib

matplotlib.use("QtAgg")  # Separate live window in PyCharm.
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.widgets import Button, TextBox
from rtlsdr import RtlSdr
from app.spectrum_ws import SpectrumServer

LNB_LO_MHZ = 9750.0  # Change to match your LNB oscillator.
RF_START_MHZ = 10490.5
RF_STOP_MHZ = 10499.5
BEACON_RF_MHZ = 10491.5
SAMPLE_RATE = 2.4e6  # RTL-SDR's practical reliable ceiling; wider captures need fewer sweep steps.


FFT_SIZE = 246  # SAMPLE_RATE / FFT_SIZE kept close to OUTPUT_BIN_HZ, as before.
AVERAGES = 335
GAIN_DB = 10.0  # Fixed gain keeps portions comparable; lower if overloaded.
USABLE_HALF_HZ = 1_066_667  # Discard the outer edges of each capture. Scaled with SAMPLE_RATE (x4/3).
STEP_HZ = 933_333  # Neighbor captures cover each tuner center. Scaled with SAMPLE_RATE (x4/3).
DC_EXCLUDE_HZ = 10_000  # Overlapping captures fill the DC region.
# Match the reference Airspy server's output, independently of capture FFT_SIZE.
OUTPUT_CENTER_HZ = 745_000_000
OUTPUT_BIN_HZ = 10_000_000 / 1024
OUTPUT_FIRST_BIN = 51
OUTPUT_STOP_BIN = 973  # Exclusive: bins 51 through 972, exactly 922 points.


def next_averages(current, elapsed, capture_seconds, target):
    """Budget capture time after measured tuning, rendering and other overhead."""
    maximum = max(1, int(SAMPLE_RATE * 0.2 / FFT_SIZE))
    overhead = max(0.0, elapsed - capture_seconds)
    estimate = current * max(0.0, target - overhead) / max(capture_seconds, 1e-6)
    return int(np.clip(round(estimate), 1, maximum))


def spectrum_levels(grid, trace):
    """Estimate the lower envelope and median of the beacon's central plateau.

    The lower envelope is only a noise-floor estimate, not calibrated noise power.
    """
    finite = np.isfinite(trace)
    if not np.any(finite):
        return None
    values = trace[finite]
    beacon_center = (BEACON_RF_MHZ - LNB_LO_MHZ) * 1e6
    core = finite & (np.abs(grid - beacon_center) <= 400_000)
    floor = np.percentile(values, 2)
    plateau = np.median(trace[core]) if np.any(core) else np.percentile(values, 90)
    lower = floor - 4
    upper = max(plateau, np.percentile(values, 99)) + 5
    upper = max(upper, lower + 12)
    return floor, plateau, lower, upper


def sweep_plan():
    start = (RF_START_MHZ - LNB_LO_MHZ) * 1e6
    stop = (RF_STOP_MHZ - LNB_LO_MHZ) * 1e6
    # Capture spectra are interpolated onto these exact Airspy bin frequencies.
    grid = OUTPUT_CENTER_HZ + (np.arange(OUTPUT_FIRST_BIN, OUTPUT_STOP_BIN)
                               - 512) * OUTPUT_BIN_HZ
    # Endpoints stay away from both DC and the discarded capture edges.
    centers = np.linspace(start + 300_000, stop - 300_000,
                          int(np.ceil((stop - start - 600_000) / STEP_HZ)) + 1)
    return grid, centers


def capture_power(sdr, window, averages=AVERAGES):
    required = FFT_SIZE * averages
    # Each complex sample is two USB bytes; reads must align to 512 bytes.
    # Pad the transfer independently of FFT size, then discard extra samples.
    read_count = ((required + 255) // 256) * 256
    samples = np.asarray(sdr.read_samples(read_count))[:required]
    blocks = samples.reshape(-1, FFT_SIZE)
    blocks = blocks - blocks.mean(axis=1, keepdims=True)
    spectrum = np.fft.fftshift(np.fft.fft(blocks * window, axis=1), axes=1)
    return np.mean(np.abs(spectrum) ** 2, axis=0) / (FFT_SIZE * np.sum(window ** 2))


def main():
    grid, centers = sweep_plan()
    offsets = np.fft.fftshift(np.fft.fftfreq(FFT_SIZE, 1 / SAMPLE_RATE))
    window = np.hanning(FFT_SIZE)
    sdr = RtlSdr()
    websocket = None
    try:
        websocket = SpectrumServer()
        websocket.start()
        print('Spectrum WebSocket: ws://localhost:7681 (LAN: ws://<this-PC-IP>:7681)')
        sdr.sample_rate = SAMPLE_RATE
        sdr.set_agc_mode(False)
        sdr.gain = GAIN_DB
        fig, ax = plt.subplots(figsize=(12, 6))
        fig.subplots_adjust(top=0.78, bottom=0.30)
        ax.set(xlabel="LNB output frequency (MHz)", ylabel="Relative power (dB/bin)",
               xlim=(grid[0] / 1e6, grid[-1] / 1e6), ylim=(-100, 0))
        ax.ticklabel_format(axis="x", style="plain", useOffset=False)
        top = ax.secondary_xaxis("top", functions=(
            lambda x: x + LNB_LO_MHZ, lambda x: x - LNB_LO_MHZ))
        top.set_xlabel("Satellite downlink (MHz)")
        top.ticklabel_format(axis="x", style="plain", useOffset=False)
        ax.grid(True, alpha=0.3)
        ax.axvline(BEACON_RF_MHZ - LNB_LO_MHZ, color="orange", ls="--",
                   label="WB beacon (nominal center)")
        previous, = ax.plot(grid / 1e6, np.full(grid.size, np.nan),
                            color="gray", alpha=0.5, label="Previous sweep")
        line, = ax.plot(grid / 1e6, np.full(grid.size, np.nan),
                        lw=1, label="Current sweep")
        ax.legend(loc="upper right")
        status = fig.text(0.1, 0.18, "Starting sweep…")
        levels_text = fig.text(0.1, 0.14, "Auto scale: waiting for first complete sweep")
        target_box = TextBox(fig.add_axes([0.22, 0.065, 0.10, 0.045]),
                             "Target (seconds) ", initial="1.0")
        auto_button = Button(fig.add_axes([0.36, 0.065, 0.14, 0.045]), "Auto tune")
        tuning_text = fig.text(0.1, 0.02, "Auto tune adjusts averaging; FFT resolution stays fixed.")
        tuning = {"pending": None, "active": False, "times": [], "captures": [],
                  "round": 0, "best": None}

        def request_tuning(_event):
            try:
                seconds = float(target_box.text)
                if not np.isfinite(seconds) or not 0.1 <= seconds <= 60:
                    raise ValueError
            except ValueError:
                tuning_text.set_text("Enter a target between 0.1 and 60 seconds.")
                return
            tuning["pending"] = seconds
            tuning_text.set_text("Auto tune queued for the next complete sweep.")

        auto_button.on_clicked(request_tuning)
        fig.suptitle("QO-100 WB — stitched spectrum (sequential captures)")
        plt.show(block=False)
        sweep_number = 0
        scale_limits = None
        averages = AVERAGES
        last_elapsed = None
        while plt.fignum_exists(fig.number):
            if tuning["pending"] is not None:
                tuning.update(target=tuning["pending"], pending=None, active=True,
                              times=[], captures=[], round=0, best=None)
            started = monotonic()
            capture_seconds = 0.0
            total = np.zeros(grid.size)
            weights = np.zeros(grid.size)
            line.set_ydata(np.full(grid.size, np.nan))
            for index, center in enumerate(centers, 1):
                if not plt.fignum_exists(fig.number):
                    return
                sdr.center_freq = center
                sdr.read_samples(65536)  # Flush old samples and allow tuner settling.
                capture_started = monotonic()
                power = capture_power(sdr, window, averages)
                capture_seconds += monotonic() - capture_started
                distance = np.abs(grid - center)
                valid = (distance <= USABLE_HALF_HZ) & (distance >= DC_EXCLUDE_HZ)
                # Blend overlaps in linear power with a raised-cosine taper, so each
                # capture's weight reaches exactly zero at its own edge instead of
                # leaving a seam where the hard cutoff in `valid` kicks in.
                weight = 0.5 * (1 + np.cos(np.pi * distance[valid] / USABLE_HALF_HZ))
                total[valid] += np.interp(grid[valid] - center, offsets, power) * weight
                weights[valid] += weight
                covered = weights > 0
                trace = np.full(grid.size, np.nan)
                trace[covered] = 10 * np.log10(np.maximum(
                    total[covered] / weights[covered], 1e-12))
                line.set_ydata(trace)
                status.set_text(f"Sweep {sweep_number + 1}: portion {index}/{len(centers)}"
                                f"  |  FFT/portion {FFT_SIZE}, total points {grid.size}"
                                f"  |  Avg {averages}"
                                + (f"  |  Last refresh {last_elapsed:.2f} s" if last_elapsed else ""))
                fig.canvas.draw_idle()
                plt.pause(0.01)
            sweep_number += 1
            websocket.publish(trace)
            previous.set_ydata(trace.copy())
            levels = spectrum_levels(grid, trace)
            if levels is not None:
                floor, plateau, lower, upper = levels
                target = np.array([lower, upper])
                if scale_limits is None:
                    scale_limits = target
                else:
                    # Expand immediately, contract slowly to avoid a jumping scale.
                    smoothed = 0.8 * scale_limits + 0.2 * target
                    scale_limits = np.array([min(lower, smoothed[0]),
                                             max(upper, smoothed[1])])
                ax.set_ylim(*scale_limits)
                levels_text.set_text(
                    f"Estimated floor: {floor:.1f} dB/bin  |  "
                    f"Beacon plateau: {plateau:.1f} dB/bin  |  Auto scale")
                fig.canvas.draw_idle()
            # Include the completed plot's rendering in the refresh measurement.
            if not plt.fignum_exists(fig.number):
                return
            fig.canvas.draw()
            last_elapsed = monotonic() - started
            if tuning["active"] and tuning["pending"] is None:
                tuning["times"].append(last_elapsed)
                tuning["captures"].append(capture_seconds)
                tuning_text.set_text(f"Auto tuning: measuring {len(tuning['times'])}/3 sweeps"
                                     f"  |  Target {tuning['target']:.2f} s")
                if len(tuning["times"]) == 3:
                    measured = float(np.median(tuning["times"]))
                    captured = float(np.median(tuning["captures"]))
                    error = abs(measured - tuning["target"])
                    if tuning["best"] is None or error < tuning["best"][0]:
                        tuning["best"] = (error, averages, measured)
                    proposed = next_averages(averages, measured, captured, tuning["target"])
                    tuning["round"] += 1
                    reached = error <= max(0.05, 0.05 * tuning["target"])
                    if reached or proposed == averages or tuning["round"] >= 8:
                        _, averages, best_time = tuning["best"]
                        tuning["active"] = False
                        result = "Target reached" if reached else "Closest measured setting; target not reached"
                        tuning_text.set_text(f"{result}: {best_time:.2f} s"
                                             f"  |  FFT {FFT_SIZE}, averages {averages}")
                    else:
                        averages = proposed
                        tuning["times"].clear()
                        tuning["captures"].clear()
                        tuning_text.set_text(f"Auto tuning: trying {averages} averages"
                                             f"  |  Target {tuning['target']:.2f} s")
    except KeyboardInterrupt:
        pass
    finally:
        if websocket is not None:
            websocket.close()
        sdr.close()
        plt.close("all")


if __name__ == "__main__":
    main()
