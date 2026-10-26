import json
import re
from dataclasses import dataclass
from typing import Dict, Optional, Union
from urllib.parse import urlparse


MAX_COMMAND_BYTES = 2048
DEFAULT_OTA_PARTITION_SIZE = 0x1C0000
DEVICE_ID_PATTERN = re.compile(r"^[a-z0-9][a-z0-9-]{0,63}$")
VERSION_PATTERN = re.compile(r"^v[0-9]+\.[0-9]+\.[0-9]+$")
SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")

STAGE_RESULTS = {
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


@dataclass(frozen=True)
class OtaCommand:
    release_id: str
    version: str
    url: str
    artifact_size: int
    sha256: str


class CommandValidationError(ValueError):
    def __init__(
        self,
        reason: str,
        release_id: Optional[str] = None,
        target_version: Optional[str] = None,
    ):
        super().__init__(reason)
        self.reason = reason
        self.release_id = release_id
        self.target_version = target_version


def _safe_string(value, max_length):
    if isinstance(value, str) and 1 <= len(value) <= max_length:
        return value
    return None


def parse_ota_command(
    payload: Union[bytes, str],
    max_artifact_size: int = DEFAULT_OTA_PARTITION_SIZE,
) -> OtaCommand:
    if isinstance(payload, str):
        raw_payload = payload.encode("utf-8")
    elif isinstance(payload, bytes):
        raw_payload = payload
    else:
        raise CommandValidationError("invalid_payload")

    if not raw_payload or len(raw_payload) > MAX_COMMAND_BYTES:
        raise CommandValidationError("invalid_payload")

    try:
        document = json.loads(raw_payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        raise CommandValidationError("invalid_payload")

    if not isinstance(document, dict):
        raise CommandValidationError("invalid_payload")

    release_id = _safe_string(document.get("release_id"), 95)
    target_version = _safe_string(document.get("version"), 31)

    schema_version = document.get("schema_version")
    if not isinstance(schema_version, int) or isinstance(schema_version, bool):
        raise CommandValidationError("invalid_payload", release_id, target_version)
    if schema_version != 1:
        raise CommandValidationError(
            "unsupported_schema_version", release_id, target_version
        )
    message_type = document.get("message_type")
    if not isinstance(message_type, str):
        raise CommandValidationError("invalid_payload", release_id, target_version)
    if message_type != "ota_command":
        raise CommandValidationError("invalid_message_type", release_id, target_version)
    target = document.get("target")
    if not isinstance(target, str):
        raise CommandValidationError("invalid_payload", release_id, target_version)
    if target != "esp32":
        raise CommandValidationError("target_mismatch", release_id, target_version)

    url = document.get("url")
    artifact_size = document.get("artifact_size")
    sha256 = document.get("sha256")
    parsed_url = urlparse(url) if isinstance(url, str) else None
    fields_valid = all(
        (
            release_id is not None,
            target_version is not None and VERSION_PATTERN.fullmatch(target_version),
            isinstance(url, str)
            and 1 <= len(url) <= 255
            and parsed_url is not None
            and parsed_url.scheme in {"http", "https"}
            and bool(parsed_url.netloc),
            isinstance(artifact_size, int)
            and not isinstance(artifact_size, bool)
            and 0 < artifact_size <= max_artifact_size,
            isinstance(sha256, str) and SHA256_PATTERN.fullmatch(sha256),
        )
    )
    if not fields_valid:
        raise CommandValidationError("invalid_payload", release_id, target_version)

    return OtaCommand(
        release_id=release_id,
        version=target_version,
        url=url,
        artifact_size=artifact_size,
        sha256=sha256,
    )


def build_status(
    *,
    device_id: str,
    release_id: str,
    stage: str,
    reported_version: str,
    target_version: str,
    uptime_sec: int,
    artifact_size: Optional[int] = None,
    reason: Optional[str] = None,
) -> Dict[str, object]:
    if not DEVICE_ID_PATTERN.fullmatch(device_id):
        raise ValueError("invalid device_id")
    if stage not in STAGE_RESULTS:
        raise ValueError("invalid OTA stage")
    if stage in {"failed", "rolled_back", "skipped"} and not reason:
        raise ValueError("terminal failure or skip status requires a reason")

    status = {
        "schema_version": 1,
        "message_type": "ota_status",
        "release_id": release_id,
        "device_id": device_id,
        "stage": stage,
        "result": STAGE_RESULTS[stage],
        "reported_version": reported_version,
        "target_version": target_version,
        "uptime_sec": max(0, int(uptime_sec)),
    }
    if artifact_size is not None:
        status["artifact_size"] = artifact_size
    if reason:
        status["reason"] = reason
    return status


def build_telemetry(
    *,
    device_id: str,
    version: str,
    temperature: float,
    humidity: float,
    rssi: int,
    free_heap: int,
    uptime_sec: int,
) -> Dict[str, object]:
    return {
        "schema_version": 1,
        "message_type": "telemetry",
        "device_id": device_id,
        "temperature": round(temperature, 1),
        "humidity": round(humidity, 1),
        "rssi": int(rssi),
        "free_heap": int(free_heap),
        "uptime_sec": max(0, int(uptime_sec)),
        "version": version,
    }
