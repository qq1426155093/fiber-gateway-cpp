#ifndef FIBER_NET_DETAIL_CONNECT_FD_H
#define FIBER_NET_DETAIL_CONNECT_FD_H

#include <cerrno>
#include <coroutine>
#include <expected>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

#include "../../common/Assert.h"
#include "../../common/IoError.h"
#include "../../event/EventLoop.h"

namespace fiber::net::detail {

template <typename Traits>
class StreamInfant {
public:
    using Address = typename Traits::Address;

    StreamInfant() = delete;
    StreamInfant(fiber::event::EventLoop *loop, int fd, Address peer)
        : loop_(loop), fd_(fd), peer_(std::move(peer)) {
    }

    StreamInfant(const StreamInfant &) = delete;
    StreamInfant &operator=(const StreamInfant &) = delete;

    StreamInfant(StreamInfant &&other) noexcept
        : loop_(other.loop_), fd_(other.fd_), peer_(std::move(other.peer_)) {
        other.loop_ = nullptr;
        other.fd_ = -1;
    }

    StreamInfant &operator=(StreamInfant &&other) noexcept {
        if (this == &other) {
            return *this;
        }
        close_fd();
        loop_ = other.loop_;
        fd_ = other.fd_;
        peer_ = std::move(other.peer_);
        other.loop_ = nullptr;
        other.fd_ = -1;
        return *this;
    }

    ~StreamInfant() {
        close_fd();
    }

    [[nodiscard]] bool valid() const noexcept {
        return fd_ >= 0;
    }

    fiber::event::EventLoop &loop() const noexcept {
        FIBER_ASSERT(loop_ != nullptr);
        return *loop_;
    }

    int release_fd() noexcept {
        int fd = fd_;
        fd_ = -1;
        return fd;
    }

    Address take_peer() {
        return std::move(peer_);
    }

    const Address &peer() const noexcept {
        return peer_;
    }

private:
    void close_fd() noexcept {
        if (fd_ < 0) {
            return;
        }
        ::close(fd_);
        fd_ = -1;
    }

    fiber::event::EventLoop *loop_ = nullptr;
    int fd_ = -1;
    Address peer_;
};

template <typename Traits>
class ConnectFd {
public:
    using Address = typename Traits::Address;
    using ConnectResult = StreamInfant<Traits>;

    class ConnectAwaiter;

    [[nodiscard]] static ConnectAwaiter connect(fiber::event::EventLoop &loop, Address peer) noexcept {
        return ConnectAwaiter(loop, std::move(peer));
    }
};

template <typename Traits>
class ConnectFd<Traits>::ConnectAwaiter {
public:
    ConnectAwaiter(fiber::event::EventLoop &loop, Address peer) noexcept
        : loop_(&loop), peer_(std::move(peer)) {
        item_.awaiter = this;
        item_.callback = &ConnectAwaiter::on_connected;
    }

    ConnectAwaiter(const ConnectAwaiter &) = delete;
    ConnectAwaiter &operator=(const ConnectAwaiter &) = delete;
    ConnectAwaiter(ConnectAwaiter &&) = delete;
    ConnectAwaiter &operator=(ConnectAwaiter &&) = delete;

    ~ConnectAwaiter() {
        if (!loop_ || !waiting_) {
            close_fd();
            return;
        }
        FIBER_ASSERT(loop_->in_loop());
        cancel_wait();
    }

    bool await_ready() noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle) {
        if (!loop_) {
            result_ = std::unexpected(fiber::common::IoErr::Invalid);
            return false;
        }
        FIBER_ASSERT(loop_->in_loop());
        handle_ = handle;
        result_ = std::unexpected(fiber::common::IoErr::Unknown);

        auto fd_result = Traits::create_socket(peer_);
        if (!fd_result) {
            result_ = std::unexpected(fd_result.error());
            return false;
        }
        fd_ = *fd_result;

        fiber::common::IoErr err = Traits::connect_once(fd_, peer_);
        if (err == fiber::common::IoErr::None) {
            result_ = ConnectResult(loop_, fd_, std::move(peer_));
            fd_ = -1;
            return false;
        }
        if (err != fiber::common::IoErr::WouldBlock) {
            result_ = std::unexpected(err);
            close_fd();
            return false;
        }

        if (loop_->poller().add(fd_, fiber::event::IoEvent::Write, &item_) != 0) {
            result_ = std::unexpected(fiber::common::io_err_from_errno(errno));
            close_fd();
            return false;
        }
        watching_ = true;
        waiting_ = true;
        return true;
    }

    fiber::common::IoResult<ConnectResult> await_resume() noexcept {
        return std::move(result_);
    }

private:
    struct ConnectItem : fiber::event::Poller::Item {
        ConnectAwaiter *awaiter = nullptr;
    };

    void close_fd() noexcept {
        if (fd_ < 0) {
            return;
        }
        ::close(fd_);
        fd_ = -1;
    }

    fiber::common::IoErr finish_connect() noexcept {
        if (fd_ < 0) {
            return fiber::common::IoErr::BadFd;
        }
        int socket_err = 0;
        socklen_t len = sizeof(socket_err);
        for (;;) {
            if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &socket_err, &len) == 0) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            return fiber::common::io_err_from_errno(errno);
        }
        if (socket_err == 0) {
            return fiber::common::IoErr::None;
        }
        return fiber::common::io_err_from_errno(socket_err);
    }

    void cancel_wait() {
        if (!waiting_) {
            return;
        }
        waiting_ = false;
        if (watching_) {
            loop_->poller().del(fd_);
            watching_ = false;
        }
        handle_ = {};
        close_fd();
    }

    void handle_connected(fiber::event::IoEvent events) {
        FIBER_ASSERT(loop_ && loop_->in_loop());
        if (!waiting_) {
            return;
        }
        if (!fiber::event::any(events & fiber::event::IoEvent::Write)) {
            return;
        }
        waiting_ = false;
        if (watching_) {
            loop_->poller().del(fd_);
            watching_ = false;
        }

        fiber::common::IoErr err = finish_connect();
        if (err == fiber::common::IoErr::None) {
            result_ = ConnectResult(loop_, fd_, std::move(peer_));
            fd_ = -1;
        }
        if (err != fiber::common::IoErr::None) {
            result_ = std::unexpected(err);
            close_fd();
        }

        auto handle = handle_;
        handle_ = {};
        if (handle) {
            handle.resume();
        }
    }

    static void on_connected(fiber::event::Poller::Item *item,
                             int fd,
                             fiber::event::IoEvent events) {
        (void) fd;
        auto *entry = static_cast<ConnectItem *>(item);
        if (!entry || !entry->awaiter) {
            return;
        }
        entry->awaiter->handle_connected(events);
    }

    fiber::event::EventLoop *loop_ = nullptr;
    Address peer_{};
    std::coroutine_handle<> handle_{};
    fiber::common::IoResult<ConnectResult> result_{std::unexpected(fiber::common::IoErr::Unknown)};
    int fd_ = -1;
    bool watching_ = false;
    bool waiting_ = false;
    ConnectItem item_{};
};

} // namespace fiber::net::detail

#endif // FIBER_NET_DETAIL_CONNECT_FD_H
