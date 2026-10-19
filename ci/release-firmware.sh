#!/usr/bin/env bash
set -euo pipefail

dist_dir="${1:?distribution directory is required}"
firmware_version="${2:?firmware version is required}"
release_root="${FIRMWARE_RELEASE_ROOT:-/opt/firmware-artifacts}"
mqtt_container="${MQTT_CONTAINER:-iot-mqtt}"
mqtt_topic="${MQTT_OTA_TOPIC:-device/esp32/ota}"

checksum_file="$dist_dir/firmware.sha256"
notify_file="$dist_dir/ota-notify.json"
release_file="firmware_${firmware_version}.bin"
source_firmware="$dist_dir/$release_file"
destination="$release_root/$release_file"
temporary_destination="${destination}.tmp.${BUILD_NUMBER:-manual}"

if [[ ! -s "$source_firmware" || ! -s "$checksum_file" || ! -s "$notify_file" ]]; then
    echo "Verified release files are incomplete in $dist_dir" >&2
    exit 1
fi

(
    cd "$dist_dir"
    sha256sum -c firmware.sha256
)

approved_sha256="$(cut -d ' ' -f 1 "$checksum_file")"

if [[ "${RELEASE_DRY_RUN:-0}" == "1" ]]; then
    echo "Dry run passed for $release_file (SHA-256: $approved_sha256)"
    exit 0
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

if ! docker inspect "$mqtt_container" >/dev/null 2>&1; then
    echo "MQTT container is unavailable: $mqtt_container" >&2
    exit 1
fi

docker exec -i "$mqtt_container" mosquitto_pub \
    -h 127.0.0.1 \
    -p 1883 \
    -q 1 \
    -t "$mqtt_topic" \
    -s < "$notify_file"

echo "OTA instruction published for $release_file"
