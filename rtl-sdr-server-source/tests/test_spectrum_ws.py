import unittest
import numpy as np
from websockets.sync.client import connect
from app.spectrum_ws import SpectrumEncoder, SpectrumServer, PROTOCOLS


class SpectrumTests(unittest.TestCase):
    def test_encoding(self):
        encoder = SpectrumEncoder()
        trace = np.full(922, -60.0)
        trace[300:400] = -50.0
        trace[0] = -120
        trace[-1] = 0
        data = encoder.encode(trace)
        self.assertEqual(len(data), 1844)
        values = np.frombuffer(data, dtype='<u2')
        self.assertEqual(values[100], 9000)
        self.assertEqual(values[350], 39000)
        self.assertEqual(values[0], 0)
        self.assertEqual(values[-1], 65535)
        self.assertEqual(data[200:202], b'\x28\x23')
        encoder.correction[100] = 1
        corrected = np.frombuffer(encoder.encode(trace), dtype='<u2')
        self.assertEqual(corrected[100], 12000)
        old_floor = encoder.floor
        encoder.encode(trace + 1)
        self.assertEqual(encoder.floor, int((old_floor + 9000)*.005 + old_floor*.995))
        with self.assertRaises(ValueError):
            encoder.encode(np.full(922, np.nan))

    def test_clients_and_shutdown(self):
        server = SpectrumServer('127.0.0.1', 0)
        server.start()
        clients = []
        try:
            for protocol in [None] + PROTOCOLS:
                client = connect(f'ws://127.0.0.1:{server.port}/',
                                 subprotocols=[protocol] if protocol else None, proxy=None)
                self.assertEqual(client.subprotocol, protocol)
                clients.append(client)
            server.publish(np.full(922, -60.0))
            for client in clients:
                payload = client.recv(timeout=3)
                self.assertIsInstance(payload, bytes)
                self.assertEqual(payload, np.full(922, 9000, dtype='<u2').tobytes())
            server.publish(np.full(922, -59.0))
            for client in clients:
                self.assertEqual(len(client.recv(timeout=3)), 1844)
            with connect(f'ws://127.0.0.1:{server.port}/', proxy=None) as late:
                self.assertEqual(len(late.recv(timeout=3)), 1844)
        finally:
            server.close()
            for client in clients:
                client.close()
        self.assertFalse(server.thread.is_alive())


if __name__ == '__main__':
    unittest.main()
