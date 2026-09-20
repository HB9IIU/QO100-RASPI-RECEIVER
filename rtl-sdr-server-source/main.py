"""Run this file to start the headless RTL-SDR WebSocket server.

Edit capture settings below. Open WEBpage/index.html for the display.
"""
import logging
import argparse
import math
import signal
import socket
import statistics
import sys
import threading
from pathlib import Path
from time import monotonic

import numpy as np
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


# ---------------------------------------------------------------- beacon offset measurement
# `--measure-offset` is a one-shot mode used by the receiver app's LNB/RTL calibration. It does
# NOT serve a spectrum: it opens the stick, measures where the QO-100 beacon really appears
# relative to where it should (10,491.500 MHz behind the nominal 9750 MHz LNB LO), prints the
# answer and exits. The stick must be free, so the normal server has to be stopped first.
#
# The measurement is scripts/calibrate_beacon.py's own, unchanged: two overlapping captures per
# reading, and the beacon centre taken as the midpoint of its two 50% edges. Each reading is a
# "correction": positive means the beacon appears LOWER than expected on the display grid (the
# same convention as --correction-khz, so the median can be passed straight back to it).
#
# Output is one line per event on stdout (flushed), meant to be parsed by the app:
#   MEASURE start captures=<n> gain=<db>
#   CAPTURE <i>/<n> ok correction_khz=<+x.xx> width_khz=<x> contrast_db=<x>
#   CAPTURE <i>/<n> rejected reason=<text>
#   RESULT ok accepted=<a> attempts=<n> median_khz=<+x.xx> mean_khz=<+x.xx> stdev_khz=<x.xx>
#   RESULT failed reason=<text>
#   RESULT cancelled
# Exit status: 0 = ok, 1 = failed, 2 = cancelled.
MEASURE_MIN_ACCEPTED = 10       # same reliability rule as calibrate_beacon.py
MEASURE_MIN_ACCEPTANCE = 0.5
MEASURE_DEFAULT_CAPTURES = 30


def _load_beacon_measurement():
    """calibrate_beacon lives in tools/ beside this file when run from source; the frozen
    binary carries it inside (build_binary.sh passes --paths tools)."""
    tools = Path(__file__).resolve().parent / 'tools'
    if tools.is_dir() and str(tools) not in sys.path:
        sys.path.insert(0, str(tools))
    import calibrate_beacon
    return calibrate_beacon


def measure_beacon_offset(stop, captures=MEASURE_DEFAULT_CAPTURES, gain=GAIN_DB,
                          sdr_factory=RtlSdr, emit=None):
    """Run the measurement; returns the exit status described above."""
    if emit is None:
        def emit(line):
            print(line, flush=True)
    cb = _load_beacon_measurement()
    # Deliberately referenced to the NOMINAL LO: the LNB's own error must stay in the result.
    center = cb.BEACON_RF_HZ - LNB_LO_MHZ * 1e6
    offsets = cb.diagnostic_grid()
    window = np.hanning(cb.FFT_SIZE)
    emit(f"MEASURE start captures={captures} gain={gain:g}")
    try:
        sdr = sdr_factory()
    except Exception as error:   # librtlsdr raises several types for a busy/missing device
        emit("RESULT failed reason=" + " ".join(f"cannot open the RTL-SDR: {error}".split()))
        return 1
    corrections = []
    try:
        sdr.sample_rate = cb.SAMPLE_RATE
        sdr.center_freq = center
        sdr.set_agc_mode(False)
        sdr.gain = gain
        sdr.read_samples(65536)   # flush the first, unsettled samples
        for index in range(1, captures + 1):
            if stop.is_set():
                emit("RESULT cancelled")
                return 2
            power = cb.capture_two_portions(sdr, center, offsets, window)
            try:
                reading = cb.estimate_offset(offsets, power)
            except ValueError as error:
                emit(f"CAPTURE {index}/{captures} rejected reason=" + " ".join(str(error).split()))
                continue
            correction_khz = reading['correction_hz'] / 1000.0
            corrections.append(correction_khz)
            emit(f"CAPTURE {index}/{captures} ok correction_khz={correction_khz:+.2f} "
                 f"width_khz={reading['width_hz'] / 1000:.1f} contrast_db={reading['contrast_db']:.1f}")
    except Exception as error:
        emit("RESULT failed reason=" + " ".join(f"measurement error: {error}".split()))
        return 1
    finally:
        try:
            sdr.close()
        except Exception:
            pass
    accepted = len(corrections)
    if accepted < MEASURE_MIN_ACCEPTED or accepted < captures * MEASURE_MIN_ACCEPTANCE:
        emit(f"RESULT failed reason=only {accepted} of {captures} captures were usable "
             "(is the beacon visible and the LNB powered?)")
        return 1
    stdev = statistics.stdev(corrections) if accepted > 1 else 0.0
    emit(f"RESULT ok accepted={accepted} attempts={captures} "
         f"median_khz={statistics.median(corrections):+.2f} "
         f"mean_khz={statistics.mean(corrections):+.2f} stdev_khz={stdev:.2f}")
    return 0


