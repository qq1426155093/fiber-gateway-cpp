# Repository Guidelines

## Project Structure & Module Organization
Source lives under `src/`. The entry point is `src/main.cpp`. Shared utilities are grouped in `src/common/`, with memory helpers in `src/common/mem/` and JSON helpers in `src/common/json/`. Headers and implementations stay together (e.g., `Buffer.h` + `Buffer.cpp`). Tests live in `tests/` and are wired through CTest/GoogleTest.

## Build, Test, and Development Commands
This project uses CMake and targets C++23. Typical local workflow:

```bash
cmake -S . -B build
cmake --build build
./build/fiber
```

If you use CLion, it will generate `cmake-build-debug/` in the repo; do not commit build outputs. Tests can be enabled (default) and run via:

```bash
ctest --test-dir build
```

## Coding Style & Naming Conventions
Follow existing C++23 style: 4-space indentation, braces on the same line, and namespaces under `fiber::...`. Class and type names use PascalCase (e.g., `Buffer`, `Generator`). Header guards follow `FIBER_<NAME>_H`. Keep includes local and explicit (e.g., `#include "../mem/Buffer.h"`). Prefer small, focused headers and keep implementations in `.cpp` files.

## Performance & Memory Requirements
Code in this project is performance-first. Pay close attention to memory allocation and release efficiency, and reduce dynamic allocation churn in latency-sensitive paths. In hot code paths, do not use allocation-heavy standard containers such as `std::string` and `std::vector`; prefer reusable buffers, fixed-size structures, or custom memory-managed types.

## Testing Guidelines
Tests use GoogleTest and CTest. Add new files under `tests/`, name them `*Test.cpp`, and register them by adding sources to the `fiber_tests` target in `CMakeLists.txt`. Run `ctest --test-dir build` after building. Keep tests small and focused on one behavior.

## Commit & Pull Request Guidelines
Use Conventional Commits with clear scopes: `type(scope): subject`. Preferred types: `feat`, `fix`, `refactor`, `perf`, `test`, `build`, `docs`, `chore`. Keep scopes aligned with modules (e.g., `core`, `json`, `mem`, `build`). Example: `feat(json): add generator state stack`. If a change is breaking, add a `BREAKING CHANGE:` footer.

For pull requests, use the same format for the title, include a short summary and motivation, link related issues, and list the exact build/test commands you ran (e.g., `cmake --build build`). Add screenshots only when behavior is user-visible.
