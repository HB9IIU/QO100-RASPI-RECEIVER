"""Airspy-compatible binary spectrum transport; no frequency metadata on wire."""
import asyncio
import json
from pathlib import Path
import socket
import threading
import sys

import numpy as np
from websockets.asyncio.server import serve
from websockets.exceptions import ConnectionClosed

PROTOCOLS = ['fft', 'fft_m0dtslivetune', 'fft_f5oeoplutofw',
             'fft_ea7kirsatcontroller', 'fft_fast']


class SpectrumEncoder:
    def __init__(self, calibration_path=None):
        path = calibration_path or Path(__file__).with_name('spectrum_calibration.json')
        if calibration_path is None and getattr(sys, 'frozen', False):
            external = Path(sys.executable).with_name('spectrum_calibration.json')
            if external.is_file():
                path = external
        self.correction = np.asarray(json.loads(Path(path).read_text())['correction_db'], dtype=float)
        if self.correction.shape != (922,) or not np.isfinite(self.correction).all():
            raise ValueError('Calibration must contain 922 finite correction_db values')
        self.floor = None

    def encode(self, trace):
        trace = np.asarray(trace, dtype=float)
        if trace.shape != (922,) or not np.isfinite(trace).all():
            raise ValueError('Only complete, finite 922-point sweeps may be published')
        # Reference: FFT_SCALE=9000, FFT_OFFSET=150, FFT_PRESCALE=3.
        values = np.trunc(np.maximum(9000 * (trace + self.correction + 150), 0)).astype(np.int64)
        # C loop: j=51; j < 922 - 102.4; ++j -> indices 51..819.
        lowest = int(values[51:820].min())
        # Initialize from first sweep to avoid the reference's long startup clipping.
        if self.floor is None:
            self.floor = lowest
        else:
            self.floor = int(lowest * 0.005 + self.floor * 0.995)
        values = np.maximum(values + (141000 - self.floor) - 114000, 0) // 3
        return np.clip(values, 0, 65535).astype('<u2').tobytes()


class SpectrumServer:
    """Dedicated event loop; slow clients skip old sweeps, never block capture."""
    def __init__(self, host='0.0.0.0', port=7681):
        if port == 0:
            # websockets 15 may expose an empty Server.sockets tuple when it
            # is itself asked to select an ephemeral port. Select one first
            # so startup follows the same fixed-port path as production.
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
                probe.bind((host, 0))
                port = probe.getsockname()[1]
        self.host, self.port = host, port
        self.encoder = SpectrumEncoder()
        self.ready = threading.Event()
        self.error = None
        self.clients = set()
        self.latest = None
        self.thread = threading.Thread(target=self._run, name='spectrum-websocket', daemon=True)

    def start(self):
        self.thread.start()
        self.ready.wait()
        if self.error:
            raise RuntimeError(f'Cannot start spectrum WebSocket server: {self.error}') from self.error

    def _run(self):
        try:
            asyncio.run(self._serve())
        except Exception as error:
            self.error = error
        finally:
            self.ready.set()

    async def _serve(self):
        self.loop = asyncio.get_running_loop()
        self.stop_event = asyncio.Event()
        def select_protocol(connection, offered):
            if not offered:
                return None
            for protocol in PROTOCOLS:
                if protocol in offered:
                    return protocol
            from websockets.exceptions import NegotiationError
            raise NegotiationError('Unsupported spectrum subprotocol')
        async with serve(self._client, self.host, self.port, subprotocols=PROTOCOLS,
                         select_subprotocol=select_protocol, compression=None,
                         close_timeout=1, max_size=4096) as server:
            self.port = server.sockets[0].getsockname()[1]
            self.ready.set()
            await self.stop_event.wait()

    async def _client(self, socket):
        queue = asyncio.Queue(maxsize=1)
        self.clients.add(queue)
        if self.latest is not None:
            queue.put_nowait(self.latest)
        async def send_updates():
            while True:
                await socket.send(await queue.get())
        sender = asyncio.create_task(send_updates())
        closed = asyncio.create_task(socket.wait_closed())
        try:
            await asyncio.wait([sender, closed], return_when=asyncio.FIRST_COMPLETED)
        finally:
            self.clients.discard(queue)
            sender.cancel()
            closed.cancel()
            await asyncio.gather(sender, closed, return_exceptions=True)

    def publish(self, trace):
        payload = self.encoder.encode(trace)
        if self.error or not self.thread.is_alive():
            raise RuntimeError('Spectrum WebSocket server stopped') from self.error
        self.loop.call_soon_threadsafe(self._deliver, payload)

    def _deliver(self, payload):
        self.latest = payload
        for queue in self.clients:
            if queue.full():
                queue.get_nowait()
            queue.put_nowait(payload)

    def close(self):
        if self.thread.is_alive():
            self.loop.call_soon_threadsafe(self.stop_event.set)
            self.thread.join(timeout=5)
