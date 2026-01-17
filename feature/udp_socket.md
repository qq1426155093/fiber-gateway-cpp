# UdpSocket Design (Datagram, Linux)

## Goals
- Provide coroutine-based UDP send/receive on a single socket.
- Use datagram semantics: each recv returns the peer address.
- Support `SO_REUSEPORT` for multi-socket servers.
- Allow one recv waiter and one send waiter concurrently.
- Require recv/send to run on the socket's `EventLoop` thread.

## API Sketch
```cpp
namespace fiber::net {

struct UdpBindOptions {
    bool reuse_addr = true;
    bool reuse_port = false;
    bool v6_only = false;
};

struct UdpRecvResult {
    size_t size = 0;
    SocketAddress peer{};
};

class UdpSocket : public common::NonCopyable, public common::NonMovable {
public:
    explicit UdpSocket(event::EventLoop &loop);
    ~UdpSocket();

    common::IoResult<void> bind(const SocketAddress &addr,
                                const UdpBindOptions &options);
    bool valid() const noexcept;
    int fd() const noexcept;
    void close();

    RecvFromAwaiter recv_from(void *buf, size_t len) noexcept;
    SendToAwaiter send_to(const void *buf, size_t len,
                          const SocketAddress &peer) noexcept;
};

} // namespace fiber::net
```

## Bind Semantics
- Creates a non-blocking datagram socket (`SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC`).
- Applies options:
  - `reuse_addr` -> `SO_REUSEADDR`
  - `reuse_port` -> `SO_REUSEPORT` (or `NotSupported` if unavailable)
  - `v6_only` -> `IPV6_V6ONLY` for IPv6 sockets
- Calls `bind()` with the provided address.

## Recv/Send Flow
- `recv_from` attempts `recvfrom()` immediately.
  - On success, returns `{size, peer}`.
  - On `EAGAIN`, waits for read readiness and retries on resume.
- `send_to` attempts `sendto()` immediately.
  - On `EAGAIN`, waits for write readiness and retries on resume.

## Concurrency Constraints
- At most one active recv waiter and one active send waiter.
- Recv and send can overlap.
- Awaiters must be used on the socket's owning loop thread (no cross-thread waits).
- Cancellation (awaiter destruction) removes the waiter without resuming.
- `close()` completes any waiters with `IoErr::Canceled` and resumes them.
