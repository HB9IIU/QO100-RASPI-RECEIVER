#!/usr/bin/env python3
"""Estimate the actual LNB local-oscillator frequency (nominally 9750 MHz).

Method
  The QO-100 wideband beacon is taken to be at exactly 10,491.500 MHz.  The MiniTiouner
  locks onto it and longmynd reports the carrier's measured IF.  Therefore:

      estimated LNB LO = 10,491.500 MHz - measured beacon IF

  Each measurement first tunes well away from the beacon (so any existing lock is broken),
  then tunes back and waits for a fresh lock, so every reading is an independent acquisition.

  --if-khz (default 741474) is only the INITIAL ACQUISITION frequency used to obtain lock.
  It does not determine the LO estimate, which comes from the IF the demodulator measures.
  One small exception: longmynd rounds the reported carrier offset to whole kHz relative to
  the requested frequency, so the measured IF carries a quantisation uncertainty of up to
  +/-1 kHz, whatever the acquisition frequency.

Limits of the result
  This is a MiniTiouner-referenced operational estimate of the LNB LO.  It is not an
  independently verified absolute-frequency measurement: it assumes the beacon really is at
  10,491.500 MHz and it includes any error of the MiniTiouner's own reference.

Operation
  Let the LNB warm up for 15-30 minutes before measuring; its oscillator drifts while warming.
  This script SENDS tune commands to longmynd: the video drops on every acquisition, and the
  app's own beacon watchdog may retune at the same time.  longmynd must be running.

Usage: python3 measure_lnb_lo.py [--n 10] [--if-khz 741474] [--sr 1500] [--settle 3]
                                 [--window 2] [--hop-khz 3000] [--host 127.0.0.1] [--port 8765]
"""
import argparse, base64, contextlib, json, math, os, socket, statistics, struct, sys, time

BEACON_RF_MHZ = 10491.500
NOMINAL_LO_MHZ = 9750.0
MIN_CYCLES_FOR_DRIFT = 9            # below this, early-vs-late is not meaningful

# longmynd's accepted ranges (longmynd_ws/main.c)
FREQ_MIN_KHZ, FREQ_MAX_KHZ = 144000, 2450000
SR_MIN_KSPS, SR_MAX_KSPS = 33, 27500


# ------------------------------------------------------------------ websocket
def connect(host, port, protocol):
    s = socket.create_connection((host, port), timeout=5)
    try:
        key = base64.b64encode(os.urandom(16)).decode()
        s.sendall((f"GET / HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\n"
                   f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
                   f"Sec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: {protocol}\r\n\r\n").encode())
        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = s.recv(1024)
            if not chunk:
                raise ConnectionError("connection closed during handshake")
            buf += chunk
        head, rest = buf.split(b"\r\n\r\n", 1)
        if b" 101 " not in head.split(b"\r\n")[0] + b" ":
            raise ConnectionError("handshake failed: " + head.decode(errors="replace"))
    except BaseException:
        s.close()
        raise
    return s, rest


def send_text(s, text):
    data = text.encode()
    mask = os.urandom(4)
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
    s.sendall(bytes([0x81, 0x80 | len(data)]) + mask + masked)  # short frames only


class Monitor:
    def __init__(self, s, buf):
        self.s, self.buf = s, buf
        s.settimeout(1.0)

    def _need(self, n):
        while len(self.buf) < n:
            chunk = self.s.recv(65536)
            if not chunk:
                raise EOFError("monitor connection closed")
            self.buf += chunk

    def next(self):
        """Next status packet as (server_timestamp_s, rx dict); None after a 1 s silence."""
        while True:
            try:
                self._need(2)
                op, ln = self.buf[0] & 0x0F, self.buf[1] & 0x7F
                pos = 2
                if ln == 126:
                    self._need(4); ln = struct.unpack(">H", self.buf[2:4])[0]; pos = 4
                elif ln == 127:
                    self._need(10); ln = struct.unpack(">Q", self.buf[2:10])[0]; pos = 10
                self._need(pos + ln)
            except socket.timeout:
                return None
            payload, self.buf = self.buf[pos:pos + ln], self.buf[pos + ln:]
            if op == 1:
                doc = json.loads(payload.decode())
                rx = doc.get("packet", {}).get("rx")
                if rx:
                    return float(doc.get("timestamp", time.time())), rx


