#!/usr/bin/env python3
"""Compare the beacon offset seen by the RTL-SDR stick with the one seen by the MiniTiouner.

Measurement model (both measured against the nominal beacon, 10,491.500 MHz):

  RTL correction relative to nominal 9750 MHz
      ~= LNB LO error + RTL-SDR reference error
  MiniTiouner-referenced LNB LO deviation
      ~= LNB LO error + MiniTiouner reference error
  RTL correction - MiniTiouner LO deviation
      ~= RTL-SDR reference error - MiniTiouner reference error      ("RTL-to-MiniTiouner
                                                                    reference difference")

The RTL measurement is deliberately referenced to the NOMINAL 9750.000 MHz LO and this cannot be
changed: feeding in the actual LO would remove the LNB error from the RTL figure and then
subtract it a second time in the comparison.

The final difference approximates the RTL-SDR's own error ONLY if the MiniTiouner is accepted as
the frequency reference.  It is not an independently verified absolute RTL-SDR error.  The
"common" component (LNB LO error, plus any deviation of the beacon from 10,491.500 MHz) cannot
be separated from either measurement.

Requirements
  * The RTL-SDR and the MiniTiouner must be fed from the SAME LNB oscillator (a splitter, or two
    outputs of one twin/quad LNB).  Two separate LNBs would make the subtraction meaningless.
  * The RTL server must be STOPPED: the RTL phase needs the stick to itself.
  * Let the LNB, the stick and the receiver warm up for at least 15-30 minutes first.
  * Run inside calibration-venv (needs numpy + pyrtlsdr).
  * With --minitiouner, longmynd must be running.  The MiniTiouner phases send tune commands, so
    the video drops and the app's beacon watchdog may also retune.

Phases
  RTL only (default):   30 captures from the stick.
  --minitiouner:        MiniTiouner (before)  ->  RTL  ->  MiniTiouner (after).
  Measuring the MiniTiouner both before and after the RTL phase brackets it in time, so slow LNB
  drift cancels in the comparison instead of showing up as a false RTL error; the difference
  between the two MiniTiouner phases is reported as the drift.

--if-khz (default 741474) is only the MiniTiouner's INITIAL ACQUISITION frequency used to obtain
lock.  It does not define the nominal reference or the calculated LNB LO (see measure_lnb_lo.py;
longmynd's whole-kHz rounding still gives the MiniTiouner figure +/-1 kHz).

Usage (calibration-venv):
  python3 rtl_offset_test.py                      # RTL phase only
  python3 rtl_offset_test.py --minitiouner        # bracketed comparison
  options: --n 30 --gain 10 --mt-n 10 and the MiniTiouner options of measure_lnb_lo.py
"""
import argparse, math, statistics, subprocess, sys, time
from argparse import Namespace
from pathlib import Path

import numpy as np
from rtlsdr import RtlSdr

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))                                                # measure_lnb_lo
sys.path.insert(0, str(HERE.parent / 'rtl-sdr-server-source' / 'tools'))     # calibrate_beacon
import measure_lnb_lo as mlo
import calibrate_beacon as cb   # capture_two_portions / estimate_offset, used unchanged

NOMINAL_LO_MHZ = 9750.0          # fixed on purpose, see docstring
MIN_ACCEPTED = 10                # same reliability rule as calibrate_beacon.py
MIN_ACCEPTANCE = 0.5
DRIFT_WARN_KHZ = 1.5             # MiniTiouner before/after difference worth warning about


def print_stats(title, d, unit="kHz", accepted=None):
    print(f"{title}")
    if accepted is not None:
        print(f"  accepted / attempts : {accepted[0]} / {accepted[1]}")
    print(f"  median              : {d['median']:+.2f} {unit}   (primary)")
    print(f"  mean                : {d['mean']:+.2f} {unit}   (supplementary)")
    print(f"  min / max           : {d['min']:+.2f} / {d['max']:+.2f} {unit}")
    print(f"  standard deviation  : {d['stdev']:.2f} {unit}   (sample)")
    print(f"  median abs. dev.    : {d['mad']:.2f} {unit}")
    if d["drift"] is not None:
        print(f"  early vs late drift : {d['drift']:+.2f} {unit}   (median last third - first third)")
    else:
        print(f"  early vs late drift : not meaningful with fewer than "
              f"{mlo.MIN_CYCLES_FOR_DRIFT} values")


