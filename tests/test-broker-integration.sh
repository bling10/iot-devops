#!/usr/bin/env bash
set -euo pipefail

source_dist_dir="${1:?verified distribution directory is required}"
firmware_version="${2:?firmware version is required}"
mqtt_container="${MQTT_CONTAINER:-iot-mqtt}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fixture="$(mktemp -d)"
declare -a subscriber_pids=()

cleanup() {
    for subscriber_pid in "${subscriber_pids[@]}"; do
        if kill -0 "$subscriber_pid" 2>/dev/null; then
            kill "$subscriber_pid" 2>/dev/null || true
            wait "$subscriber_pid" 2>/dev/null || true
        fi
    done
    rm -rf "$fixture"
}
trap cleanup EXIT

release_file="firmware_${firmware_version}.bin"
mkdir -p "$fixture/dist" "$fixture/release"
cp "$source_dist_dir/$release_file" "$fixture/dist/"
cp "$source_dist_dir/firmware.sha256" "$fixture/dist/"

python3 - "$source_dist_dir/manifest.json" "$fixture/dist/ota-notify.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as file:
    manifest = json.load(file)

command = {
    "schema_version": 1,
    "message_type": "ota_command",
    "release_id": manifest["release_id"],
    "version": manifest["version"],
    "target": "esp32",
    "url": manifest["artifact_url"],
    "artifact_size": manifest["artifact_size"],
    "sha256": manifest["sha256"],
}
with open(sys.argv[2], "w", encoding="utf-8") as file:
    json.dump(command, file, ensure_ascii=False, indent=2)
    file.write("\n")
PY

timeout 15 docker exec "$mqtt_container" mosquitto_sub \
    -h 127.0.0.1 \
    -p 1883 \
    -q 1 \
    -t iot/v1/devices/sim-ci-001/ota/command \
    -C 1 > "$fixture/sim-ci-001.json" &
subscriber_pids+=("$!")
timeout 15 docker exec "$mqtt_container" mosquitto_sub \
    -h 127.0.0.1 \
    -p 1883 \
    -q 1 \
    -t iot/v1/devices/sim-ci-003/ota/command \
    -C 1 > "$fixture/sim-ci-003.json" &
subscriber_pids+=("$!")
sleep 1

OTA_TARGET_DEVICE_IDS='sim-ci-001,sim-ci-003' \
RELEASE_DRY_RUN=0 \
FIRMWARE_RELEASE_ROOT="$fixture/release" \
MQTT_CONTAINER="$mqtt_container" \
    bash "$repo_root/ci/release-firmware.sh" "$fixture/dist" "$firmware_version"

for subscriber_pid in "${subscriber_pids[@]}"; do
    wait "$subscriber_pid"
done
subscriber_pids=()

python3 - "$fixture/sim-ci-001.json" "$fixture/sim-ci-003.json" <<'PY'
import json
import sys

for payload_path in sys.argv[1:]:
    with open(payload_path, encoding="utf-8") as file:
        command = json.load(file)
    if (
        command.get("schema_version") != 1
        or command.get("message_type") != "ota_command"
        or command.get("target") != "esp32"
    ):
        raise SystemExit("Captured MQTT payload does not match the v1 contract")

print("Broker integration passed: 2 device topics, 2 valid v1 payloads")
PY
