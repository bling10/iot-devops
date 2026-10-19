#!/usr/bin/env bash
set -euo pipefail

repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root"

git diff-tree --check --no-commit-id --root -r HEAD

required_files=(
    "firmware/esp32-node/CMakeLists.txt"
    "firmware/esp32-node/main/main.c"
    "firmware/esp32-node/partitions.csv"
    "firmware/esp32-node/sdkconfig.defaults"
)

for required_file in "${required_files[@]}"; do
    if [[ ! -s "$required_file" ]]; then
        echo "Required source file is missing or empty: $required_file" >&2
        exit 1
    fi
done

grep -q '^CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y$' firmware/esp32-node/sdkconfig.defaults

ota_partition_count="$(awk -F ',' '
    /^[[:space:]]*ota_[01][[:space:]]*,/ { count++ }
    END { print count + 0 }
' firmware/esp32-node/partitions.csv)"

if [[ "$ota_partition_count" -ne 2 ]]; then
    echo "Expected ota_0 and ota_1 partitions, found: $ota_partition_count" >&2
    exit 1
fi

echo "Source baseline checks passed."