def websocket_addresses(port):
    lines = [f"    This computer     ws://localhost:{port}/"]
    try:
        # UDP connect selects the default outbound interface; no packet is sent.
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.connect(('192.0.2.1', 9))
            address = probe.getsockname()[0]
        if not address.startswith('127.') and address != '0.0.0.0':
            lines.append(f"    Network address   ws://{address}:{port}/")
    except OSError:
        pass  # Offline: the local endpoint remains available.
    return '\n'.join(lines)


def print_banner():
    print("""
======================================================================
                       QO-100 SPECTRUM BRIDGE
                  RTL-SDR wideband WebSocket server
----------------------------------------------------------------------
  Scans the QO-100 wideband transponder in overlapping portions,
  combines them into a 922-point spectrum, and streams each completed
  sweep to connected WebSocket displays. Spectrum only; no DATV decode.

  airspy_fft_ws-compatible WebSocket format and frequency grid.

  Author    Daniel HB9IIU
  Date      September 2026
  GitHub    http://github.com/hb9iiu
======================================================================
""", flush=True)


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



def run(stop, sdr_factory=RtlSdr, server_factory=SpectrumServer, correction_hz=0.0,
        ws_port=7681):
    if not np.isfinite(correction_hz):
        raise ValueError('Frequency correction must be finite')
    if not 1 <= ws_port <= 65535:
        raise ValueError('WebSocket port must be between 1 and 65535')
    grid, centers = sweep_plan()
    offsets = np.fft.fftshift(np.fft.fftfreq(FFT_SIZE, 1 / SAMPLE_RATE))
    window = np.hanning(FFT_SIZE)
    portions = []
    weight_sum = np.zeros(grid.size)
    for center in centers:
        distance = np.abs(grid - center)
        valid = (distance <= USABLE_HALF_HZ) & (distance >= DC_EXCLUDE_HZ)
        weight = 0.5 * (1 + np.cos(np.pi * distance[valid] / USABLE_HALF_HZ))
        weight_sum[valid] += weight
        portions.append((center, valid, weight))
    if np.any(weight_sum <= 0):
        raise ValueError("Sweep settings leave gaps in the 922-point output grid")

    server = server_factory()
    server.port = ws_port
    sdr = None
    try:
        server.start()
        sdr = sdr_factory()
        sdr.sample_rate = SAMPLE_RATE
        sdr.set_agc_mode(False)
        sdr.gain = GAIN_DB
        print(f"""  RECEIVER
    Sample rate       {SAMPLE_RATE / 1e6:.3f} MS/s
    FFT / averaging   {FFT_SIZE} bins / {AVERAGES} averages
    Tuner gain        {sdr.gain:.1f} dB actual ({GAIN_DB:.1f} dB requested; AGC off)
    LNB oscillator    {LNB_LO_MHZ:.3f} MHz
    Correction        {correction_hz / 1000:+.3f} kHz (subtracted from tuner frequency)

  SPECTRUM
    Downlink range    {RF_START_MHZ:.3f} - {RF_STOP_MHZ:.3f} MHz nominal
    Output IF range   {grid[0] / 1e6:.6f} - {grid[-1] / 1e6:.6f} MHz
    Sweep             {len(centers)} portions -> {grid.size} output points

  CONNECTION
{websocket_addresses(ws_port)}
    Binary message    1,844 bytes per completed sweep (922 x uint16)

  Press Ctrl+C to stop. Progress is reported about every 10 seconds.
----------------------------------------------------------------------
""", flush=True)
        last_report = monotonic()
        count = 0
        while not stop.is_set():
            started = monotonic()
            total = np.zeros(grid.size)
            for center, valid, weight in portions:
                if stop.is_set():
                    return  # Never publish a partial sweep.
                sdr.center_freq = center - correction_hz
                sdr.read_samples(65536)
                power = capture_power(sdr, window)
                total[valid] += np.interp(grid[valid] - center, offsets, power) * weight
            if stop.is_set():
                return
            trace = 10 * np.log10(np.maximum(total / weight_sum, 1e-12))
            server.publish(trace)
            count += 1
            if count == 1 or monotonic() - last_report >= 10:
                logging.info("STREAMING | Sweep %6d | Refresh %.3f s | 922 points sent",
                             count, monotonic() - started)
                last_report = monotonic()
    finally:
        try:
            if sdr is not None:
                sdr.close()
        finally:
            server.close()


