#!/usr/bin/env bash
set -euo pipefail

project_dir="${1:?project directory is required}"
firmware_version="${2:?firmware version is required}"

if [[ ! "$firmware_version" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "Invalid firmware version: $firmware_version" >&2
    exit 1
fi

if ! command -v idf.py >/dev/null 2>&1; then
    if [[ -z "${IDF_PATH:-}" || ! -f "$IDF_PATH/export.sh" ]]; then
        echo "ESP-IDF environment is unavailable." >&2
        exit 1
    fi
    # shellcheck disable=SC1090
    source "$IDF_PATH/export.sh"
fi

export FIRMWARE_VERSION="$firmware_version"

cd "$project_dir"
idf.py set-target esp32
idf.py build
