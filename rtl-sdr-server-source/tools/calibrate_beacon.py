"""Measure QO-100 WB beacon offset with two overlapping RTL-SDR captures.
Stop main.py / the compiled server before running. No settings are modified.
"""
import argparse
import csv
from datetime import datetime
import json
from pathlib import Path
import time

import numpy as np
from rtlsdr import RtlSdr

SAMPLE_RATE = 2_400_000
FFT_SIZE = 4096
AVERAGES = 64
BEACON_RF_HZ = 10_491_500_000
PORTION_OFFSETS_HZ = (-400_000, 400_000)
USABLE_HALF_HZ = 1_050_000


def diagnostic_grid():
    step = SAMPLE_RATE / FFT_SIZE
    return np.arange(-int(1_400_000 / step), int(1_400_000 / step) + 1) * step


def capture_two_portions(sdr, center, offsets, window):
    bins = np.fft.fftshift(np.fft.fftfreq(FFT_SIZE, 1 / SAMPLE_RATE))
    total = np.zeros(offsets.size)
    weights = np.zeros(offsets.size)
    for shift in PORTION_OFFSETS_HZ:
        sdr.center_freq = center + shift
        sdr.read_samples(65536)  # Flush samples from the previous tuning.
        blocks = np.asarray(sdr.read_samples(FFT_SIZE * AVERAGES)).reshape(-1, FFT_SIZE)
        blocks = blocks - blocks.mean(axis=1, keepdims=True)
        fft = np.fft.fftshift(np.fft.fft(blocks * window, axis=1), axes=1)
        power = np.mean(np.abs(fft) ** 2, axis=0)
        relative = offsets - shift
        valid = (np.abs(relative) < USABLE_HALF_HZ) & (np.abs(relative) > 10_000)
        weight = .5 * (1 + np.cos(np.pi * relative[valid] / USABLE_HALF_HZ))
        total[valid] += np.interp(relative[valid], bins, power) * weight
        weights[valid] += weight
    if np.any(weights <= 0):
        raise ValueError('Two-portion capture has uncovered frequency points')
    return total / weights


def estimate_offset(offsets, power):
    """Local half-power edges, with conservative width/passband/confidence gates."""
    smooth = np.convolve(power, np.ones(21) / 21, mode='same')
    centers = []
    edges = None
    contrasts = []
    for fraction in (0.35, 0.5, 0.65):
        pair = []
        for side in (-1, 1):
            distance = offsets * side
            floor = np.median(smooth[(distance > 1.07e6) & (distance < 1.12e6)])
            plateau = np.median(smooth[(distance > .25e6) & (distance < .55e6)])
            contrast = 10 * np.log10(max(plateau, 1e-30) / max(floor, 1e-30))
            if contrast < 3:
                raise ValueError('Beacon contrast below 3 dB or passband edges not clear')
            threshold = floor + fraction * (plateau - floor)
            indices = np.flatnonzero((distance > .60e6) & (distance < 1.05e6))
            if side == -1:
                indices = indices[::-1]  # Always walk outward from the beacon.
            candidates = []
            for a, b in zip(indices[:-1], indices[1:]):
                if smooth[a] >= threshold > smooth[b]:
                    crossing = offsets[a] + (offsets[b] - offsets[a]) * (
                        (threshold - smooth[a]) / (smooth[b] - smooth[a]))
                    candidates.append(crossing)
            if len(candidates) != 1:
                raise ValueError(f"{'Left' if side == -1 else 'Right'} edge: "
                                 f'{len(candidates)} crossings at {fraction:.0%} threshold')
            pair.append(candidates[0])
            if fraction == .5:
                contrasts.append(float(contrast))
        centers.append(sum(pair) / 2)
        if fraction == .5:
            edges = pair
    width = edges[1] - edges[0]
    if not 1.4e6 <= width <= 2.05e6:
        raise ValueError('Detected beacon width is implausible')
    spread = max(centers) - min(centers)
    if spread > 25_000:
        raise ValueError('Asymmetric/distorted edges: center depends too strongly on threshold')
    return {'correction_hz': float(-centers[1]), 'width_hz': float(width),
            'edge_spread_hz': float(spread), 'contrast_db': min(contrasts)}