def is_locked(rx):
    return int(rx.get("demod_state", -1)) in (3, 4)


# ------------------------------------------------------------------ measurement
def hop_away(ctl, mon, a):
    """Tune away from the beacon and wait for >= 1 s without lock. True if it worked."""
    send_text(ctl, f"C{a.if_khz + a.hop_khz},{a.sr}")
    start, unlocked_since = time.time(), None
    while time.time() - start < 8:
        pkt = mon.next()
        if pkt is None:
            continue
        if not is_locked(pkt[1]):
            unlocked_since = unlocked_since or time.time()
            if time.time() - unlocked_since >= 1.0:
                return True
        else:
            unlocked_since = None
    return False


def acquire_and_measure(ctl, mon, a):
    """One fresh acquisition. Returns (result dict, None) or (None, reason)."""
    send_text(ctl, f"C{a.if_khz},{a.sr}")
    t0, lock_ts = time.time(), None
    while time.time() - t0 < 40:
        pkt = mon.next()
        if pkt is not None and is_locked(pkt[1]):
            lock_ts, lock_s = pkt[0], time.time() - t0
            break
    if lock_ts is None:
        return None, "no lock within 40 s"

    # Settling: keep reading and discarding.  Packets are judged by the SERVER timestamp they
    # carry, so anything queued before the settling period ended can never enter the window.
    window_start, window_end = lock_ts + a.settle, lock_ts + a.settle + a.window
    samples, lost = [], False
    deadline = time.time() + a.settle + a.window + 10
    while time.time() < deadline:
        pkt = mon.next()
        if pkt is None:
            continue
        ts, rx = pkt
        if ts >= window_end:
            break
        if ts < window_start:
            continue
        if is_locked(rx):
            samples.append(rx)
        else:
            lost = True
    if lost:
        return None, "lock lost during measurement window"
    if len(samples) < 3:
        return None, f"only {len(samples)} status packets in the window"

    med = lambda k: statistics.median(float(r.get(k, 0)) for r in samples)
    ldpc = [float(r.get("errors_ldpc_count", 0)) for r in samples]
    return {"lock_s": lock_s, "if_khz": med("frequency"), "mer_db": med("mer") / 10,
            "modcod": int(samples[-1].get("modcod", -1)), "ber_pct": med("ber") / 100,
            "agc1": med("agc1"), "agc2": med("agc2"),
            # a raw 16-bit register that is NOT observed to be monotonic (it can fall within a
            # window), so report first and last value instead of a median or a "count"
            "ldpc_first": ldpc[0], "ldpc_last": ldpc[-1]}, None


# ------------------------------------------------------------------ statistics
def lo_mhz(if_khz):
    return BEACON_RF_MHZ - if_khz / 1000.0


def describe(values):
    """Robust summary of a list of numbers (used for every phase of every script)."""
    n = len(values)
    med = statistics.median(values)
    third = n // 3
    return {"n": n, "median": med, "mean": statistics.mean(values),
            "min": min(values), "max": max(values),
            "stdev": statistics.stdev(values) if n > 1 else float("nan"),   # sample (n-1)
            "mad": statistics.median(abs(v - med) for v in values),
            # early vs late: median of last third minus median of first third
            "drift": (statistics.median(values[-third:]) - statistics.median(values[:third]))
                     if n >= MIN_CYCLES_FOR_DRIFT else None}


