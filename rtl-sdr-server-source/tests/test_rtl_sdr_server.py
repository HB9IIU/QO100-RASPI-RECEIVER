import threading
import unittest
from unittest.mock import patch
import numpy as np
import main as app
from app.spectrum_ws import SpectrumEncoder


class HeadlessTests(unittest.TestCase):
    def test_complete_sweep(self):
        stop = threading.Event()
        class Sdr:
            closed = False
            def set_agc_mode(self, enabled):
                assert not enabled
            def read_samples(self, count):
                assert count % 256 == 0
                return np.exp(2j * np.pi * .1 * np.arange(count)) * .05
            def close(self):
                self.closed = True
        class Server:
            closed = False
            trace = None
            def start(self): pass
            def publish(self, trace):
                self.trace = trace
                assert len(SpectrumEncoder().encode(trace)) == 1844
                stop.set()
            def close(self):
                self.closed = True
        sdr, server = Sdr(), Server()
        app.run(stop, lambda: sdr, lambda: server)
        self.assertEqual(server.trace.shape, (922,))
        self.assertTrue(np.isfinite(server.trace).all())
        self.assertTrue(sdr.closed and server.closed)

    def test_open_failure_cleans_server(self):
        class Server:
            closed = False
            def start(self): pass
            def close(self): self.closed = True
        server = Server()
        def fail(): raise OSError('No device')
        with self.assertRaises(OSError):
            app.run(threading.Event(), fail, lambda: server)
        self.assertTrue(server.closed)


if __name__ == '__main__':
    unittest.main()
