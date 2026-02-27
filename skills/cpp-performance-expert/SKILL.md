---
name: cpp-performance-expert
description: Performance-first modern C++ engineering for C++20/C++23 systems with coroutines and high concurrency. Use when implementing, refactoring, reviewing, or debugging latency/throughput-critical C++ code, especially around coroutine design, lock contention, allocation pressure, cache locality, and memory ownership/lifetime safety.
---

# Cpp Performance Expert

## Objective

Design and implement production C++ with measurable performance gains while preserving correctness and maintainability.

## Default Stance

- Prioritize predictable latency and stable throughput over abstraction convenience.
- Minimize allocations, copies, indirection, and synchronization.
- Keep ownership explicit and lifetime boundaries easy to audit.
- Validate changes with profiling or benchmarks before claiming wins.

## Workflow

### 1. Frame Constraints First

- Capture SLO targets: p50/p99 latency, throughput, memory ceiling, and tail behavior.
- Identify hot paths and critical threads.
- Confirm platform, compiler flags, and active C++ standard mode.

### 2. Build a Cost Model

- Estimate per-request cost: syscalls, allocations, copies, context switches, and lock waits.
- Mark ownership transitions and lifetime boundaries.
- Define success metrics before writing code.

### 3. Apply C++20/23 and Coroutine Patterns

- Prefer structured concurrency and cancellation-aware control flow.
- Keep coroutine frames small; avoid capturing heavy objects by value.
- Split I/O wait, parse/compute, and emit stages to reduce head-of-line blocking.
- Prefer `std::span`/`std::string_view` and move semantics on zero-copy eligible paths.

### 4. Optimize Memory and Concurrency

- Prefer stack/local storage for tiny short-lived objects.
- Reuse buffers and arenas on hot paths.
- Reduce false sharing and lock contention; shard state by core/thread where possible.
- Use lock-free patterns only with a clear correctness argument and benchmark proof.

### 5. Verify and Guard

- Compile with strict warnings and sanitizers in debug/CI.
- Run microbenchmarks and representative load benchmarks.
- Reject changes that do not improve metrics or that regress tail latency.

### 6. Build and Test with Configured CMake/CTest

- Use `scripts/cmake-build-test.sh` to run configure, build, and tests.
- Keep machine-specific CMake/CTest location in `scripts/cmake-tools.conf`.
- Prefer config-file based tool discovery over hardcoding paths in commands.
- Update only `scripts/cmake-tools.conf` when moving to a different machine.

## Review Checklist

Load `references/perf-checklist.md` when running review-heavy tasks that need a concrete checklist for coroutine correctness, concurrency contention, and memory optimization.

## Build Scripts

- `scripts/cmake-tools.conf`
  - Store `CMAKE_CTEST_BIN_DIR` for machine-specific tool location.
  - Optionally override with `CMAKE_BIN` and `CTEST_BIN`.
- `scripts/cmake-build-test.sh`
  - Resolve cmake/ctest from config.
  - Run configure: `cmake -S <source> -B <build>`.
  - Run build: `cmake --build <build>`.
  - Run tests: `ctest --test-dir <build> --output-on-failure`.
  - Example:
    - `skills/cpp-performance-expert/scripts/cmake-build-test.sh --source . --build build --config skills/cpp-performance-expert/scripts/cmake-tools.conf`
