---
name: code-format
description: Format C/C++ source files with clang-format using the repository .clang-format file. Use when requests mention format, reformat, style cleanup, clang-format, or pre-commit code style normalization for .c/.cc/.cpp/.cxx/.h/.hh/.hpp/.hxx files.
---

# Code Format

## Quick Workflow

1. Confirm `clang-format` is available.
2. Run formatting from the workspace root so `.clang-format` is discovered.
3. Prefer `scripts/format.sh` for repeatable formatting runs.
4. Format only requested files unless the user explicitly asks for a repo-wide pass.
5. Review changes with `git diff` before finishing.

## Commands

- Format specific files:
  - `skills/code-format/scripts/format.sh src/net/detail/StreamFd.cpp src/net/detail/DatagramFd.cpp`
- Check mode (no edits, fail on style drift):
  - `skills/code-format/scripts/format.sh --check src/net/detail/StreamFd.cpp`
- Format tracked C/C++ files in the repo:
  - `skills/code-format/scripts/format.sh`

## Rules

- Use `--style=file` and rely on workspace `.clang-format`.
- Do not use fallback styles that can hide missing config.
- Keep formatting changes scoped to task-related files by default.
- If `.clang-format` is missing, stop and report the issue.

## Scripts

- `scripts/format.sh`
  - Accept explicit file paths.
  - Support `--check` mode via `clang-format --dry-run --Werror`.
  - When no files are provided, discover tracked C/C++ files via `git ls-files`.
