#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
CERT_DIR="${CERT_DIR:-${BUILD_DIR}/http3-demo}"
BIN="${BUILD_DIR}/http3_demo_lsquic"
PORT="${1:-8443}"

if ! [[ "${PORT}" =~ ^[0-9]+$ ]] || ((PORT < 1 || PORT > 65535)); then
  echo "invalid port: ${PORT}" >&2
  exit 1
fi

CERT_FILE="${CERT_DIR}/cert.pem"
KEY_FILE="${CERT_DIR}/key.pem"

if [[ ! -s "${CERT_FILE}" || ! -s "${KEY_FILE}" ]]; then
  "${ROOT_DIR}/scripts/gen_http3_demo_cert.sh" "${CERT_DIR}"
fi

if [[ ! -x "${BIN}" ]]; then
  CMAKE_BIN="${CMAKE_BIN:-}"
  if [[ -z "${CMAKE_BIN}" ]]; then
    if command -v cmake >/dev/null 2>&1; then
      CMAKE_BIN="cmake"
    elif [[ -x "${HOME}/Desktop/Software/clion/bin/cmake/linux/x64/bin/cmake" ]]; then
      CMAKE_BIN="${HOME}/Desktop/Software/clion/bin/cmake/linux/x64/bin/cmake"
    else
      echo "cmake not found; set CMAKE_BIN or install cmake" >&2
      exit 1
    fi
  fi

  "${CMAKE_BIN}" -S "${ROOT_DIR}" -B "${BUILD_DIR}"
  "${CMAKE_BIN}" --build "${BUILD_DIR}" --target http3_demo_lsquic -j
fi

echo "starting: ${BIN} ${PORT} ${CERT_FILE} ${KEY_FILE}"
echo "verify : curl --http3 -k https://127.0.0.1:${PORT}/"
exec "${BIN}" "${PORT}" "${CERT_FILE}" "${KEY_FILE}"
