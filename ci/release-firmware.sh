#!/usr/bin/env bash
set -euo pipefail

dist_dir="${1:?distribution directory is required}"
firmware_version="${2:?firmware version is required}"
release_root="${FIRMWARE_RELEASE_ROOT:-/opt/firmware-artifacts}"
mqtt_container="${MQTT_CONTAINER:-iot-mqtt}"
raw_target_device_ids="${OTA_TARGET_DEVICE_IDS:-}"

checksum_file="$dist_dir/firmware.sha256"
notify_file="$dist_dir/ota-notify.json"
release_file="firmware_${firmware_version}.bin"
source_firmware="$dist_dir/$release_file"
destination="$release_root/$release_file"
temporary_destination="${destination}.tmp.${BUILD_NUMBER:-manual}"

normalized_target_device_ids="${raw_target_device_ids//,/ }"
normalized_target_device_ids="${normalized_target_device_ids//$'\r'/ }"
normalized_target_device_ids="${normalized_target_device_ids//$'\n'/ }"
normalized_target_device_ids="${normalized_target_device_ids//$'\t'/ }"
read -r -a requested_device_ids <<< "$normalized_target_device_ids"

declare -a target_device_ids=()
declare -A seen_device_ids=()
for device_id in "${requested_device_ids[@]}"; do
    if [[ ! "$device_id" =~ ^[a-z0-9][a-z0-9-]{0,63}$ ]]; then
        echo "Invalid OTA target device ID: $device_id" >&2
        exit 1
    fi
    if [[ -z "${seen_device_ids[$device_id]+present}" ]]; then
        seen_device_ids["$device_id"]=1
        target_device_ids+=("$device_id")
    fi
done

if (( ${#target_device_ids[@]} == 0 )); then
    echo "OTA_TARGET_DEVICE_IDS must contain at least one valid device ID." >&2
    exit 1
fi

if [[ ! -s "$source_firmware" || ! -s "$checksum_file" || ! -s "$notify_file" ]]; then
    echo "Verified release files are incomplete in $dist_dir" >&2
    exit 1
fi

(
    cd "$dist_dir"
    sha256sum -c firmware.sha256
)

approved_sha256="$(cut -d ' ' -f 1 "$checksum_file")"

python3 - "$notify_file" "$firmware_version" "$approved_sha256" "$source_firmware" <<'PY'
import json
import os
import sys

notify_path, expected_version, expected_sha256, firmware_path = sys.argv[1:]
with open(notify_path, encoding="utf-8") as file:
    command = json.load(file)

expected_values = {
    "schema_version": 1,
    "message_type": "ota_command",
    "version": expected_version,
    "target": "esp32",
    "artifact_size": os.path.getsize(firmware_path),
    "sha256": expected_sha256,
}
for field, expected in expected_values.items():
    if command.get(field) != expected:
        raise SystemExit(
            f"OTA command field {field!r} does not match the verified artifact"
        )

for field in ("release_id", "url"):
    if not isinstance(command.get(field), str) or not command[field]:
        raise SystemExit(f"OTA command field {field!r} is missing or empty")
PY

if [[ "${RELEASE_DRY_RUN:-0}" == "1" ]]; then
    echo "Dry run passed for $release_file (SHA-256: $approved_sha256)"
    echo "Target device count: ${#target_device_ids[@]}"
    for device_id in "${target_device_ids[@]}"; do
        echo "Target topic: iot/v1/devices/${device_id}/ota/command"
    done
    exit 0
fi

if ! docker inspect "$mqtt_container" >/dev/null 2>&1; then
    echo "MQTT container is unavailable: $mqtt_container" >&2
    exit 1
fi

mkdir -p "$release_root"

if [[ -f "$destination" ]]; then
    source_sha256="$(sha256sum "$source_firmware" | cut -d ' ' -f 1)"
    destination_sha256="$(sha256sum "$destination" | cut -d ' ' -f 1)"
    if [[ "$source_sha256" != "$destination_sha256" ]]; then
        echo "Refusing to overwrite an existing release with different content: $destination" >&2
        exit 1
    fi
    echo "Release artifact already exists with the approved SHA-256; reusing it."
else
    trap 'rm -f "$temporary_destination"' EXIT
    install -m 0644 "$source_firmware" "$temporary_destination"
    mv "$temporary_destination" "$destination"
    trap - EXIT
fi

destination_sha256="$(sha256sum "$destination" | cut -d ' ' -f 1)"
if [[ "$approved_sha256" != "$destination_sha256" ]]; then
    echo "Published artifact SHA-256 does not match the approved artifact." >&2
    exit 1
fi

declare -a published_device_ids=()
declare -a failed_device_ids=()
for device_id in "${target_device_ids[@]}"; do
    mqtt_topic="iot/v1/devices/${device_id}/ota/command"
    if docker exec -i "$mqtt_container" mosquitto_pub \
        -h 127.0.0.1 \
        -p 1883 \
        -q 1 \
        -t "$mqtt_topic" \
        -s < "$notify_file"; then
        published_device_ids+=("$device_id")
        echo "OTA instruction published: $device_id -> $mqtt_topic"
    else
        failed_device_ids+=("$device_id")
        echo "OTA instruction publish failed: $device_id -> $mqtt_topic" >&2
    fi
done

echo "OTA publish summary: succeeded=${#published_device_ids[@]}, failed=${#failed_device_ids[@]}"
if (( ${#failed_device_ids[@]} > 0 )); then
    printf 'Failed device IDs: %s\n' "${failed_device_ids[*]}" >&2
    exit 1
fi

echo "OTA instruction published for $release_file to ${#published_device_ids[@]} device(s)"