# ---------------------------------------------------------------- RTL phase
def rtl_server_running():
    result = subprocess.run(["pgrep", "-x", "rtl-sdr-server"], capture_output=True)
    return result.returncode == 0


def rtl_phase(n, gain):
    """Returns the list of accepted corrections in kHz and the attempt count."""
    if rtl_server_running():
        raise SystemExit("The RTL server (rtl-sdr-server) is running. Stop it first: this phase "
                         "needs exclusive access to the stick.")
    center = cb.BEACON_RF_HZ - NOMINAL_LO_MHZ * 1e6
    offsets = cb.diagnostic_grid()
    window = np.hanning(cb.FFT_SIZE)
    try:
        sdr = RtlSdr()
    except Exception as error:      # librtlsdr raises several types for a busy/missing device
        raise SystemExit(f"Cannot open the RTL-SDR ({error}). Is the RTL server (or another "
                         "program) still using the stick?")
    good = []
    try:
        sdr.sample_rate = cb.SAMPLE_RATE
        sdr.center_freq = center
        sdr.set_agc_mode(False)
        sdr.gain = gain
        sdr.read_samples(65536)
        print(f"RTL phase: {n} captures, gain {gain:g} dB, referenced to the NOMINAL LO "
              f"{NOMINAL_LO_MHZ:g} MHz")
        print("time      #  correction_kHz  width_kHz  edge_spread_kHz  contrast_dB")
        for i in range(1, n + 1):
            power = cb.capture_two_portions(sdr, center, offsets, window)
            stamp = time.strftime('%H:%M:%S')
            try:
                r = cb.estimate_offset(offsets, power)
            except ValueError as error:
                print(f"{stamp}  {i:>2}  rejected: {error}")
                continue
            good.append(r['correction_hz'] / 1000.0)
            print(f"{stamp}  {i:>2}  {r['correction_hz'] / 1000:>+13.2f}  {r['width_hz'] / 1000:>9.1f}  "
                  f"{r['edge_spread_hz'] / 1000:>15.2f}  {r['contrast_db']:>10.1f}")
    except KeyboardInterrupt:
        print("\nInterrupted; using what was captured so far.")
    finally:
        sdr.close()
    return good, n


def evaluate_rtl(good, attempts):
    """Return the RTL statistics, or None when the result is not reliable."""
    print()
    if len(good) < MIN_ACCEPTED or len(good) < attempts * MIN_ACCEPTANCE:
        print(f"RTL result NOT reliable: accepted {len(good)} / {attempts}; need at least "
              f"{MIN_ACCEPTED} accepted and {MIN_ACCEPTANCE:.0%} acceptance "
              "(is the beacon visible and the LNB powered?).")
        return None
    d = mlo.describe(good)
    print_stats("RTL correction relative to nominal 9750 MHz", d, accepted=(len(good), attempts))
    print("  (positive = beacon appears LOWER than expected, i.e. the real LO is higher)")
    return d


# ---------------------------------------------------------------- MiniTiouner phase
def mt_phase(label, args):
    """One MiniTiouner phase; returns (LO deviation kHz from the median IF, IF statistics)."""
    ns = Namespace(n=args.mt_n, if_khz=args.if_khz, sr=args.sr, settle=args.settle,
                   window=args.window, hop_khz=args.hop_khz)
    print(f"\nMiniTiouner phase ({label}) started {time.strftime('%H:%M:%S')}")
    with mlo.connections(args.host, args.port) as (ctl, mon):
        measured = mlo.run_measurements(ctl, mon, ns)
    if len(measured) < min(3, args.mt_n):
        print(f"MiniTiouner phase ({label}): only {len(measured)} valid acquisitions - not usable.")
        return None
    d = mlo.describe(measured)
    deviation = (mlo.lo_mhz(d["median"]) - NOMINAL_LO_MHZ) * 1000.0
    print(f"\nMiniTiouner ({label}): {len(measured)} valid, median IF {d['median']:.1f} kHz "
          f"(mean {d['mean']:.2f}, min {d['min']:.0f}, max {d['max']:.0f}, "
          f"stdev {d['stdev']:.2f}, MAD {d['mad']:.2f}) -> LO deviation {deviation:+.1f} kHz")
    return deviation


