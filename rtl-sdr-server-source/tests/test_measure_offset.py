"""Tests for `main.py --measure-offset` (measure_beacon_offset) with a simulated stick.

The fake stick produces baseband IQ whose spectrum looks like the QO-100 beacon: a flat 1.5 MHz
plateau with smooth edges over a lower noise floor, placed where a beacon would arrive behind an
LNB whose real oscillator is `lo_error_hz` above the nominal 9750 MHz. With that error the beacon
arrives LOWER in IF than expected, so the measured correction must come out POSITIVE and equal to
the LNB error (same convention as --correction-khz).
"""
import threading
import unittest

import numpy as np

import main as app


class FakeStick:
    RATE = 2.4e6

    def __init__(self, lo_error_hz=26_000.0, beacon_present=True, seed=1):
        self.lo_error_hz = lo_error_hz
        self.beacon_present = beacon_present
        self.center_freq = 0.0
        self.sample_rate = self.RATE
        self.gain = 0.0
        self.closed = False
        self.rng = np.random.default_rng(seed)

    def set_agc_mode(self, enabled):
        assert not enabled          # the measurement needs a fixed gain

    def read_samples(self, count):
        noise = (self.rng.standard_normal(count) + 1j * self.rng.standard_normal(count)) / np.sqrt(2)
        freqs = np.fft.fftfreq(count, 1 / self.RATE)
        # Where the beacon really arrives, relative to the tuner centre.
        arrival_if = (app.BEACON_RF_MHZ - (app.LNB_LO_MHZ + self.lo_error_hz / 1e6)) * 1e6
        distance = np.abs(freqs - (arrival_if - self.center_freq))
        floor = 0.3                                  # about -10 dB below the plateau
        if self.beacon_present:
            t = np.clip((distance - 0.65e6) / 0.2e6, 0, 1)
            shape = floor + (1 - floor) * 0.5 * (1 + np.cos(np.pi * t))
        else:
            shape = np.full(count, floor)
        return np.fft.ifft(np.fft.fft(noise) * shape)

    def close(self):
        self.closed = True


class MeasureOffsetTests(unittest.TestCase):
    def run_measure(self, stick, captures=10, stop=None):
        lines = []
        status = app.measure_beacon_offset(stop or threading.Event(), captures, 10.0,
                                           lambda: stick, lines.append)
        return status, lines

    def test_correction_equals_the_lnb_error(self):
        stick = FakeStick(lo_error_hz=26_000.0)
        status, lines = self.run_measure(stick)
        self.assertEqual(status, 0, lines)
        result = [line for line in lines if line.startswith("RESULT")][-1]
        self.assertTrue(result.startswith("RESULT ok accepted="), result)
        median = float(result.split("median_khz=")[1].split()[0])
        self.assertAlmostEqual(median, 26.0, delta=3.0)   # positive: beacon arrives lower
        self.assertTrue(stick.closed)
        self.assertEqual(lines[0], "MEASURE start captures=10 gain=10")

    def test_negative_lnb_error_gives_negative_correction(self):
        status, lines = self.run_measure(FakeStick(lo_error_hz=-18_000.0))
        self.assertEqual(status, 0, lines)
        median = float(lines[-1].split("median_khz=")[1].split()[0])
        self.assertAlmostEqual(median, -18.0, delta=3.0)

    def test_no_beacon_is_reported_as_failure(self):
        stick = FakeStick(beacon_present=False)
        status, lines = self.run_measure(stick)
        self.assertEqual(status, 1)
        self.assertTrue(lines[-1].startswith("RESULT failed reason="), lines[-1])
        self.assertTrue(stick.closed)

    def test_cancel_stops_early_and_releases_the_stick(self):
        stop = threading.Event()
        stop.set()
        stick = FakeStick()
        status, lines = self.run_measure(stick, stop=stop)
        self.assertEqual(status, 2)
        self.assertEqual(lines[-1], "RESULT cancelled")
        self.assertTrue(stick.closed)

    def test_busy_stick_is_a_clear_failure(self):
        def busy():
            raise OSError("usb_claim_interface error -6")
        lines = []
        status = app.measure_beacon_offset(threading.Event(), 10, 10.0, busy, lines.append)
        self.assertEqual(status, 1)
        self.assertIn("cannot open the RTL-SDR", lines[-1])


if __name__ == "__main__":
    unittest.main()
