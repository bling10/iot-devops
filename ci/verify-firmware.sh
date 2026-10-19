#!/usr/bin/env bash
set -euo pipefail

project_dir="${1:?project directory is required}"
dist_dir="${2:?distribution directory is required}"
firmware_version="${3:?firmware version is required}"
git_commit="${4:?git commit is required}"
build_number="${5:?build number is required}"
ota_server_host="${6:?OTA server host is required}"

project_dir="$(cd "$project_dir" && pwd)"
mkdir -p "$dist_dir"
dist_dir="$(cd "$dist_dir" && pwd)"

build_dir="$project_dir/build"
source_firmware="$build_dir/sample_project.bin"
project_description="$build_dir/project_description.json"
partition_table="$project_dir/partitions.csv"
release_file="firmware_${firmware_version}.bin"
release_path="$dist_dir/$release_file"

if [[ ! -s "$source_firmware" ]]; then
    echo "Firmware image is missing or empty: $source_firmware" >&2
    exit 1
fi

if [[ ! -s "$project_description" ]]; then
    echo "ESP-IDF project description is missing: $project_description" >&2
    exit 1
fi

python3 - "$project_description" "$firmware_version" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as file:
    description = json.load(file)

actual_version = description.get("project_version")
expected_version = sys.argv[2]
if actual_version != expected_version:
    raise SystemExit(
        f"Firmware version mismatch: expected {expected_version}, got {actual_version!r}"
    )
PY

partition_size_hex="$(awk -F ',' '
    /^[[:space:]]*ota_0[[:space:]]*,/ {
        value = $5
        gsub(/[[:space:]\r]/, "", value)
        print value
        exit
    }
' "$partition_table")"

if [[ ! "$partition_size_hex" =~ ^0x[0-9A-Fa-f]+$ ]]; then
    echo "Unable to read ota_0 partition size from $partition_table" >&2
    exit 1
fi

partition_limit=$((partition_size_hex))
firmware_size="$(stat -c '%s' "$source_firmware")"
warning_limit=$((partition_limit * 90 / 100))

if (( firmware_size > partition_limit )); then
    echo "Firmware size $firmware_size exceeds OTA partition limit $partition_limit" >&2
    exit 1
fi

cp "$source_firmware" "$release_path"

for artifact in \
    "$build_dir/sample_project.elf" \
    "$build_dir/sample_project.map" \
    "$build_dir/bootloader/bootloader.bin" \
    "$build_dir/partition_table/partition-table.bin" \
    "$build_dir/flasher_args.json"; do
    if [[ -f "$artifact" ]]; then
        cp "$artifact" "$dist_dir/"
    fi
done

if command -v esptool.py >/dev/null 2>&1; then
    esptool.py --chip esp32 image_info --version 2 "$release_path" > "$dist_dir/image-info.txt"
elif command -v esptool >/dev/null 2>&1; then
    esptool --chip esp32 image-info "$release_path" > "$dist_dir/image-info.txt"
else
    echo "esptool is unavailable; firmware structure cannot be verified." >&2
    exit 1
fi

grep -Fq 'Project name: sample_project' "$dist_dir/image-info.txt"
grep -Fq "App version: $firmware_version" "$dist_dir/image-info.txt"

(
    cd "$project_dir"
    idf.py size > "$dist_dir/size-report.txt"
    idf.py size-components > "$dist_dir/size-components-report.txt"
)

firmware_sha256="$(sha256sum "$release_path" | cut -d ' ' -f 1)"
printf '%s  %s\n' "$firmware_sha256" "$release_file" > "$dist_dir/firmware.sha256"

capacity_status="PASS"
if (( firmware_size >= warning_limit )); then
    capacity_status="WARN"
fi

release_id="jenkins-${build_number}-${git_commit:0:12}"
artifact_url="http://${ota_server_host}:8081/${release_file}"
generated_at="$(date -u '+%Y-%m-%dT%H:%M:%SZ')"

cat > "$dist_dir/manifest.json" <<EOF
{
  "release_id": "${release_id}",
  "version": "${firmware_version}",
  "build_number": ${build_number},
  "git_commit": "${git_commit}",
  "target": "esp32",
  "artifact_file": "${release_file}",
  "artifact_url": "${artifact_url}",
  "artifact_size": ${firmware_size},
  "partition_limit": ${partition_limit},
  "capacity_status": "${capacity_status}",
  "sha256": "${firmware_sha256}",
  "status": "ARTIFACT_VERIFIED",
  "generated_at": "${generated_at}"
}
EOF

cat > "$dist_dir/ota-notify.json" <<EOF
{
  "release_id": "${release_id}",
  "version": "${firmware_version}",
  "url": "${artifact_url}",
  "artifact_size": ${firmware_size},
  "sha256": "${firmware_sha256}"
}
EOF

cat > "$dist_dir/verification-summary.txt" <<EOF
status=ARTIFACT_VERIFIED
version=${firmware_version}
git_commit=${git_commit}
firmware_size=${firmware_size}
partition_limit=${partition_limit}
capacity_status=${capacity_status}
sha256=${firmware_sha256}
EOF

echo "Firmware verification passed: $release_file"
echo "SHA-256: $firmware_sha256"
echo "Capacity: $firmware_size / $partition_limit bytes ($capacity_status)"
