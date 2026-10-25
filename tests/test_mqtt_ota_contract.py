import json
import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
EXAMPLES_DIR = REPO_ROOT / "docs" / "protocol" / "examples"
FIRMWARE_SOURCE = REPO_ROOT / "firmware" / "esp32-node" / "main" / "main.c"

DEVICE_ID_PATTERN = re.compile(r"^[a-z0-9][a-z0-9-]{0,63}$")
VERSION_PATTERN = re.compile(r"^v[0-9]+\.[0-9]+\.[0-9]+$")
SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")


def load_example(name):
    with (EXAMPLES_DIR / name).open(encoding="utf-8") as file:
        return json.load(file)


class MqttOtaContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = FIRMWARE_SOURCE.read_text(encoding="utf-8")

    def test_ota_command_example_matches_v1_contract(self):
        command = load_example("ota-command.json")

        self.assertEqual(command["schema_version"], 1)
        self.assertEqual(command["message_type"], "ota_command")
        self.assertEqual(command["target"], "esp32")
        self.assertRegex(command["version"], VERSION_PATTERN)
        self.assertGreater(command["artifact_size"], 0)
        self.assertRegex(command["sha256"], SHA256_PATTERN)
        self.assertRegex(command["url"], r"^https?://")

    def test_status_examples_use_valid_stage_result_pairs(self):
        expected_results = {
            "received": "in_progress",
            "downloading": "in_progress",
            "verified": "in_progress",
            "rebooting": "in_progress",
            "rollback": "in_progress",
            "healthy": "success",
            "failed": "failed",
            "rolled_back": "failed",
            "skipped": "skipped",
        }

        for name in ("ota-status-healthy.json", "ota-status-failed.json"):
            with self.subTest(example=name):
                status = load_example(name)
                self.assertEqual(status["schema_version"], 1)
                self.assertEqual(status["message_type"], "ota_status")
                self.assertRegex(status["device_id"], DEVICE_ID_PATTERN)
                self.assertEqual(status["result"], expected_results[status["stage"]])

    def test_telemetry_example_identifies_its_device(self):
        telemetry = load_example("telemetry.json")

        self.assertEqual(telemetry["schema_version"], 1)
        self.assertEqual(telemetry["message_type"], "telemetry")
        self.assertRegex(telemetry["device_id"], DEVICE_ID_PATTERN)
        self.assertRegex(telemetry["version"], VERSION_PATTERN)

    def test_firmware_uses_mac_identity_and_device_topics(self):
        required_fragments = (
            "esp_read_mac(mac, ESP_MAC_WIFI_STA)",
            '"esp32-%02x%02x%02x%02x%02x%02x"',
            '"iot/v1/devices/%s/telemetry"',
            '"iot/v1/devices/%s/ota/command"',
            '"iot/v1/devices/%s/ota/status"',
            ".credentials.client_id = device_id",
        )
        for fragment in required_fragments:
            with self.subTest(fragment=fragment):
                self.assertIn(fragment, self.source)

        self.assertNotIn('#define DEVICE_ID          "esp32"', self.source)

    def test_firmware_validates_v1_and_keeps_explicit_legacy_path(self):
        required_fragments = (
            'cJSON_GetObjectItemCaseSensitive(root, "schema_version")',
            'cJSON_GetObjectItemCaseSensitive(root, "message_type")',
            'cJSON_GetObjectItemCaseSensitive(root, "target")',
            'return "unsupported_schema_version"',
            'return "invalid_message_type"',
            'return "target_mismatch"',
            '"device/esp32/ota"',
            "handle_ota_message(event->data, event->data_len, is_legacy_topic)",
        )
        for fragment in required_fragments:
            with self.subTest(fragment=fragment):
                self.assertIn(fragment, self.source)

    def test_firmware_emits_v1_status_and_telemetry(self):
        required_fragments = (
            'cJSON_AddNumberToObject(root, "schema_version", OTA_SCHEMA_VERSION)',
            'cJSON_AddStringToObject(root, "message_type", "ota_status")',
            'cJSON_AddStringToObject(root, "device_id", device_id)',
            '\\"message_type\\":\\"telemetry\\"',
            '\\"device_id\\":\\"%s\\"',
            'const char *failure_reason = "internal_error"',
        )
        for fragment in required_fragments:
            with self.subTest(fragment=fragment):
                self.assertIn(fragment, self.source)


if __name__ == "__main__":
    unittest.main()
