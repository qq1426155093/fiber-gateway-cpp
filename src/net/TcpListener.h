#ifndef FIBER_NET_TCP_LISTENER_H
#define FIBER_NET_TCP_LISTENER_H

#include <cstdint>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../common/IoError.h"
#include "../event/EventLoop.h"
#include "detail/AcceptFd.h"
#include "SocketAddress.h"

namespace fiber::net {

struct ListenOptions {
    int backlog = SOMAXCONN;
    bool reuse_addr = true;
    bool reuse_port = false;
    bool v6_only = false;
};

struct AcceptResult {
    AcceptResult() = default;
    AcceptResult(int fd, SocketAddress peer) : fd_(fd), peer_(std::move(peer)) {}
    AcceptResult(const AcceptResult &) = delete;
    AcceptResult &operator=(const AcceptResult &) = delete;

    AcceptResult(AcceptResult &&other) noexcept : fd_(other.fd_), peer_(std::move(other.peer_)) {
        other.fd_ = -1;
    }

    AcceptResult &operator=(AcceptResult &&other) noexcept {
        if (this == &other) {
            return *this;
        }
        close_fd();
        fd_ = other.fd_;
        peer_ = std::move(other.peer_);
        other.fd_ = -1;
        return *this;
    }

    ~AcceptResult() {
        close_fd();
    }

    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    [[nodiscard]] int fd() const noexcept { return fd_; }
    int release_fd() noexcept {
        int fd = fd_;
        fd_ = -1;
        return fd;
    }
    [[nodiscard]] const SocketAddress &peer() const noexcept { return peer_; }
    SocketAddress take_peer() { return std::move(peer_); }

private:
    void close_fd() noexcept {
        if (fd_ < 0) {
            return;
        }
        int fd = fd_;
        fd_ = -1;
        ::close(fd);
    }

    int fd_ = -1;
    SocketAddress peer_{};
};

struct TcpTraits {
    using Address = SocketAddress;
    using ListenOptions = fiber::net::ListenOptions;
    using AcceptResult = fiber::net::AcceptResult;

    static fiber::common::IoResult<int> bind(const Address &addr,
                                             const ListenOptions &options);
    static fiber::common::IoErr accept_once(int fd, AcceptResult &out);
};

class TcpListener : public common::NonCopyable, public common::NonMovable {
public:
    using AcceptAwaiter = detail::AcceptFd<TcpTraits>::AcceptAwaiter;

    explicit TcpListener(fiber::event::EventLoop &loop);
    ~TcpListener();

    fiber::common::IoResult<void> bind(const SocketAddress &addr,
                                       const ListenOptions &options);
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] int fd() const noexcept;
    void close();

    [[nodiscard]] AcceptAwaiter accept() noexcept;

private:
    detail::AcceptFd<TcpTraits> acceptor_;
};

} // namespace fiber::net

#endif // FIBER_NET_TCP_LISTENER_H
