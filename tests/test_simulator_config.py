import unittest
from pathlib import Path

from simulator.config import load_runtime_config


REPO_ROOT = Path(__file__).resolve().parents[1]


class SimulatorConfigTests(unittest.TestCase):
    def test_default_config_defines_five_unique_devices(self):
        config = load_runtime_config(
            REPO_ROOT / "simulator" / "config.json",
            environment={},
        )

        device_ids = [device.device_id for device in config.devices]
        self.assertEqual(device_ids, [f"sim-{number:03d}" for number in range(1, 6)])
        self.assertEqual(len(device_ids), len(set(device_ids)))
        self.assertEqual(config.mqtt.host, "iot-mqtt")

    def test_environment_overrides_broker_and_state_directory(self):
        config = load_runtime_config(
            REPO_ROOT / "simulator" / "config.json",
            environment={
                "MQTT_HOST": "192.0.2.10",
                "MQTT_PORT": "2883",
                "SIMULATOR_STATE_DIR": "/tmp/simulator-state",
            },
        )

        self.assertEqual(config.mqtt.host, "192.0.2.10")
        self.assertEqual(config.mqtt.port, 2883)
        self.assertEqual(config.state_dir.as_posix(), "/tmp/simulator-state")

    def test_container_and_dependency_versions_are_fixed(self):
        dockerfile = (REPO_ROOT / "simulator" / "Dockerfile").read_text(encoding="utf-8")
        requirements = (REPO_ROOT / "simulator" / "requirements.txt").read_text(
            encoding="utf-8"
        )
        start_script = (REPO_ROOT / "scripts" / "start.sh").read_text(encoding="utf-8")

        self.assertIn("FROM python:3.13.5-alpine3.22", dockerfile)
        self.assertIn("paho-mqtt==2.1.0", requirements)
        self.assertIn("SIMULATOR_UID=10001", start_script)
        self.assertIn('install -d -m 0755 -o "${SIMULATOR_UID}"', start_script)

    def test_telegraf_consumes_v1_device_topics(self):
        telegraf_config = (
            REPO_ROOT / "deploy" / "iot" / "telegraf" / "telegraf.conf"
        ).read_text(encoding="utf-8")

        for fragment in (
            '"iot/v1/devices/+/telemetry"',
            '"iot/v1/devices/+/ota/status"',
            'name_override = "sensor_data"',
            'name_override = "ota_status"',
            'tag_keys = ["device_id", "version"]',
            'tag_keys = ["device_id", "stage", "result"]',
        ):
            with self.subTest(fragment=fragment):
                self.assertIn(fragment, telegraf_config)


if __name__ == "__main__":
    unittest.main()
