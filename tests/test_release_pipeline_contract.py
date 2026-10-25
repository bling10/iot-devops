import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
VERIFY_SCRIPT = (REPO_ROOT / "ci" / "verify-firmware.sh").read_text(encoding="utf-8")
RELEASE_SCRIPT = (REPO_ROOT / "ci" / "release-firmware.sh").read_text(encoding="utf-8")
JENKINSFILE = (REPO_ROOT / "Jenkinsfile").read_text(encoding="utf-8")


class ReleasePipelineContractTests(unittest.TestCase):
    def test_verifier_generates_complete_v1_command(self):
        for fragment in (
            '"schema_version": 1',
            '"message_type": "ota_command"',
            '"target": "esp32"',
            '"release_id": "${release_id}"',
            '"artifact_size": ${firmware_size}',
            '"sha256": "${firmware_sha256}"',
        ):
            with self.subTest(fragment=fragment):
                self.assertIn(fragment, VERIFY_SCRIPT)

    def test_release_script_uses_validated_device_topics(self):
        for fragment in (
            "OTA_TARGET_DEVICE_IDS",
            "^[a-z0-9][a-z0-9-]{0,63}$",
            'iot/v1/devices/${device_id}/ota/command',
            "RELEASE_DRY_RUN",
            "-q 1",
        ):
            with self.subTest(fragment=fragment):
                self.assertIn(fragment, RELEASE_SCRIPT)

        self.assertNotIn("MQTT_OTA_TOPIC", RELEASE_SCRIPT)
        self.assertNotIn("device/esp32/ota", RELEASE_SCRIPT)
        self.assertNotIn("\n        -r", RELEASE_SCRIPT)

    def test_jenkins_passes_targets_and_dry_run_mode(self):
        for fragment in (
            "OTA_TARGET_DEVICE_IDS",
            "OTA_RELEASE_DRY_RUN",
            "OTA_TARGET_DISPLAY",
            "OTA_TARGET_COUNT",
            "RELEASE_DRY_RUN=1",
            './ci/release-firmware.sh "$DIST_DIR" "$VERSION"',
        ):
            with self.subTest(fragment=fragment):
                self.assertIn(fragment, JENKINSFILE)


if __name__ == "__main__":
    unittest.main()
