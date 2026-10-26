#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENV_FILE="${ENV_FILE:-/data/devops/secrets/iot-devops.env}"
IOT_COMPOSE="${PROJECT_ROOT}/deploy/iot/compose.yml"

# Docker recreates its Unix socket during startup. Pass its current numeric
# group ID into Jenkins so the non-root jenkins user can use the mounted API.
DOCKER_SOCKET=/var/run/docker.sock
if [[ ! -S "${DOCKER_SOCKET}" ]]; then
  echo "Docker socket is unavailable: ${DOCKER_SOCKET}" >&2
  exit 1
fi
DOCKER_GID="$(stat -c '%g' "${DOCKER_SOCKET}")"
if [[ ! "${DOCKER_GID}" =~ ^[0-9]+$ ]]; then
  echo "Invalid Docker socket group ID: ${DOCKER_GID}" >&2
  exit 1
fi
export DOCKER_GID

# Bind mounts must exist and be writable by their non-root container users.
RUNTIME_ROOT="$(docker compose --env-file "${ENV_FILE}" -f "${IOT_COMPOSE}" config --environment | sed -n 's/^RUNTIME_ROOT=//p')"
if [[ -z "${RUNTIME_ROOT}" || "${RUNTIME_ROOT}" != /* || "${RUNTIME_ROOT}" == / ]]; then
  echo "Invalid RUNTIME_ROOT in Compose environment" >&2
  exit 1
fi
MQTT_LOG_DIR="${RUNTIME_ROOT}/mqtt/logs"
install -d -m 0755 -o 1883 -g 1883 -- "${MQTT_LOG_DIR}"
if [[ -f "${MQTT_LOG_DIR}/mosquitto.log" && ! -L "${MQTT_LOG_DIR}/mosquitto.log" ]]; then
  chown 1883:1883 -- "${MQTT_LOG_DIR}/mosquitto.log"
fi

docker compose --env-file "${ENV_FILE}" -f "${PROJECT_ROOT}/deploy/gitlab/compose.yml" up -d
docker compose --env-file "${ENV_FILE}" -f "${PROJECT_ROOT}/deploy/jenkins/compose.yml" up -d
docker compose --env-file "${ENV_FILE}" -f "${IOT_COMPOSE}" up -d
