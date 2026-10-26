import hashlib
import json
import tempfile
import threading
import unittest
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from simulator.ota import OtaDownloadError, download_and_verify
from simulator.protocol import parse_ota_command


class QuietHandler(SimpleHTTPRequestHandler):
    def log_message(self, format, *args):
        return


class SimulatorOtaTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp_dir = tempfile.TemporaryDirectory()
        cls.firmware = b"esp32-test-firmware" * 256
        Path(cls.temp_dir.name, "firmware.bin").write_bytes(cls.firmware)
        handler = partial(QuietHandler, directory=cls.temp_dir.name)
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
        cls.server_thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.server_thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.server_thread.join(timeout=2)
        cls.temp_dir.cleanup()

    def make_command(self, **overrides):
        payload = {
            "schema_version": 1,
            "message_type": "ota_command",
            "release_id": "release-http-test",
            "version": "v1.0.43",
            "target": "esp32",
            "url": f"http://127.0.0.1:{self.server.server_port}/firmware.bin",
            "artifact_size": len(self.firmware),
            "sha256": hashlib.sha256(self.firmware).hexdigest(),
        }
        payload.update(overrides)
        return parse_ota_command(json.dumps(payload))

    def test_download_streams_and_verifies_real_http_response(self):
        result = download_and_verify(self.make_command(), timeout_sec=2)

        self.assertEqual(result.artifact_size, len(self.firmware))
        self.assertEqual(result.sha256, hashlib.sha256(self.firmware).hexdigest())

    def test_sha256_mismatch_is_rejected(self):
        command = self.make_command(sha256="0" * 64)

        with self.assertRaises(OtaDownloadError) as context:
            download_and_verify(command, timeout_sec=2)

        self.assertEqual(context.exception.reason, "sha256_mismatch")

    def test_declared_size_mismatch_is_rejected(self):
        command = self.make_command(artifact_size=len(self.firmware) + 1)

        with self.assertRaises(OtaDownloadError) as context:
            download_and_verify(command, timeout_sec=2)

        self.assertEqual(context.exception.reason, "content_length_mismatch")


if __name__ == "__main__":
    unittest.main()
