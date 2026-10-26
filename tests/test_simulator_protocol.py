import json
import unittest

from simulator.protocol import (
    CommandValidationError,
    build_status,
    parse_ota_command,
)


VALID_COMMAND = {
    "schema_version": 1,
    "message_type": "ota_command",
    "release_id": "jenkins-43-abcdef123456",
    "version": "v1.0.43",
    "target": "esp32",
    "url": "http://127.0.0.1:8081/firmware_v1.0.43.bin",
    "artifact_size": 16,
    "sha256": "a" * 64,
}


class SimulatorProtocolTests(unittest.TestCase):
    def test_valid_command_is_parsed(self):
        command = parse_ota_command(json.dumps(VALID_COMMAND).encode("utf-8"))

        self.assertEqual(command.release_id, VALID_COMMAND["release_id"])
        self.assertEqual(command.version, "v1.0.43")
        self.assertEqual(command.artifact_size, 16)

    def test_unsupported_schema_keeps_safe_release_id(self):
        payload = dict(VALID_COMMAND, schema_version=2)

        with self.assertRaises(CommandValidationError) as context:
            parse_ota_command(json.dumps(payload))

        self.assertEqual(context.exception.reason, "unsupported_schema_version")
        self.assertEqual(context.exception.release_id, payload["release_id"])

    def test_missing_release_id_cannot_be_correlated(self):
        payload = dict(VALID_COMMAND)
        del payload["release_id"]

        with self.assertRaises(CommandValidationError) as context:
            parse_ota_command(json.dumps(payload))

        self.assertEqual(context.exception.reason, "invalid_payload")
        self.assertIsNone(context.exception.release_id)

    def test_missing_schema_is_invalid_payload(self):
        payload = dict(VALID_COMMAND)
        del payload["schema_version"]

        with self.assertRaises(CommandValidationError) as context:
            parse_ota_command(json.dumps(payload))

        self.assertEqual(context.exception.reason, "invalid_payload")

    def test_missing_envelope_string_is_invalid_payload(self):
        for field in ("message_type", "target"):
            with self.subTest(field=field):
                payload = dict(VALID_COMMAND)
                del payload[field]

                with self.assertRaises(CommandValidationError) as context:
                    parse_ota_command(json.dumps(payload))

                self.assertEqual(context.exception.reason, "invalid_payload")

    def test_boolean_artifact_size_is_rejected(self):
        payload = dict(VALID_COMMAND, artifact_size=True)

        with self.assertRaises(CommandValidationError) as context:
            parse_ota_command(json.dumps(payload))

        self.assertEqual(context.exception.reason, "invalid_payload")

    def test_status_builder_uses_frozen_stage_result_mapping(self):
        status = build_status(
            device_id="sim-001",
            release_id="release-1",
            stage="healthy",
            reported_version="v1.0.43",
            target_version="v1.0.43",
            uptime_sec=2,
            artifact_size=16,
        )

        self.assertEqual(status["result"], "success")
        self.assertNotIn("reason", status)

    def test_status_builder_requires_reason_for_failure(self):
        with self.assertRaises(ValueError):
            build_status(
                device_id="sim-001",
                release_id="release-1",
                stage="failed",
                reported_version="v1.0.42",
                target_version="v1.0.43",
                uptime_sec=2,
            )


if __name__ == "__main__":
    unittest.main()
