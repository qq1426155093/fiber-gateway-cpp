#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="${1:-${ROOT_DIR}/build/http3-demo}"
CERT_FILE="${OUT_DIR}/cert.pem"
KEY_FILE="${OUT_DIR}/key.pem"
DAYS="${DAYS:-7}"

mkdir -p "${OUT_DIR}"

openssl req \
  -x509 \
  -newkey rsa:2048 \
  -sha256 \
  -nodes \
  -days "${DAYS}" \
  -subj "/CN=localhost" \
  -addext "subjectAltName=DNS:localhost,IP:127.0.0.1,IP:0:0:0:0:0:0:0:1" \
  -addext "keyUsage=digitalSignature,keyEncipherment" \
  -addext "extendedKeyUsage=serverAuth" \
  -keyout "${KEY_FILE}" \
  -out "${CERT_FILE}" >/dev/null 2>&1

chmod 600 "${KEY_FILE}"

echo "generated cert: ${CERT_FILE}"
echo "generated key : ${KEY_FILE}"