# ---------------------------------------------------------------- arguments / main
def parse_args():
    ap = argparse.ArgumentParser(
        description="RTL-SDR vs MiniTiouner beacon offsets (same LNB, nominal 9750 MHz reference).")
    ap.add_argument("--n", type=mlo.in_range(1, 10**6, int), default=30,
                    help="RTL captures (default 30; at least 10 must be accepted)")
    ap.add_argument("--gain", type=float, default=10, help="RTL gain in dB (default 10)")
    ap.add_argument("--minitiouner", action="store_true",
                    help="also run the MiniTiouner phases (before and after the RTL phase)")
    mlo.add_receiver_arguments(ap, n_dest="mt_n", n_default=10,
                               n_help="MiniTiouner acquisitions per phase")
    a = ap.parse_args()
    if not math.isfinite(a.gain):
        ap.error("--gain must be a finite number")
    mlo.check_receiver_arguments(ap, a)
    return a


def main():
    a = parse_args()
    print(f"Started {time.strftime('%Y-%m-%d %H:%M:%S')}")
    print("Same LNB required for both receivers; LNB and receiver should have warmed up for "
          "15-30 minutes.\n")

    before = after = None
    try:
        if a.minitiouner:
            before = mt_phase("before RTL", a)
            print()
        good, attempts = rtl_phase(a.n, a.gain)
        rtl = evaluate_rtl(good, attempts)
        if a.minitiouner:
            after = mt_phase("after RTL", a)
    except (OSError, EOFError, ConnectionError) as error:
        print(f"Cannot talk to longmynd at {a.host}:{a.port}: {error}", file=sys.stderr)
        return 1

    print("\n================ SUMMARY ================")
    if rtl is None:
        print("RTL correction relative to nominal 9750 MHz:       not reliable - no comparison made")
        return 1
    rtl_khz = rtl["median"]
    print(f"RTL correction relative to nominal 9750 MHz:       {rtl_khz:+7.1f} kHz")
    phases = [v for v in (before, after) if v is not None]
    if not phases:
        print("MiniTiouner-referenced LNB LO deviation:            not measured "
              "(use --minitiouner)")
        return 0
    mt_khz = statistics.mean(phases)
    print(f"MiniTiouner-referenced LNB LO deviation:           {mt_khz:+7.1f} kHz"
          + (f"   (mean of before {before:+.1f} and after {after:+.1f})" if len(phases) == 2 else ""))
    diff = rtl_khz - mt_khz
    print(f"RTL-to-MiniTiouner reference difference:           {diff:+7.1f} kHz")
    if len(phases) == 2:
        drift = after - before
        print(f"LNB/MiniTiouner drift during the run (after - before): {drift:+.1f} kHz"
              + ("   <-- large: the comparison is less reliable" if abs(drift) > DRIFT_WARN_KHZ else ""))

    print("\nInterpretation:")
    print(f"  The common component of about {mt_khz:+.1f} kHz is attributed mainly to the LNB LO error")
    print("  (plus any deviation of the beacon from 10,491.500 MHz; the two cannot be separated).")
    print(f"  The remaining {diff:+.1f} kHz is the RTL-SDR frequency-reference error relative to the")
    print("  MiniTiouner. It equals the RTL-SDR's own error only if the MiniTiouner is accepted as")
    print("  the frequency reference; it is not an independently verified absolute error.")
    print("  Resolution: the MiniTiouner figure carries about +/-1 kHz (longmynd rounding).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
