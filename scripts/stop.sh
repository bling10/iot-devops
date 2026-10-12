#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENV_FILE="${ENV_FILE:-/data/devops/secrets/iot-devops.env}"

docker compose --env-file "${ENV_FILE}" -f "${PROJECT_ROOT}/deploy/iot/compose.yml" down
docker compose --env-file "${ENV_FILE}" -f "${PROJECT_ROOT}/deploy/jenkins/compose.yml" down
docker compose --env-file "${ENV_FILE}" -f "${PROJECT_ROOT}/deploy/gitlab/compose.yml" down
