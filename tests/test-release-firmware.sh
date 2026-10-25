#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
release_script="$repo_root/ci/release-firmware.sh"
test_root="$(mktemp -d)"
trap 'rm -rf "$test_root"' EXIT

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

assert_contains() {
    local expected="$1"
    local file="$2"
    grep -Fq -- "$expected" "$file" || fail "$file does not contain: $expected"
}

create_fixture() {
    local fixture="$1"
    local firmware_size
    local firmware_sha256
    mkdir -p "$fixture/dist" "$fixture/bin" "$fixture/payloads"
    printf 'test firmware\n' > "$fixture/dist/firmware_v1.2.3.bin"
    (
        cd "$fixture/dist"
        sha256sum firmware_v1.2.3.bin > firmware.sha256
    )
    firmware_size="$(stat -c '%s' "$fixture/dist/firmware_v1.2.3.bin")"
    firmware_sha256="$(cut -d ' ' -f 1 "$fixture/dist/firmware.sha256")"
    cat > "$fixture/dist/ota-notify.json" <<JSON
{
  "schema_version": 1,
  "message_type": "ota_command",
  "release_id": "jenkins-42-abcdef7890ab",
  "version": "v1.2.3",
  "target": "esp32",
  "url": "http://example.test/firmware_v1.2.3.bin",
  "artifact_size": ${firmware_size},
  "sha256": "${firmware_sha256}"
}
JSON
    cat > "$fixture/bin/docker" <<'SH'
#!/usr/bin/env bash
set -euo pipefail

printf '%s\n' "$*" >> "$FAKE_DOCKER_LOG"
if [[ "${1:-}" == "inspect" ]]; then
    exit 0
fi

topic=""
args=("$@")
for ((index = 0; index < ${#args[@]}; index++)); do
    if [[ "${args[$index]}" == "-t" ]]; then
        topic="${args[$((index + 1))]}"
        break
    fi
done

sequence="$(find "$FAKE_DOCKER_PAYLOAD_DIR" -type f | wc -l)"
cat > "$FAKE_DOCKER_PAYLOAD_DIR/${sequence}.json"
if [[ -n "${FAKE_DOCKER_FAIL_TOPIC:-}" && "$topic" == "$FAKE_DOCKER_FAIL_TOPIC" ]]; then
    exit 17
fi
SH
    chmod +x "$fixture/bin/docker"
}

run_release() {
    local fixture="$1"
    local targets="$2"
    local dry_run="$3"
    env \
        PATH="$fixture/bin:$PATH" \
        FAKE_DOCKER_LOG="$fixture/docker.log" \
        FAKE_DOCKER_PAYLOAD_DIR="$fixture/payloads" \
        FAKE_DOCKER_FAIL_TOPIC="${FAKE_DOCKER_FAIL_TOPIC:-}" \
        FIRMWARE_RELEASE_ROOT="$fixture/release" \
        OTA_TARGET_DEVICE_IDS="$targets" \
        RELEASE_DRY_RUN="$dry_run" \
        BUILD_NUMBER=42 \
        bash "$release_script" "$fixture/dist" v1.2.3
}

dry_fixture="$test_root/dry"
create_fixture "$dry_fixture"
dry_output="$(run_release "$dry_fixture" $'sim-001, sim-003\nsim-001' 1)"
[[ "$dry_output" == *"iot/v1/devices/sim-001/ota/command"* ]] || fail "dry-run omitted sim-001 topic"
[[ "$dry_output" == *"iot/v1/devices/sim-003/ota/command"* ]] || fail "dry-run omitted sim-003 topic"
[[ ! -e "$dry_fixture/docker.log" ]] || fail "dry-run invoked Docker"
[[ ! -e "$dry_fixture/release/firmware_v1.2.3.bin" ]] || fail "dry-run copied the firmware"

publish_fixture="$test_root/publish"
create_fixture "$publish_fixture"
run_release "$publish_fixture" $'sim-001, sim-003\nsim-001' 0 >/dev/null
[[ -s "$publish_fixture/release/firmware_v1.2.3.bin" ]] || fail "release firmware was not copied"
[[ "$(grep -c 'mosquitto_pub' "$publish_fixture/docker.log")" == "2" ]] || fail "expected exactly two MQTT publishes"
assert_contains "-q 1 -t iot/v1/devices/sim-001/ota/command -s" "$publish_fixture/docker.log"
assert_contains "-q 1 -t iot/v1/devices/sim-003/ota/command -s" "$publish_fixture/docker.log"
if grep -Eq '(^|[[:space:]])-r($|[[:space:]])' "$publish_fixture/docker.log"; then
    fail "MQTT publish unexpectedly used retain"
fi
for payload in "$publish_fixture"/payloads/*.json; do
    cmp -s "$publish_fixture/dist/ota-notify.json" "$payload" || fail "published payload changed"
done

invalid_fixture="$test_root/invalid"
create_fixture "$invalid_fixture"
if run_release "$invalid_fixture" 'sim-001,../invalid' 0 >/dev/null 2>&1; then
    fail "invalid device ID was accepted"
fi
[[ ! -e "$invalid_fixture/docker.log" ]] || fail "invalid input reached Docker"
[[ ! -e "$invalid_fixture/release/firmware_v1.2.3.bin" ]] || fail "invalid input copied the firmware"

empty_fixture="$test_root/empty"
create_fixture "$empty_fixture"
if run_release "$empty_fixture" ' ,  ' 1 >/dev/null 2>&1; then
    fail "empty target list was accepted"
fi

partial_fixture="$test_root/partial"
create_fixture "$partial_fixture"
if FAKE_DOCKER_FAIL_TOPIC='iot/v1/devices/sim-fail/ota/command' \
    run_release "$partial_fixture" 'sim-001,sim-fail,sim-003' 0 >/dev/null 2>&1; then
    fail "partial MQTT failure returned success"
fi
[[ "$(grep -c 'mosquitto_pub' "$partial_fixture/docker.log")" == "3" ]] || fail "partial failure did not attempt every target"

echo "release-firmware integration tests passed"
