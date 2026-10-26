import logging
import os
import signal
import threading
from pathlib import Path

import paho.mqtt.client as mqtt

from .config import load_runtime_config
from .device import DeviceSimulator
from .state import StateStore


LOGGER = logging.getLogger(__name__)


def create_client(device_id, mqtt_config):
    client = mqtt.Client(
        mqtt.CallbackAPIVersion.VERSION2,
        client_id=device_id,
        protocol=mqtt.MQTTv311,
    )
    if mqtt_config.username:
        client.username_pw_set(mqtt_config.username, mqtt_config.password)
    client.reconnect_delay_set(min_delay=1, max_delay=30)
    client.enable_logger()
    return client


def run():
    logging.basicConfig(
        level=os.environ.get("LOG_LEVEL", "INFO").upper(),
        format="%(asctime)s %(levelname)s %(name)s %(message)s",
    )
    config_path = Path(
        os.environ.get("SIMULATOR_CONFIG", "/app/simulator/config.json")
    )
    runtime = load_runtime_config(config_path)
    state_store = StateStore(runtime.state_dir)
    stop_event = threading.Event()
    devices = []

    def request_stop(signum, frame):
        LOGGER.info("shutdown requested by signal %s", signum)
        stop_event.set()

    signal.signal(signal.SIGTERM, request_stop)
    signal.signal(signal.SIGINT, request_stop)

    for device_config in runtime.devices:
        client = create_client(device_config.device_id, runtime.mqtt)
        device = DeviceSimulator(
            device_config,
            client,
            state_store,
            telemetry_interval_sec=runtime.telemetry_interval_sec,
            download_timeout_sec=runtime.download_timeout_sec,
            reboot_delay_sec=runtime.reboot_delay_sec,
        )
        client.on_connect = device.on_connect
        client.on_message = device.on_message
        client.connect_async(
            runtime.mqtt.host,
            runtime.mqtt.port,
            runtime.mqtt.keepalive_sec,
        )
        client.loop_start()
        device.start_telemetry()
        devices.append(device)

    LOGGER.info(
        "started %d simulated devices against %s:%d",
        len(devices),
        runtime.mqtt.host,
        runtime.mqtt.port,
    )
    stop_event.wait()

    for device in devices:
        device.stop()
        device.client.loop_stop()


if __name__ == "__main__":
    run()