def summarize(rows, attempts):
    if len(rows) < 10 or len(rows) < attempts * .5:
        return {'reliable': False, 'accepted': len(rows), 'attempts': attempts,
                'reason': 'Need at least 10 accepted measurements and 50% acceptance'}
    values = np.array([row['correction_hz'] for row in rows])
    times = np.array([row['elapsed_s'] for row in rows])
    count = max(1, len(rows) // 5)
    drift = np.median(values[-count:]) - np.median(values[:count])
    return {'reliable': True, 'accepted': len(rows), 'attempts': attempts,
            'recommended_correction_hz': int(round(float(np.median(values)) / 1000) * 1000),
            'percentile_10_90_hz': np.percentile(values, [10, 90]).tolist(),
            'late_minus_early_hz': float(drift),
            'linear_drift_hz_per_minute': float(np.polyfit(times, values, 1)[0] * 60),
            'median_edge_threshold_spread_hz': float(np.median([r['edge_spread_hz'] for r in rows])),
            'note': 'Combined LNB and SDR offset; statistical repeatability is not absolute accuracy.'}


class DiagnosticPlot:
    def __init__(self, offsets, center):
        import matplotlib
        matplotlib.use('QtAgg')
        import matplotlib.pyplot as plt
        self.plt = plt
        self.offsets = offsets
        self.fig, self.ax = plt.subplots(figsize=(12, 6))
        self.fig.subplots_adjust(bottom=.24)
        self.raw, = self.ax.plot(offsets / 1000, np.zeros_like(offsets),
                                 alpha=.35, label='Measured spectrum')
        self.smoothed, = self.ax.plot([], [], label='Smoothed spectrum')
        self.ax.axvline(0, color='black', ls=':', label='Nominal beacon center')
        self.thresholds = []
        for side in (-1, 1):
            bounds = sorted([side * 600, side * 1050])
            self.ax.axvspan(*bounds, color='orange', alpha=.12,
                           label='Edge search' if side == -1 else None)
            self.ax.axvspan(*sorted([side * 1070, side * 1120]), color='gray', alpha=.2,
                           label='Noise measurement' if side == -1 else None)
            self.ax.axvspan(*sorted([side * 250, side * 550]), color='green', alpha=.08,
                           label='Plateau measurement' if side == -1 else None)
            for fraction in (.35, .5, .65):
                line, = self.ax.plot(bounds, [0, 0], color='red',
                                    ls='-' if fraction == .5 else '--', alpha=.6,
                                    label='35/50/65% power thresholds' if side == -1 and fraction == .5 else None)
                self.thresholds.append((side, fraction, line))
        self.ax.set(xlim=(offsets[0]/1000, offsets[-1]/1000),
                    xlabel=f'Offset from {center/1e6:.6f} MHz LNB output (kHz)',
                    ylabel='Relative power (dB/bin)',
                    title='Beacon diagnostics — two overlapping captures — close window to stop')
        self.ax.grid(alpha=.2)
        self.ax.legend(loc='upper right', fontsize=8)
        self.status = self.fig.text(.08, .06, 'Waiting for samples', fontsize=10)
        plt.show(block=False)

    def update(self, power, row, accepted, attempts):
        smooth = np.convolve(power, np.ones(21)/21, mode='same')
        normalization = FFT_SIZE * np.sum(np.hanning(FFT_SIZE)**2)
        def db(values):
            return 10 * np.log10(np.maximum(values / normalization, 1e-30))
        self.raw.set_ydata(db(power))
        self.smoothed.set_data(self.offsets/1000, db(smooth))
        for side, fraction, line in self.thresholds:
            distance = self.offsets * side
            floor = np.median(smooth[(distance > 1.07e6) & (distance < 1.12e6)])
            plateau = np.median(smooth[(distance > .25e6) & (distance < .55e6)])
            threshold = db(floor + fraction*(plateau-floor))
            line.set_ydata([threshold, threshold])
        values = db(smooth)[20:-20]
        self.ax.set_ylim(np.percentile(values, 1)-4, np.percentile(values, 99)+5)
        result = row['rejection'] or f"Accepted: correction {row['correction_hz']/1000:+.1f} kHz"
        self.status.set_text(f"{row['elapsed_s']:.0f}s — accepted {accepted}/{attempts}\n{result}")
        self.fig.canvas.draw_idle()
        self.plt.pause(.001)

    def is_open(self):
        return self.plt.fignum_exists(self.fig.number)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--minutes', type=float, default=5)
    parser.add_argument('--lnb-lo-mhz', type=float, default=9750)
    parser.add_argument('--gain', type=float, default=10)
    parser.add_argument('--no-plot', action='store_true', help='Disable the live diagnostic window')
    parser.add_argument('--output-dir', type=Path,
                        default=Path(__file__).resolve().parents[1] / 'reports')
    args = parser.parse_args()
    if not np.isfinite(args.minutes) or args.minutes <= 0:
        parser.error('--minutes must be positive')
    center = BEACON_RF_HZ - args.lnb_lo_mhz * 1e6
    offsets = diagnostic_grid()
    window = np.hanning(FFT_SIZE)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    prefix = args.output_dir / ('beacon-' + datetime.now().strftime('%Y%m%d-%H%M%S-%f'))
    rows = []
    attempts = 0
    sdr = None
    plot = None
    started = time.monotonic()
    print('Stop the receiver server first. Calibration requires exclusive access to the stick.')
    print(f'Two captures centered at {(center-400000)/1e6:.6f} and '
          f'{(center+400000)/1e6:.6f} MHz; 2.4 MS/s; {args.minutes:g} minutes.')
    try:
        sdr = RtlSdr()
        sdr.sample_rate = SAMPLE_RATE
        sdr.center_freq = center
        sdr.set_agc_mode(False)
        sdr.gain = args.gain
        sdr.read_samples(65536)
        if not args.no_plot:
            plot = DiagnosticPlot(offsets, center)
        started = time.monotonic()
        with prefix.with_suffix('.csv').open('w', newline='') as handle:
            fields = ['elapsed_s', 'correction_hz', 'width_hz', 'edge_spread_hz', 'contrast_db', 'rejection']
            writer = csv.DictWriter(handle, fieldnames=fields)
            writer.writeheader()
            while time.monotonic() - started < args.minutes * 60:
                if plot is not None and not plot.is_open():
                    break
                power = capture_two_portions(sdr, center, offsets, window)
                elapsed = time.monotonic() - started
                attempts += 1
                try:
                    result = estimate_offset(offsets, power)
                    row = {'elapsed_s': elapsed, **result, 'rejection': ''}
                    rows.append(row)
                except ValueError as error:
                    row = {'elapsed_s': elapsed, 'rejection': str(error)}
                writer.writerow(row)
                if plot is not None:
                    plot.update(power, row, len(rows), attempts)
                if attempts % 50 == 0:
                    handle.flush()
                    message = f'{elapsed:.0f}s: accepted {len(rows)}/{attempts}'
                    if rows:
                        message += f", median correction {np.median([r['correction_hz'] for r in rows])/1000:+.1f} kHz"
                    print(message, flush=True)
    except KeyboardInterrupt:
        print('\nStopped early; summarizing available measurements.')
    finally:
        if sdr is not None:
            sdr.close()
        if plot is not None:
            plot.plt.close(plot.fig)
    report = summarize(rows, attempts)
    report.update(duration_s=time.monotonic() - started, lnb_lo_mhz=args.lnb_lo_mhz,
                  sample_rate=SAMPLE_RATE, fft_size=FFT_SIZE, gain_db=args.gain,
                  portion_offsets_hz=PORTION_OFFSETS_HZ)
    prefix.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    if report['reliable']:
        print(f"Recommended display correction: {report['recommended_correction_hz']/1000:+.0f} kHz")
        print(f"Late minus early drift: {report['late_minus_early_hz']/1000:+.1f} kHz")
        print('Positive correction means tune LOWER by this amount for the unchanged output frequency grid.')
        print('This estimate assumes symmetric beacon edges; verify before applying. No settings changed.')
    else:
        print('No reliable correction: ' + report['reason'])
    print(f'Results: {prefix}.csv and {prefix}.json')


if __name__ == '__main__':
    main()
