#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

rm -rf "${ROOT_DIR}/web/data"
mkdir -p "${ROOT_DIR}/web/data"

echo "Cleaned ${ROOT_DIR}/web/data"
