#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage:
  format.sh [--check] [file ...]

Behavior:
  - Uses clang-format with --style=file (requires .clang-format in current directory)
  - With explicit files: formats/checks only those files
  - Without files: formats/checks tracked C/C++ files from git ls-files
USAGE
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

check_mode=0
if [[ "${1:-}" == "--check" ]]; then
  check_mode=1
  shift
fi

if ! command -v clang-format >/dev/null 2>&1; then
  echo "clang-format not found in PATH" >&2
  exit 1
fi

if [[ ! -f .clang-format ]]; then
  echo ".clang-format not found in current directory: $(pwd)" >&2
  exit 1
fi

files=()
if [[ $# -gt 0 ]]; then
  for f in "$@"; do
    files+=("$f")
  done
else
  if ! command -v git >/dev/null 2>&1; then
    echo "git not found; pass file paths explicitly" >&2
    exit 1
  fi

  while IFS= read -r f; do
    files+=("$f")
  done < <(git ls-files '*.c' '*.cc' '*.cpp' '*.cxx' '*.h' '*.hh' '*.hpp' '*.hxx')
fi

if [[ ${#files[@]} -eq 0 ]]; then
  echo "No matching C/C++ files found" >&2
  exit 0
fi

if [[ $check_mode -eq 1 ]]; then
  clang-format --dry-run --Werror --style=file "${files[@]}"
else
  clang-format -i --style=file "${files[@]}"
fi
