import json
import os
from dataclasses import dataclass
from pathlib import Path
from typing import List, Mapping, Optional

from .device import DeviceConfig


@dataclass(frozen=True)
class MqttConfig:
    host: str
    port: int
    keepalive_sec: int
    username: Optional[str] = None
    password: Optional[str] = None


@dataclass(frozen=True)
class RuntimeConfig:
    mqtt: MqttConfig
    devices: List[DeviceConfig]
    state_dir: Path
    telemetry_interval_sec: float
    download_timeout_sec: float
    reboot_delay_sec: float


def _positive_number(value, name, number_type):
    try:
        parsed = number_type(value)
    except (TypeError, ValueError):
        raise ValueError(f"{name} must be a positive number")
    if parsed <= 0:
        raise ValueError(f"{name} must be a positive number")
    return parsed


def load_runtime_config(
    path: Path,
    environment: Optional[Mapping[str, str]] = None,
) -> RuntimeConfig:
    env = os.environ if environment is None else environment
    with Path(path).open(encoding="utf-8") as config_file:
        document = json.load(config_file)

    if not isinstance(document, dict):
        raise ValueError("simulator config must be a JSON object")
    mqtt_document = document.get("mqtt", {})
    settings = document.get("settings", {})
    device_documents = document.get("devices")
    if not isinstance(mqtt_document, dict) or not isinstance(settings, dict):
        raise ValueError("mqtt and settings must be JSON objects")
    if not isinstance(device_documents, list) or not device_documents:
        raise ValueError("devices must be a non-empty JSON array")

    mqtt = MqttConfig(
        host=env.get("MQTT_HOST", mqtt_document.get("host", "iot-mqtt")),
        port=_positive_number(
            env.get("MQTT_PORT", mqtt_document.get("port", 1883)),
            "MQTT port",
            int,
        ),
        keepalive_sec=_positive_number(
            mqtt_document.get("keepalive_sec", 60), "MQTT keepalive", int
        ),
        username=env.get("MQTT_USERNAME") or None,
        password=env.get("MQTT_PASSWORD") or None,
    )
    devices = [
        DeviceConfig(
            device_id=device["device_id"],
            initial_version=device["initial_version"],
            fault_mode=device.get("fault_mode", "none"),
        )
        for device in device_documents
        if isinstance(device, dict)
    ]
    if len(devices) != len(device_documents):
        raise ValueError("each device entry must be a JSON object")
    device_ids = [device.device_id for device in devices]
    if len(device_ids) != len(set(device_ids)):
        raise ValueError("device_id values must be unique")

    return RuntimeConfig(
        mqtt=mqtt,
        devices=devices,
        state_dir=Path(
            env.get(
                "SIMULATOR_STATE_DIR",
                settings.get("state_dir", "/app/state"),
            )
        ),
        telemetry_interval_sec=_positive_number(
            settings.get("telemetry_interval_sec", 5),
            "telemetry_interval_sec",
            float,
        ),
        download_timeout_sec=_positive_number(
            settings.get("download_timeout_sec", 15),
            "download_timeout_sec",
            float,
        ),
        reboot_delay_sec=_positive_number(
            settings.get("reboot_delay_sec", 2),
            "reboot_delay_sec",
            float,
        ),
    )