def main():
    parser = argparse.ArgumentParser(description="QO-100 Spectrum Bridge — RTL-SDR WebSocket server")
    parser.add_argument('--self-test', action='store_true', help='Check bundled libraries and WebSocket delivery without opening the SDR')
    parser.add_argument('--correction-khz', type=float, default=0.0,
                        help='Frequency correction in kHz; positive tunes lower and moves signals higher on the fixed display grid (default: 0)')
    parser.add_argument('--ws-port', type=int, default=7681,
                        help='WebSocket listening port, 1–65535 (default: 7681)')
    parser.add_argument('--measure-offset', action='store_true',
                        help='One-shot: measure the beacon offset seen by this stick and print '
                             'the result (see measure_beacon_offset); serves no spectrum')
    parser.add_argument('--captures', type=int, default=MEASURE_DEFAULT_CAPTURES,
                        help='Captures for --measure-offset, 10-1000 (default: 30)')
    parser.add_argument('--gain', type=float, default=GAIN_DB,
                        help='Tuner gain in dB for --measure-offset (default: 10)')
    args = parser.parse_args()
    if not 10 <= args.captures <= 1000:
        parser.error('--captures must be between 10 and 1000')
    if not math.isfinite(args.gain):
        parser.error('--gain must be finite')
    if not np.isfinite(args.correction_khz):
        parser.error('--correction-khz must be finite')
    if not 1 <= args.ws_port <= 65535:
        parser.error('--ws-port must be between 1 and 65535')
    if args.measure_offset:
        stop = threading.Event()
        signal.signal(signal.SIGINT, lambda _signum, _frame: stop.set())
        signal.signal(signal.SIGTERM, lambda _signum, _frame: stop.set())
        return measure_beacon_offset(stop, args.captures, args.gain)
    if args.self_test:
        from websockets.sync.client import connect
        # websockets 15 can expose an empty Server.sockets tuple when asked
        # to bind port 0 directly, even though fixed-port servers work
        # normally. Reserve an ephemeral port first, then exercise the same
        # fixed-port startup path used by the real server.
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
            probe.bind(('127.0.0.1', 0))
            self_test_port = probe.getsockname()[1]
        server = SpectrumServer('127.0.0.1', self_test_port)
        try:
            server.start()
            with connect(f'ws://127.0.0.1:{server.port}', subprotocols=['fft'], proxy=None) as client:
                server.publish(np.full(922, -60.0))
                payload = client.recv(timeout=5)
                if len(payload) != 1844:
                    raise RuntimeError('Unexpected WebSocket payload length')
            print('PASS: native RTL-SDR library loaded, calibration loaded, 1844-byte WebSocket message received')
        finally:
            server.close()
        return 0
    logging.basicConfig(level=logging.INFO, format="%(asctime)s | %(message)s", datefmt="%H:%M:%S")
    logging.getLogger('websockets').setLevel(logging.WARNING)
    print_banner()
    logging.info("STARTING  | Opening receiver and WebSocket server...")
    stop = threading.Event()
    def request_stop(_signum, _frame):
        stop.set()
    signal.signal(signal.SIGINT, request_stop)
    signal.signal(signal.SIGTERM, request_stop)
    try:
        run(stop, correction_hz=args.correction_khz * 1000, ws_port=args.ws_port)
    except Exception:
        logging.exception("RTL-SDR server stopped due to an error")
        return 1
    logging.info("STOPPED   | Receiver and WebSocket server closed. Goodbye, 73!")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
