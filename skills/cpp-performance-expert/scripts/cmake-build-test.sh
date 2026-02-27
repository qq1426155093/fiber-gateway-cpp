#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage:
  cmake-build-test.sh [--source DIR] [--build DIR] [--config FILE] [--target NAME] [--no-test]

Behavior:
  - Loads CMake/CTest location from config file.
  - Runs configure + build.
  - Runs ctest unless --no-test is given.
USAGE
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_DIR="$(pwd)"
BUILD_DIR="${SOURCE_DIR}/build"
CONFIG_FILE="${SCRIPT_DIR}/cmake-tools.conf"
BUILD_TARGET=""
RUN_TESTS=1

while [[ $# -gt 0 ]]; do
  case "$1" in
  --source)
    SOURCE_DIR="$2"
    shift 2
    ;;
  --build)
    BUILD_DIR="$2"
    shift 2
    ;;
  --config)
    CONFIG_FILE="$2"
    shift 2
    ;;
  --target)
    BUILD_TARGET="$2"
    shift 2
    ;;
  --no-test)
    RUN_TESTS=0
    shift
    ;;
  -h | --help)
    usage
    exit 0
    ;;
  *)
    echo "unknown option: $1" >&2
    usage
    exit 1
    ;;
  esac
done

if [[ ! -f "${CONFIG_FILE}" ]]; then
  echo "config file not found: ${CONFIG_FILE}" >&2
  exit 1
fi

# shellcheck disable=SC1090
source "${CONFIG_FILE}"

CM_BIN="${CMAKE_BIN:-}"
CT_BIN="${CTEST_BIN:-}"

if [[ -z "${CM_BIN}" || -z "${CT_BIN}" ]]; then
  BIN_DIR="${CMAKE_CTEST_BIN_DIR:-}"
  if [[ -n "${BIN_DIR}" ]]; then
    [[ -n "${CM_BIN}" ]] || CM_BIN="${BIN_DIR%/}/cmake"
    [[ -n "${CT_BIN}" ]] || CT_BIN="${BIN_DIR%/}/ctest"
  fi
fi

if [[ -z "${CM_BIN}" ]]; then
  CM_BIN="$(command -v cmake || true)"
fi
if [[ -z "${CT_BIN}" ]]; then
  CT_BIN="$(command -v ctest || true)"
fi

if [[ -z "${CM_BIN}" || ! -x "${CM_BIN}" ]]; then
  echo "cmake not executable: ${CM_BIN:-<empty>}" >&2
  exit 1
fi
if [[ -z "${CT_BIN}" || ! -x "${CT_BIN}" ]]; then
  echo "ctest not executable: ${CT_BIN:-<empty>}" >&2
  exit 1
fi

cmake_configure_cmd=("${CM_BIN}" -S "${SOURCE_DIR}" -B "${BUILD_DIR}")
cmake_build_cmd=("${CM_BIN}" --build "${BUILD_DIR}")

if [[ -n "${BUILD_TARGET}" ]]; then
  cmake_build_cmd+=(--target "${BUILD_TARGET}")
fi

ctest_cmd=("${CT_BIN}" --test-dir "${BUILD_DIR}" --output-on-failure)

"${cmake_configure_cmd[@]}"
"${cmake_build_cmd[@]}"

if [[ "${RUN_TESTS}" -eq 1 ]]; then
  "${ctest_cmd[@]}"
fi