def report(measured_if):
    d = describe(measured_if)
    med, mean = d["median"], d["mean"]
    print("\n================ RESULT ================")
    print(f"Valid acquisitions: {d['n']}")
    print("Calculation:  estimated LNB LO = 10,491.500 MHz - measured beacon IF")
    print(f"              estimated LNB LO = 10,491.500 - {med / 1000:.4f} = {lo_mhz(med):.4f} MHz "
          f"(from the MEDIAN measured IF)")
    print(f"Estimated LNB LO           : {lo_mhz(med):.4f} MHz")
    print(f"Deviation from nominal 9750: {(lo_mhz(med) - NOMINAL_LO_MHZ) * 1000:+.1f} kHz")
    print("\nMeasured IF (kHz):")
    print(f"  median {med:.1f}   min {d['min']:.0f}   max {d['max']:.0f}")
    print(f"  standard deviation (sample) {d['stdev']:.2f}   median absolute deviation {d['mad']:.2f}")
    if d["drift"] is not None:
        print(f"  early vs late drift (median of last third - first third): {d['drift']:+.1f} kHz "
              f"(LO change {-d['drift']:+.1f} kHz)")
    else:
        print(f"  early vs late drift: not meaningful with fewer than {MIN_CYCLES_FOR_DRIFT} "
              f"valid acquisitions (1 kHz resolution)")
    print(f"\nSupplementary: mean measured IF {mean:.2f} kHz -> LO {lo_mhz(mean):.4f} MHz")
    print("\nNotes:")
    print("  - LDPC_first->last is the raw demodulator register at the start and end of each window;")
    print("    it does not behave as a clean cumulative counter on this hardware, so it is shown raw.")
    print("  - Resolution: longmynd rounds the carrier to whole kHz, so expect +/-1 kHz.")
    print("  - This is a MiniTiouner-referenced operational estimate of the LNB LO, not an")
    print("    independently verified absolute-frequency measurement (it assumes the beacon is")
    print("    at exactly 10,491.500 MHz).")
    print("  - Measure only after the LNB has warmed up for 15-30 minutes.")


# ------------------------------------------------------------------ arguments
def in_range(lo, hi, cast):
    def check(text):
        value = cast(text)
        if not (lo <= value <= hi):
            raise argparse.ArgumentTypeError(f"must be between {lo} and {hi}")
        return value
    return check


def finite_float(minimum, strict):
    def check(text):
        value = float(text)
        if not math.isfinite(value) or value < minimum or (strict and value == minimum):
            raise argparse.ArgumentTypeError(
                f"must be a finite number {'>' if strict else '>='} {minimum:g}")
        return value
    return check


def add_receiver_arguments(ap, n_dest="n", n_default=10, n_help="number of hop-and-acquire measurements"):
    """The MiniTiouner options, shared with rtl_offset_test.py."""
    ap.add_argument("--" + n_dest.replace("_", "-"), dest=n_dest, type=in_range(1, 10**6, int),
                    default=n_default, help=f"{n_help} (default {n_default})")
    ap.add_argument("--if-khz", type=in_range(FREQ_MIN_KHZ, FREQ_MAX_KHZ, int), default=741474,
                    help="INITIAL ACQUISITION IF in kHz, only used to obtain lock; the LO estimate "
                         "comes from the IF the demodulator measures (default 741474)")
    ap.add_argument("--sr", type=in_range(SR_MIN_KSPS, SR_MAX_KSPS, int), default=1500,
                    help="symbol rate in kS/s (default 1500, the beacon)")
    ap.add_argument("--settle", type=finite_float(0, True), default=3.0,
                    help="seconds to read and discard after lock, before measuring (default 3)")
    ap.add_argument("--window", type=finite_float(0, True), default=2.0,
                    help="seconds of measurement after settling (default 2)")
    ap.add_argument("--hop-khz", type=in_range(0, FREQ_MAX_KHZ - FREQ_MIN_KHZ, int), default=3000,
                    help="tune this far above the beacon first to break the lock (default 3000). "
                         "0 = no hop: then a status packet from before the retune may be taken "
                         "as the new lock")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=in_range(1, 65535, int), default=8765)


