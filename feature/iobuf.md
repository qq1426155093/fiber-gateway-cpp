# IoBuf Design

Goal: introduce an IO buffer that:
- owns memory via RAII,
- supports zero-copy readable slices,
- keeps refcounting explicit,
- can be chained for `readv` / `writev`.

## Memory Layout

`IoBuf` is split logically into:
- `ControlBlock`: refcount + capacity
- `DataBlock`: raw bytes used for socket IO

The first implementation uses one allocation:

```text
+----------------------+----------------------+
| ControlBlock         | aligned data bytes   |
+----------------------+----------------------+
```

This keeps the hot path to one allocation and one free.

## Ownership Model

`IoBuf` is a lightweight shared handle.

- Copy construction and copy assignment retain the same `ControlBlock`.
- Copying an `IoBuf` never copies payload bytes. It only increments the
  refcount and copies the current view state.
- Sharing a subrange explicitly still goes through `retain_slice(...)` or
  `unsafe_retain_slice(...)`.
- Slicing never copies payload bytes. It only creates a new view over the same
  `ControlBlock`.

Each `IoBuf` handle stores:
- `control_`
- `view_begin_`
- `view_end_`
- `pos_`
- `last_`

The byte ranges are:

```text
data_begin <= view_begin <= pos <= last <= view_end <= data_end
```

- `[pos, last)` is readable
- `[last, view_end)` is writable
- `[view_begin, pos)` is headroom inside the current view

## Refcount Semantics

Two slice APIs are exposed:

- `retain_slice(offset, len)`
  - safe path
  - increments refcount with `std::atomic_ref<uint32_t>` using CAS
- `unsafe_retain_slice(offset, len)`
  - non-atomic fast path
  - increments refcount with plain `++`

Important constraint:

- `unsafe_retain_slice(...)` must not race with any atomic retain/release on the
  same `ControlBlock`.
- The implementation does not track local/shared state internally.
- The caller is responsible for choosing the unsafe path only when the buffer is
  still thread-confined.

Release stays RAII-driven:

- `IoBuf` destructor decrements the refcount on the safe atomic path.
- When the refcount reaches `0`, the single allocation is freed.

This keeps destruction correct for shared handles while still allowing an
explicit non-atomic retain fast path before publication.

## Slice Semantics

`retain_slice(offset, len)` and `unsafe_retain_slice(offset, len)` slice from
the current readable range.

If the current readable range is `[pos, last)`, then:

- `slice_begin = pos + offset`
- `slice_end = slice_begin + len`

The returned slice has:

- `view_begin = slice_begin`
- `view_end = slice_end`
- `pos = slice_begin`
- `last = slice_end`

So a retained slice is a read-only view by default: it has readable bytes but
no writable tailroom.

## Mutable Operations

The first implementation keeps mutation simple:

- `commit(n)` advances `last`
- `consume(n)` advances `pos`
- `clear()` resets `pos = last = view_begin`
- `reset()` resets the handle back to the full underlying allocation

No resizing, reallocation, or compaction is provided yet.

## IoBufChain

`IoBufChain` keeps multiple `IoBuf` handles in order for vectored IO.

Responsibilities:
- append/prepend `IoBuf`
- sum total readable/writable bytes
- export readable segments as `writev` iovecs
- export writable segments as `readv` iovecs
- advance the chain with `consume(bytes)` / `commit(bytes)`
- `consume(bytes)` only advances read cursors
- `drop_empty_front()` reclaims an already-drained prefix
- `consume_and_compact(bytes)` advances and reclaims drained front nodes in one
  traversal

The chain caches aggregate readable/writable byte counts so hot-path consume and
commit operations do not need a pre-scan to validate byte ranges.

The chain owns nodes, while each node owns its `IoBuf` handle. Shared payload
ownership still lives in each handle's `ControlBlock`.

## Initial Scope

First implementation includes:
- self-allocated buffers only
- shared-handle `IoBuf`
- explicit safe/unsafe slice retain
- `IoBufChain`
- focused tests for ownership, slicing, and iovec export

Deferred:
- wrapping external memory
- custom allocators/deleters
- shared-write resize/compact logic
- replacing the existing HTTP `BufChain`
