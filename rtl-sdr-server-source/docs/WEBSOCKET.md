# Spectrum WebSocket

Run main.py. The headless server binds to 0.0.0.0:7681 until stopped.
Connect locally to ws://localhost:7681/ or use the computer's LAN IP.
Subprotocols: fft, fft_fast, fft_m0dtslivetune, fft_f5oeoplutofw,
fft_ea7kirsatcontroller. Connections without a subprotocol also work.

Each complete sweep produces one binary message: 922 little-endian uint16
values (1844 bytes), with no header or metadata. New clients get the latest
completed sweep. Slow clients skip old sweeps. All protocols receive the
same sweep cadence; fft_fast does not produce new measurements faster.
Frequency at index i: 740498046.875 + i * 9765.625 Hz (LNB output).

The encoder follows the reference's 9000 scale, +150 dB offset, floor search
indices 51..819, floor smoothing coefficient 0.995, floor target 141000,
viewport offset 114000, division by 3 and saturation to uint16.
Steady-state floor is about 9000 counts; a 1 dB rise is 3000 counts.
We initialize the floor from the first measurement to avoid startup saturation.
Floor smoothing advances once per complete sweep, not at Airspy timer rates.
RTL-SDR power averaging remains in use, rather than Airspy's per-FFT dB smoothing.
The result matches the wire format and display scaling, not receiver calibration
or the original time response. Automatic plot y-axis scaling does not affect WS data.

app/spectrum_calibration.json contains 922 additive dB corrections, initially zero,
applied only to WebSocket levels before floor adjustment. Restart after editing.
These are not the original Airspy correction table's integer units.

Dependency: websockets>=15,<16 (installed in the project environment).
Tests: .venv/bin/python -m unittest discover -s tests

## Operation

Run `.venv/bin/python main.py` from the project directory, or run main.py in PyCharm.
Open WEBpage/index.html for the existing browser display. Stop with Ctrl+C.
Edit the capture constants at the top of main.py. No plot window opens.

The old plotting program is preserved in archive/plot_monitor.py. Do not run it
alongside main.py: they require the same stick and port.

Frequency correction: `.venv/bin/python main.py --correction-khz 36`
Positive 36 tunes each portion 36 kHz lower, shifting received signals 36 kHz
higher on the unchanged 922-point frequency grid. Default is zero. The value
is a run-time argument, not a persistent calibration change. Rebuild executable
after source changes before using new arguments with the binary.
