# Stream Design (TcpStream / UnixStream)

Goal: shared stream read/write mechanics for TCP and Unix domain sockets that:
- allow reads/writes from any EventLoop thread,
- resume on the caller's loop,
- permit one read waiter and one write waiter concurrently,
- allow read+write overlap.

## Core: detail::StreamFd

`StreamFd` owns the fd and IO-loop state:
- `EventLoop &loop`
- `Poller::Item item`
- `IoEvent watching` (Read/Write mask)
- `ReadWaiter *read_waiter`
- `WriteWaiter *write_waiter`
- `int fd`

All poller operations and fd I/O happen on the IO loop.

### Waiters

Two waiter types (read/write), each contains:
- `StreamFd *stream`
- `EventLoop *resume_loop`
- `std::coroutine_handle<> handle`
- buffer + length
- `IoResult<size_t> result`
- `std::atomic<State> state`
- `NotifyEntry start_entry` (posted to IO loop)
- `NotifyEntry cancel_entry` (posted to IO loop for cancellation)
- `NotifyEntry resume_entry` (posted to resume loop)

State machine:
- `Waiting -> Notified -> Resumed`
- `Waiting/Notified -> Canceled`

Resume is always deferred via `resume_entry` on the caller loop.

### Cross-loop initiation

`await_suspend` captures the caller loop as `resume_loop`.

If caller loop == IO loop:
- Try immediate non-blocking read/write.
- If success or error (not WouldBlock), return `false` (no suspend).
- Otherwise install waiter + poller and return `true`.

If caller loop != IO loop:
- Always post `start_entry` to IO loop and return `true`.
- `start_entry` performs read/write attempt and installs poller if needed.

### Cancellation

Awaiter destruction calls `cancel_read` / `cancel_write`.
- If on IO loop, cancel directly.
- Otherwise post `cancel_entry` to IO loop.
- Cancellation removes the waiter (if installed) and does **not** resume the coroutine.

`close()` is different: it completes any waiters with `IoErr::Canceled` and
resumes them normally (so the coroutine observes the error).

### Poller integration

`StreamFd` maintains `watching` as Read/Write mask. When waiters change:
- Add poller item if mask transitions `None -> Read/Write`.
- Mod poller item on `Read <-> Write` or `Read|Write` transitions.
- Del poller item if mask transitions to `None`.

On poller events:
- If Read event and read waiter exists, attempt read.
- If Write event and write waiter exists, attempt write.
- Each completion posts resume to the caller loop.

## Public wrappers

### TcpStream / UnixStream

Thin wrappers around `detail::StreamFd`, identical APIs:

```cpp
class TcpStream {
public:
    explicit TcpStream(fiber::event::EventLoop &loop, int fd);
    ~TcpStream();

    bool valid() const noexcept;
    int fd() const noexcept;
    void close();

    ReadAwaiter read(void *buf, size_t len) noexcept;
    WriteAwaiter write(const void *buf, size_t len) noexcept;
};
```

`UnixStream` is identical.

## Error semantics

- `read`/`write` return `IoErr::Busy` when a waiter already exists for the op.
- `read` returns size `0` on EOF (not an error).
- `close` completes waiting ops with `IoErr::Canceled`.
- `cancel` (awaiter destruction) does not resume.

## Test notes

- Same-loop read/write immediate completion when data is available.
- Cross-loop read/write resumes on caller loop (thread ID check).
- Busy behavior for multiple readers/writers.
- Read+write overlap (both can wait).
- Cancellation: awaiter destroyed while waiting should not resume.
- Close: waiter resumes with `IoErr::Canceled`.

## TLS Stream (detail::TlsStreamFd)

`TlsStreamFd` drives BoringSSL directly on a non-blocking fd and reuses the
poller wait strategy from `StreamFd`.

- Holds `SSL*` + fd and calls `SSL_read`/`SSL_write` directly.
- On `SSL_ERROR_WANT_READ/WRITE`, waits for the corresponding fd event and
  retries internally until completion.
- Provides `handshake()` and `shutdown()` awaiters in addition to `read/write`.
- Only one in-flight TLS operation at a time (read/write/handshake/shutdown).
- Awaiters must run on the owning `EventLoop` (no cross-thread waits).
- `close()` cancels the waiter with `IoErr::Canceled` and frees `SSL*` while
  leaving fd ownership to the caller (transport owns the socket).
