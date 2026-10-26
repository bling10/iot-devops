import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
VERIFY_SCRIPT = (REPO_ROOT / "ci" / "verify-firmware.sh").read_text(encoding="utf-8")
RELEASE_SCRIPT = (REPO_ROOT / "ci" / "release-firmware.sh").read_text(encoding="utf-8")
JENKINSFILE = (REPO_ROOT / "Jenkinsfile").read_text(encoding="utf-8")
JENKINS_DOCKERFILE = (REPO_ROOT / "deploy" / "jenkins" / "Dockerfile").read_text(
    encoding="utf-8"
)
JENKINS_COMPOSE = (REPO_ROOT / "deploy" / "jenkins" / "compose.yml").read_text(
    encoding="utf-8"
)
IOT_COMPOSE = (REPO_ROOT / "deploy" / "iot" / "compose.yml").read_text(
    encoding="utf-8"
)
START_SCRIPT = (REPO_ROOT / "scripts" / "start.sh").read_text(encoding="utf-8")


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

    def test_jenkins_runtime_persistently_provides_python3(self):
        for fragment in (
            "FROM jenkins/jenkins:2.567-jdk21",
            "apt-get install -y --no-install-recommends python3",
            "USER jenkins",
        ):
            with self.subTest(fragment=fragment):
                self.assertIn(fragment, JENKINS_DOCKERFILE)

        for fragment in (
            "build:",
            "dockerfile: Dockerfile",
            "image: iot-jenkins:2.567-jdk21-python3",
        ):
            with self.subTest(fragment=fragment):
                self.assertIn(fragment, JENKINS_COMPOSE)

    def test_release_restart_uses_stashed_original_metadata(self):
        for fragment in (
            "release-metadata.properties",
            "loadReleaseMetadata",
            "OTA_TARGET_DEVICE_IDS_RESOLVED",
            "OTA_RELEASE_DRY_RUN_RESOLVED",
            "env.VERSION = releaseMetadata['VERSION']",
            "env.DIST_DIR = releaseMetadata['DIST_DIR']",
            "env.FIRMWARE_SHA256 = releaseMetadata['FIRMWARE_SHA256']",
            '"OTA_TARGET_DEVICE_IDS=${env.OTA_TARGET_DEVICE_IDS_RESOLVED}"',
            '"RELEASE_DRY_RUN=${env.OTA_RELEASE_DRY_RUN_RESOLVED}"',
        ):
            with self.subTest(fragment=fragment):
                self.assertIn(fragment, JENKINSFILE)

        self.assertIn(
            "release-metadata.properties,ci/release-firmware.sh",
            JENKINSFILE,
        )

    def test_jenkins_receives_the_host_docker_socket_group(self):
        self.assertIn("group_add:", JENKINS_COMPOSE)
        self.assertIn('${DOCKER_GID:-991}', JENKINS_COMPOSE)
        self.assertIn("DOCKER_SOCKET=/var/run/docker.sock", START_SCRIPT)
        self.assertIn("DOCKER_GID=\"$(stat -c '%g'", START_SCRIPT)
        self.assertIn("export DOCKER_GID", START_SCRIPT)

    def test_telegraf_receives_the_host_docker_socket_group(self):
        telegraf_service = IOT_COMPOSE.split("  telegraf:\n", 1)[1].split(
            "\n  grafana:\n", 1
        )[0]
        self.assertIn("group_add:", telegraf_service)
        self.assertIn('${DOCKER_GID:-991}', telegraf_service)


if __name__ == "__main__":
    unittest.main()