def check_receiver_arguments(ap, a):
    if not a.host.strip():
        ap.error("--host must not be empty")
    if a.if_khz + a.hop_khz > FREQ_MAX_KHZ:
        ap.error(f"--if-khz + --hop-khz must not exceed {FREQ_MAX_KHZ} kHz")


def parse_args():
    ap = argparse.ArgumentParser(
        description="Estimate the actual LNB LO: 10,491.500 MHz beacon minus its measured IF.")
    add_receiver_arguments(ap)
    a = ap.parse_args()
    check_receiver_arguments(ap, a)
    return a


# ------------------------------------------------------------------ running
@contextlib.contextmanager
def connections(host, port):
    """Control socket + Monitor; both sockets are always closed."""
    ctl_sock = mon_sock = None
    try:
        ctl_sock, _ = connect(host, port, "control")
        mon_sock, rest = connect(host, port, "monitor")
        yield ctl_sock, Monitor(mon_sock, rest)
    finally:
        for sock in (ctl_sock, mon_sock):
            if sock is not None:
                try:
                    sock.close()
                except OSError:
                    pass


def run_measurements(ctl, mon, a, label=""):
    """a.n hop-and-acquire cycles, one printed row each. Returns the measured IFs (kHz)."""
    print(f"{label}Initial acquisition IF {a.if_khz} kHz (only to obtain lock), SR {a.sr} kS/s, "
          f"{a.n} acquisitions, settle {a.settle:g} s, window {a.window:g} s\n")
    print("time      #  lock_s  measured_IF_kHz  residual_from_tune_kHz  MER_dB  modcod  BER%  "
          "LDPC_first->last  agc1  agc2  estimated_LNB_LO_MHz")
    measured = []
    try:
        for i in range(1, a.n + 1):
            note = ""
            if a.hop_khz and not hop_away(ctl, mon, a):
                note = "  (hop: receiver stayed locked!)"
            r, reason = acquire_and_measure(ctl, mon, a)
            if r is None:
                print(f"{time.strftime('%H:%M:%S')}  {i:>2}  rejected: {reason}{note}")
                continue
            measured.append(r["if_khz"])
            ldpc = f"{r['ldpc_first']:.0f}->{r['ldpc_last']:.0f}"
            print(f"{time.strftime('%H:%M:%S')}  {i:>2}  {r['lock_s']:>5.1f}  {r['if_khz']:>15.0f}  "
                  f"{r['if_khz'] - a.if_khz:>+22.0f}  {r['mer_db']:>6.1f}  {r['modcod']:>6d}  "
                  f"{r['ber_pct']:>4.2f}  {ldpc:>16}  {r['agc1']:>4.0f}  {r['agc2']:>4.0f}  "
                  f"{lo_mhz(r['if_khz']):>19.3f}{note}")
            time.sleep(1)
    except KeyboardInterrupt:
        print("\nInterrupted; using what was measured so far.")
    return measured


def main():
    a = parse_args()
    measured = []
    try:
        with connections(a.host, a.port) as (ctl, mon):
            print(f"Started {time.strftime('%Y-%m-%d %H:%M:%S')}")
            print("Estimating the LNB LO:  estimated LNB LO = 10,491.500 MHz - measured beacon IF")
            measured = run_measurements(ctl, mon, a)
    except (OSError, EOFError, ConnectionError) as error:
        print(f"Cannot talk to longmynd at {a.host}:{a.port}: {error}", file=sys.stderr)
        return 1
    if not measured:
        print("\nNo valid measurement (is the LNB powered and the beacon locking?).")
        return 1
    report(measured)
    return 0


if __name__ == "__main__":
    sys.exit(main())
