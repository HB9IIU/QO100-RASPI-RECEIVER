# RTL-SDR QO-100 spectrum server

1. Run **main.py** in PyCharm.
2. Open **WEBpage/index.html** for the spectrum display.
3. Stop the server with **Ctrl+C** in the terminal.

Capture settings are near the top of main.py. The WebSocket address is
ws://localhost:7681/ on this computer.

- app/: supporting code and calibration; nothing to run here.
- WEBpage/: browser display.
- tests/: automated developer checks; not needed to use the receiver.
- docs/: technical documentation.
- archive/: preserved old plotting version.
- eshail-*/: original reference projects, left in place.

Developer checks: `.venv/bin/python -m unittest discover -s tests`
