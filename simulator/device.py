import json
import logging
import random
import threading
import time
from dataclasses import dataclass
from typing import Callable

from .ota import OtaDownloadError, download_and_verify
from .protocol import (
    DEVICE_ID_PATTERN,
    VERSION_PATTERN,
    CommandValidationError,
    build_status,
    build_telemetry,
    parse_ota_command,
)
from .state import StateStore


LOGGER = logging.getLogger(__name__)
FAULT_MODES = {
    "none",
    "download_failed",
    "sha256_mismatch",
    "slow_download",
    "health_check_failed",
    "silent_after_received",
}


@dataclass(frozen=True)
class DeviceConfig:
    device_id: str
    initial_version: str
    fault_mode: str = "none"

    def __post_init__(self):
        if not DEVICE_ID_PATTERN.fullmatch(self.device_id):
            raise ValueError(f"invalid device_id: {self.device_id}")
        if not VERSION_PATTERN.fullmatch(self.initial_version):
            raise ValueError(f"invalid initial_version: {self.initial_version}")
        if self.fault_mode not in FAULT_MODES:
            raise ValueError(f"invalid fault_mode: {self.fault_mode}")


class DeviceSimulator:
    def __init__(
        self,
        config: DeviceConfig,
        mqtt_client,
        state_store: StateStore,
        *,
        downloader: Callable = download_and_verify,
        download_timeout_sec: float = 15,
        reboot_delay_sec: float = 2,
        telemetry_interval_sec: float = 5,
    ):
        self.config = config
        self.client = mqtt_client
        self.state_store = state_store
        self.downloader = downloader
        self.download_timeout_sec = download_timeout_sec
        self.reboot_delay_sec = reboot_delay_sec
        self.telemetry_interval_sec = telemetry_interval_sec
        self.command_topic = f"iot/v1/devices/{config.device_id}/ota/command"
        self.status_topic = f"iot/v1/devices/{config.device_id}/ota/status"
        self.telemetry_topic = f"iot/v1/devices/{config.device_id}/telemetry"
        self._boot_started_at = time.monotonic()
        self._ota_lock = threading.Lock()
        self._ota_in_progress = False
        self._stop_event = threading.Event()
        self._telemetry_thread = None
        self._temperature = 24.0 + random.random() * 3
        self._humidity = 48.0 + random.random() * 6

        saved_state = self.state_store.load(config.device_id)
        self.current_version = (
            saved_state.get("current_version", config.initial_version)
            if saved_state
            else config.initial_version
        )

    def on_connect(self, client, userdata, flags, reason_code, properties):
        if getattr(reason_code, "is_failure", False):
            LOGGER.error("%s MQTT connection rejected: %s", self.config.device_id, reason_code)
            return
        client.subscribe(self.command_topic, qos=1)
        LOGGER.info("%s subscribed to %s", self.config.device_id, self.command_topic)

    def on_message(self, client, userdata, message):
        if message.topic != self.command_topic:
            return
        threading.Thread(
            target=self.process_command,
            args=(message.payload,),
            name=f"ota-{self.config.device_id}",
            daemon=True,
        ).start()

    def start_telemetry(self):
        self._telemetry_thread = threading.Thread(
            target=self._telemetry_loop,
            name=f"telemetry-{self.config.device_id}",
            daemon=True,
        )
        self._telemetry_thread.start()

    def stop(self):
        self._stop_event.set()
        if self._telemetry_thread:
            self._telemetry_thread.join(timeout=self.telemetry_interval_sec + 1)
        self.client.disconnect()

    def process_command(self, payload):
        try:
            command = parse_ota_command(payload)
        except CommandValidationError as error:
            LOGGER.warning("%s rejected OTA command: %s", self.config.device_id, error.reason)
            if error.release_id:
                self._publish_status(
                    release_id=error.release_id,
                    stage="skipped" if error.reason == "target_mismatch" else "failed",
                    target_version=error.target_version or self.current_version,
                    reason=error.reason,
                )
            return

        with self._ota_lock:
            if self._ota_in_progress:
                self._publish_status(
                    release_id=command.release_id,
                    stage="failed",
                    target_version=command.version,
                    artifact_size=command.artifact_size,
                    reason="ota_busy",
                )
                return
            self._ota_in_progress = True

        try:
            previous_version = self.current_version
            if command.version == self.current_version:
                self._publish_status(
                    release_id=command.release_id,
                    stage="skipped",
                    target_version=command.version,
                    artifact_size=command.artifact_size,
                    reason="version_unchanged",
                )
                return

            self._publish_status(
                release_id=command.release_id,
                stage="received",
                target_version=command.version,
                artifact_size=command.artifact_size,
            )
            if self.config.fault_mode == "silent_after_received":
                LOGGER.warning("%s entering injected silent mode", self.config.device_id)
                self.client.disconnect()
                return

            self._publish_status(
                release_id=command.release_id,
                stage="downloading",
                target_version=command.version,
                artifact_size=command.artifact_size,
            )
            self.downloader(
                command,
                timeout_sec=self.download_timeout_sec,
                fault_mode=self.config.fault_mode,
            )
            self._publish_status(
                release_id=command.release_id,
                stage="verified",
                target_version=command.version,
                artifact_size=command.artifact_size,
            )
            self._publish_status(
                release_id=command.release_id,
                stage="rebooting",
                target_version=command.version,
                artifact_size=command.artifact_size,
            )
            time.sleep(self.reboot_delay_sec)
            self._boot_started_at = time.monotonic()
            self.current_version = command.version

            if self.config.fault_mode == "health_check_failed":
                self._publish_status(
                    release_id=command.release_id,
                    stage="rollback",
                    target_version=command.version,
                    artifact_size=command.artifact_size,
                    reason="health_check_failed",
                )
                time.sleep(self.reboot_delay_sec)
                self._boot_started_at = time.monotonic()
                self.current_version = previous_version
                self._publish_status(
                    release_id=command.release_id,
                    stage="rolled_back",
                    target_version=command.version,
                    artifact_size=command.artifact_size,
                    reason="health_check_failed",
                )
                return

            self._publish_status(
                release_id=command.release_id,
                stage="healthy",
                target_version=command.version,
                artifact_size=command.artifact_size,
            )
        except OtaDownloadError as error:
            self._publish_status(
                release_id=command.release_id,
                stage="failed",
                target_version=command.version,
                artifact_size=command.artifact_size,
                reason=error.reason,
            )
        finally:
            with self._ota_lock:
                self._ota_in_progress = False

    def _publish_status(
        self,
        *,
        release_id,
        stage,
        target_version,
        artifact_size=None,
        reason=None,
    ):
        status = build_status(
            device_id=self.config.device_id,
            release_id=release_id,
            stage=stage,
            reported_version=self.current_version,
            target_version=target_version,
            artifact_size=artifact_size,
            uptime_sec=self._uptime_sec(),
            reason=reason,
        )
        self._publish_json(self.status_topic, status, wait=True)
        self.state_store.save(
            self.config.device_id,
            {
                "current_version": self.current_version,
                "last_status": status,
            },
        )

    def _telemetry_loop(self):
        while not self._stop_event.wait(self.telemetry_interval_sec):
            self._temperature = min(32.0, max(18.0, self._temperature + random.uniform(-0.3, 0.3)))
            self._humidity = min(75.0, max(30.0, self._humidity + random.uniform(-0.8, 0.8)))
            telemetry = build_telemetry(
                device_id=self.config.device_id,
                version=self.current_version,
                temperature=self._temperature,
                humidity=self._humidity,
                rssi=random.randint(-67, -45),
                free_heap=random.randint(175000, 195000),
                uptime_sec=self._uptime_sec(),
            )
            try:
                self._publish_json(self.telemetry_topic, telemetry, wait=False)
            except RuntimeError as error:
                LOGGER.warning("%s telemetry publish failed: %s", self.config.device_id, error)

    def _publish_json(self, topic, document, *, wait):
        message_info = self.client.publish(
            topic,
            json.dumps(document, separators=(",", ":")),
            qos=1,
            retain=False,
        )
        if getattr(message_info, "rc", 0) != 0:
            raise RuntimeError(f"MQTT publish failed with rc={message_info.rc}")
        if wait:
            message_info.wait_for_publish(timeout=5)
            if hasattr(message_info, "is_published") and not message_info.is_published():
                raise RuntimeError("MQTT publish acknowledgement timed out")

    def _uptime_sec(self):
        return int(time.monotonic() - self._boot_started_at)
