# Stream ET/active_ Design Update

Goal: optional ET mode for streams with a lightweight `active_` bit cache to
reduce redundant epoll wakeups while keeping cross-loop read/write semantics.

## Key epoll ET rule (behavioral constraint)

With EPOLLET, the application must read/write until `EAGAIN` to guarantee
future edge notifications. If the fd remains readable/writable and no `EAGAIN`
is observed, the kernel may not generate another edge even if more data arrives
later because the "readable" state did not transition.

Therefore, any ET-based strategy must treat `EAGAIN` as the only reliable
signal to clear a cached readiness bit and re-arm interest.

## active_ bit cache

Maintain a small atomic bitset in `StreamFd`:

- `Readable` bit: indicates caller may attempt `read()` without waiting.
- `Writable` bit: indicates caller may attempt `write()` without waiting.
- `Error` bit: indicates fd is in error/hup state; operations fail with cached error.

Suggested initial values:
- `Readable = 0`, `Writable = 1`, `Error = 0`.

### Bit transitions

**On epoll events (IO loop):**
- If Read event: set `Readable`.
- If Write event: set `Writable`.
- If Error/Hup: set `Error`, cache `IoErr`, del from poller.

**On read attempt:**
- If `Error` set: return cached error immediately.
- If `Readable` is 0 and no waiter: wait (arm/read event).
- If `read()` returns `EAGAIN`: clear `Readable` and re-arm Read.
- If `read()` returns data or 0: keep `Readable` = 1 (data might remain).

**On write attempt:**
- If `Error` set: return cached error immediately.
- If `Writable` is 0 and no waiter: wait (arm/write event).
- If `write()` returns `EAGAIN`: clear `Writable` and re-arm Write.
- If `write()` writes some bytes: keep `Writable` = 1.

**On hard I/O error:**
- Set `Error` and cache error.
- Del fd from poller and fail all pending waiters with cached error.

### Why not use `size < buf_size`?

Short read/write is not a reliable indicator of readiness state. Only `EAGAIN`
means "not ready" for non-blocking fds.

## Poller changes (required for ET)

Per-item flags are needed; ET cannot be global. Add to `Poller::Item`:

- `bool edge_triggered = false;`

Update `Poller::to_epoll_events` to OR `EPOLLET` when `edge_triggered` is true.

