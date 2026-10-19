#!/usr/bin/env sh
set -eu

title="${1:?notification title is required}"
message="${2:?notification message is required}"

if [ -z "${DINGTALK_TOKEN:-}" ]; then
    echo "Jenkins credential dingtalk-robot-token is unavailable." >&2
    exit 1
fi

escape_json() {
    printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'
}

escaped_title="$(escape_json "$title")"
escaped_message="$(escape_json "$message")"
payload="{\"msgtype\":\"markdown\",\"markdown\":{\"title\":\"${escaped_title}\",\"text\":\"${escaped_message}\"}}"

curl --fail --silent --show-error \
    -H 'Content-Type: application/json' \
    --data-binary "$payload" \
    "https://oapi.dingtalk.com/robot/send?access_token=${DINGTALK_TOKEN}"
