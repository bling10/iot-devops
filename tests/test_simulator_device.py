import hashlib
import json
import tempfile
import threading
import unittest
from pathlib import Path

from simulator.device import DeviceConfig, DeviceSimulator
from simulator.ota import DownloadResult
from simulator.state import StateStore


class PublishedMessage:
    def __init__(self):
        self.waited = False

    def wait_for_publish(self, timeout=None):
        self.waited = True

    def is_published(self):
        return self.waited


class FakeMqttClient:
    def __init__(self):
        self.messages = []
        self.disconnected = False

    def publish(self, topic, payload, qos, retain):
        self.messages.append((topic, json.loads(payload), qos, retain))
        return PublishedMessage()

    def disconnect(self):
        self.disconnected = True


class SimulatorDeviceTests(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.client = FakeMqttClient()
        self.config = DeviceConfig(
            device_id="sim-001",
            initial_version="v1.0.42",
            fault_mode="none",
        )
        self.store = StateStore(Path(self.temp_dir.name))

    def tearDown(self):
        self.temp_dir.cleanup()

    @staticmethod
    def command(version="v1.0.43", release_id="release-device-test"):
        firmware = b"firmware"
        return json.dumps(
            {
                "schema_version": 1,
                "message_type": "ota_command",
                "release_id": release_id,
                "version": version,
                "target": "esp32",
                "url": "http://firmware.test/firmware.bin",
                "artifact_size": len(firmware),
                "sha256": hashlib.sha256(firmware).hexdigest(),
            }
        ).encode("utf-8")

    @staticmethod
    def successful_downloader(command, timeout_sec, fault_mode):
        return DownloadResult(command.artifact_size, command.sha256)

    def stages(self):
        return [message[1]["stage"] for message in self.client.messages]

    def test_successful_upgrade_publishes_ordered_states_and_persists_version(self):
        device = DeviceSimulator(
            self.config,
            self.client,
            self.store,
            downloader=self.successful_downloader,
            reboot_delay_sec=0,
        )

        device.process_command(self.command())

        self.assertEqual(
            self.stages(),
            ["received", "downloading", "verified", "rebooting", "healthy"],
        )
        self.assertEqual(device.current_version, "v1.0.43")
        self.assertEqual(self.store.load("sim-001")["current_version"], "v1.0.43")
        self.assertEqual(self.store.load("sim-001")["last_status"]["stage"], "healthy")
        self.assertTrue(all(qos == 1 and not retain for _, _, qos, retain in self.client.messages))

    def test_same_version_is_skipped_without_download(self):
        downloads = []

        def downloader(*args, **kwargs):
            downloads.append(True)
            return self.successful_downloader(*args, **kwargs)

        device = DeviceSimulator(
            self.config,
            self.client,
            self.store,
            downloader=downloader,
            reboot_delay_sec=0,
        )

        device.process_command(self.command(version="v1.0.42"))

        self.assertEqual(self.stages(), ["skipped"])
        self.assertEqual(self.client.messages[0][1]["reason"], "version_unchanged")
        self.assertEqual(downloads, [])

    def test_health_failure_rolls_back_to_previous_version(self):
        config = DeviceConfig(
            device_id="sim-004",
            initial_version="v1.0.42",
            fault_mode="health_check_failed",
        )
        client = FakeMqttClient()
        device = DeviceSimulator(
            config,
            client,
            self.store,
            downloader=self.successful_downloader,
            reboot_delay_sec=0,
        )

        device.process_command(self.command())

        stages = [message[1]["stage"] for message in client.messages]
        self.assertEqual(
            stages,
            ["received", "downloading", "verified", "rebooting", "rollback", "rolled_back"],
        )
        self.assertEqual(client.messages[-2][1]["reported_version"], "v1.0.43")
        self.assertEqual(client.messages[-1][1]["reported_version"], "v1.0.42")
        self.assertEqual(device.current_version, "v1.0.42")
        self.assertEqual(client.messages[-1][1]["reason"], "health_check_failed")

    def test_target_mismatch_is_skipped_like_the_firmware(self):
        payload = json.loads(self.command())
        payload["target"] = "another-platform"
        device = DeviceSimulator(
            self.config,
            self.client,
            self.store,
            downloader=self.successful_downloader,
            reboot_delay_sec=0,
        )

        device.process_command(json.dumps(payload))

        self.assertEqual(self.stages(), ["skipped"])
        self.assertEqual(self.client.messages[0][1]["reason"], "target_mismatch")

    def test_second_command_is_rejected_while_ota_is_running(self):
        download_started = threading.Event()
        allow_download_to_finish = threading.Event()

        def blocking_downloader(command, timeout_sec, fault_mode):
            download_started.set()
            self.assertTrue(allow_download_to_finish.wait(timeout=2))
            return self.successful_downloader(command, timeout_sec, fault_mode)

        device = DeviceSimulator(
            self.config,
            self.client,
            self.store,
            downloader=blocking_downloader,
            reboot_delay_sec=0,
        )
        first_ota = threading.Thread(target=device.process_command, args=(self.command(),))
        first_ota.start()
        self.assertTrue(download_started.wait(timeout=2))

        device.process_command(self.command(release_id="release-device-test-2"))
        allow_download_to_finish.set()
        first_ota.join(timeout=2)

        busy_statuses = [
            message
            for _, message, _, _ in self.client.messages
            if message["release_id"] == "release-device-test-2"
        ]
        self.assertEqual(len(busy_statuses), 1)
        self.assertEqual(busy_statuses[0]["stage"], "failed")
        self.assertEqual(busy_statuses[0]["reason"], "ota_busy")


if __name__ == "__main__":
    unittest.main()
