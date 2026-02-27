# C++ Performance Checklist

Use this checklist before merging latency/throughput-sensitive C++ changes.

## 1. Measurement Gate

- Define baseline workload and environment.
- Report at least one latency metric (`p50` or `p99`) and one throughput metric.
- Record memory footprint and allocation count on the hot path.
- Reject unmeasured performance claims.

## 2. Ownership and Lifetime

- Make ownership explicit (`unique_ptr`, value, reference, span/view).
- Avoid dangling `string_view`/`span` from temporaries.
- Keep object lifetime local where possible.
- Remove shared ownership from hot paths unless required by cross-owner lifetime.

## 3. Allocation and Memory Locality

- Eliminate avoidable heap allocations in hot loops.
- Reuse buffers for repeated request paths.
- Reserve capacity for vectors/strings with known upper bounds.
- Prefer contiguous storage for scan-heavy data.
- Check for large object moves/copies in tight loops.

## 4. Coroutine-Specific Checks

- Ensure coroutine frame does not capture large objects by value.
- Keep suspension boundaries explicit and minimal.
- Verify cancellation/timeout paths release resources promptly.
- Avoid blocking syscalls inside coroutine continuations.
- Confirm awaiters preserve thread-affinity assumptions.

## 5. Concurrency and Contention

- Measure lock hold time and contention probability.
- Reduce shared mutable state; prefer sharding or message passing.
- Check false sharing on frequently updated fields.
- Validate atomic ordering is minimal and correct.
- Avoid oversubscription from unbounded task spawning.

## 6. API Surface and Copy Cost

- Prefer `std::span`/`std::string_view` for read-only non-owning inputs.
- Use move-aware interfaces for transfer of ownership.
- Return by value only when copy elision/move is expected and cheap.
- Keep serialization/parsing paths zero-copy where lifetime rules allow.

## 7. Regression Safety

- Run sanitizer builds (ASan/UBSan/TSan as applicable).
- Add or update benchmark cases that protect the optimized path.
- Add invariants/assertions in debug builds for concurrency assumptions.
- Confirm no throughput gain is hiding p99 regression.
