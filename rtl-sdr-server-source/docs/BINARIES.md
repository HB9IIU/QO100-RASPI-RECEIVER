# Linux executables

Ubuntu x86-64 executable: dist/rtl-sdr-server-ubuntu-x86_64
Built for the current Ubuntu 26.04 machine. It is not an ARM executable and
isn't guaranteed to run on older Linux systems.

Run it instead of main.py. Stop main.py first to free the stick and port 7681.
The WEBpage display connects as before; the executable serves WebSocket data,
not the HTML webpage.

Default calibration is bundled. Optionally place spectrum_calibration.json next
to the executable to override it. USB permissions and kernel driver configuration
are still operating-system settings.

Run the executable with --self-test to check library loading, calibration and a
real local WebSocket connection, without opening the radio.

## Rebuild

Use tools/build_binary.sh with a Python interpreter path and optional output name.
The interpreter needs numpy, pyrtlsdr, pyrtlsdrlib, websockets>=15,<16 and pyinstaller.
Build on the target architecture and an OS compatible with the destination.
For Raspberry Pi, the model, OS release and 32/64-bit architecture must be known
before selecting a build environment. A native Pi build is preferred.

## Raspberry Pi (64-bit Debian 12)

Native ARM64 executable: `dist/rtl-sdr-server-raspberrypi-aarch64`.
Built on this Pi with Python 3.11.2. Run from the project folder:

```sh
./dist/rtl-sdr-server-raspberrypi-aarch64
```

Open `WEBpage/index.html` in your browser and connect to
`ws://localhost:7681/`. Stop the server with Ctrl+C.
The executable includes Python and its dependencies; no venv activation is needed.

If the receiver reports `LIBUSB_ERROR_BUSY` and the kernel TV driver has claimed
the stick, release it for this session (until reboot or driver reload):

```sh
sudo modprobe -r rtl2832_sdr dvb_usb_rtl28xxu
```

Rebuild on this Pi using the separate build environment:

```sh
python3 -m venv .venv-build
.venv-build/bin/python -m pip install -r tools/requirements-build-pi.txt
bash tools/build_binary.sh .venv-build/bin/python rtl-sdr-server-raspberrypi-aarch64
```

The build script finishes with a self-test that needs local socket access.
The existing `venv` and Ubuntu executable are retained.
