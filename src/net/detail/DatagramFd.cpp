#include "DatagramFd.h"

#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../UdpSocket.h"

namespace fiber::net::detail {

DatagramFd::DatagramFd(fiber::event::EventLoop &loop) : loop_(loop) {
    item_.socket = this;
    item_.callback = &DatagramFd::on_events;
}

DatagramFd::~DatagramFd() {
    if (fd_ < 0) {
        return;
    }
    if (loop_.in_loop()) {
        close();
        return;
    }
    FIBER_ASSERT(false);
}

fiber::common::IoResult<void> DatagramFd::bind(const SocketAddress &addr,
                                               const UdpBindOptions &options) {
    if (fd_ >= 0) {
        return std::unexpected(fiber::common::IoErr::Already);
    }
    sockaddr_storage storage{};
    socklen_t len = 0;
    if (!addr.to_sockaddr(storage, len)) {
        return std::unexpected(fiber::common::IoErr::NotSupported);
    }
    int domain = addr.family() == IpFamily::V4 ? AF_INET : AF_INET6;
    int fd = ::socket(domain, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return std::unexpected(fiber::common::io_err_from_errno(errno));
    }
    if (options.reuse_addr) {
        int reuse = 1;
        if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
            fiber::common::IoErr err = fiber::common::io_err_from_errno(errno);
            ::close(fd);
            return std::unexpected(err);
        }
    }
    if (domain == AF_INET6 && options.v6_only) {
        int v6_only = 1;
        if (::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6_only, sizeof(v6_only)) != 0) {
            fiber::common::IoErr err = fiber::common::io_err_from_errno(errno);
            ::close(fd);
            return std::unexpected(err);
        }
    }
#ifdef SO_REUSEPORT
    if (options.reuse_port) {
        int reuse = 1;
        if (::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse)) != 0) {
            fiber::common::IoErr err = fiber::common::io_err_from_errno(errno);
            ::close(fd);
            return std::unexpected(err);
        }
    }
#else
    if (options.reuse_port) {
        ::close(fd);
        return std::unexpected(fiber::common::IoErr::NotSupported);
    }
#endif
    if (::bind(fd, reinterpret_cast<const sockaddr *>(&storage), len) != 0) {
        fiber::common::IoErr err = fiber::common::io_err_from_errno(errno);
        ::close(fd);
        return std::unexpected(err);
    }
    fd_ = fd;
    return {};
}

bool DatagramFd::valid() const noexcept {
    return fd_ >= 0;
}

int DatagramFd::fd() const noexcept {
    return fd_;
}

void DatagramFd::close() {
    FIBER_ASSERT(loop_.in_loop());
    if (fd_ < 0) {
        return;
    }
    if (registered_) {
        loop_.poller().del(fd_);
        registered_ = false;
        watching_ = fiber::event::IoEvent::None;
    }

    if (read_waiter_) {
        LocalThreadWaiter *waiter = read_waiter_;
        read_waiter_ = nullptr;
        waiter->err_ = fiber::common::IoErr::Canceled;
        waiter->coro_.resume();
    }

    if (write_waiter_) {
        LocalThreadWaiter *waiter = write_waiter_;
        write_waiter_ = nullptr;
        waiter->err_ = fiber::common::IoErr::Canceled;
        waiter->coro_.resume();
    }

    int fd = fd_;
    fd_ = kInvalidFd;
    ::close(fd);
}

DatagramFd::RecvFromAwaiter DatagramFd::recv_from(void *buf, size_t len) noexcept {
    return {*this, buf, len};
}

DatagramFd::SendToAwaiter DatagramFd::send_to(const void *buf, size_t len, const SocketAddress &peer) noexcept {
    return {*this, buf, len, peer};
}

fiber::common::IoErr DatagramFd::recv_from_once(void *buf,
                                                size_t len,
                                                SocketAddress &peer,
                                                size_t &out) {
    out = 0;
    if (fd_ < 0) {
        return fiber::common::IoErr::BadFd;
    }
    for (;;) {
        sockaddr_storage addr{};
        socklen_t addr_len = sizeof(addr);
        ssize_t rc = ::recvfrom(fd_, buf, len, 0, reinterpret_cast<sockaddr *>(&addr), &addr_len);
        if (rc >= 0) {
            out = static_cast<size_t>(rc);
            SocketAddress parsed;
            if (!SocketAddress::from_sockaddr(reinterpret_cast<const sockaddr *>(&addr), addr_len, parsed)) {
                return fiber::common::IoErr::NotSupported;
            }
            peer = parsed;
            return fiber::common::IoErr::None;
        }
        int err = errno;
        if (err == EINTR) {
            continue;
        }
        if (err == EAGAIN || err == EWOULDBLOCK) {
            return fiber::common::IoErr::WouldBlock;
        }
        return fiber::common::io_err_from_errno(err);
    }
}

fiber::common::IoErr DatagramFd::send_to_once(const void *buf,
                                              size_t len,
                                              const SocketAddress &peer,
                                              size_t &out) {
    out = 0;
    if (fd_ < 0) {
        return fiber::common::IoErr::BadFd;
    }
    sockaddr_storage storage{};
    socklen_t addr_len = 0;
    if (!peer.to_sockaddr(storage, addr_len)) {
        return fiber::common::IoErr::NotSupported;
    }
    for (;;) {
        ssize_t rc = ::sendto(fd_, buf, len, 0,
                              reinterpret_cast<const sockaddr *>(&storage), addr_len);
        if (rc >= 0) {
            out = static_cast<size_t>(rc);
            return fiber::common::IoErr::None;
        }
        int err = errno;
        if (err == EINTR) {
            continue;
        }
        if (err == EAGAIN || err == EWOULDBLOCK) {
            return fiber::common::IoErr::WouldBlock;
        }
        return fiber::common::io_err_from_errno(err);
    }
}

void DatagramFd::handle_events(fiber::event::IoEvent events) {
    FIBER_ASSERT(loop_.in_loop());
    if (!fiber::event::any(events)) {
        return;
    }
    fiber::event::IoEvent desired = watching_ & ~events;
    if (desired != fiber::event::IoEvent::None) {
        loop_.poller().mod(fd_, desired, &item_, fiber::event::Poller::Mode::OneShot);
    }
    watching_ = desired;

    if (fiber::event::any(events & fiber::event::IoEvent::Read)) {
        LocalThreadWaiter *waiter = read_waiter_;
        FIBER_ASSERT(waiter);
        read_waiter_ = nullptr;
        waiter->coro_.resume();
    }
    if (fiber::event::any(events & fiber::event::IoEvent::Write)) {
        LocalThreadWaiter *waiter = write_waiter_;
        FIBER_ASSERT(waiter);
        write_waiter_ = nullptr;
        waiter->coro_.resume();
    }
}

void DatagramFd::on_events(fiber::event::Poller::Item *item,
                           int fd,
                           fiber::event::IoEvent events) {
    (void) fd;
    auto *socket_item = static_cast<DatagramItem *>(item);
    if (!socket_item->socket) {
        return;
    }
    socket_item->socket->handle_events(events);
}

DatagramFd::RecvFromAwaiter::RecvFromAwaiter(DatagramFd &socket, void *buf, size_t len) noexcept
    : socket_(&socket), buf_(buf), len_(len) {
    event_ = fiber::event::IoEvent::Read;
}

DatagramFd::RecvFromAwaiter::~RecvFromAwaiter() {
    if (!waiting_) {
        return;
    }
    FIBER_ASSERT(socket_->loop_.in_loop());
    socket_->cancel_event<fiber::event::IoEvent::Read>(this);
}

bool DatagramFd::RecvFromAwaiter::await_suspend(std::coroutine_handle<> handle) {
    FIBER_ASSERT(socket_->loop_.in_loop());
    coro_ = handle;
    err_ = fiber::common::IoErr::None;
    completed_ = false;

    size_t out = 0;
    fiber::common::IoErr err = socket_->recv_from_once(buf_, len_, peer_, out);
    if (err == fiber::common::IoErr::None) {
        result_ = out;
        completed_ = true;
        return false;
    }
    if (err != fiber::common::IoErr::WouldBlock) {
        err_ = err;
        completed_ = true;
        return false;
    }

    waiting_ = true;
    fiber::common::IoErr watch_err = socket_->begin_event<fiber::event::IoEvent::Read>(this);
    if (watch_err != fiber::common::IoErr::None) {
        err_ = watch_err;
        completed_ = true;
        waiting_ = false;
        return false;
    }
    return true;
}

fiber::common::IoResult<UdpRecvResult> DatagramFd::RecvFromAwaiter::await_resume() noexcept {
    waiting_ = false;
    if (completed_) {
        completed_ = false;
        if (err_ == fiber::common::IoErr::None) {
            return UdpRecvResult{result_, peer_};
        }
        return std::unexpected(err_);
    }

    if (err_ != fiber::common::IoErr::None) {
        return std::unexpected(err_);
    }

    size_t out = 0;
    fiber::common::IoErr err = socket_->recv_from_once(buf_, len_, peer_, out);
    if (err == fiber::common::IoErr::None) {
        return UdpRecvResult{out, peer_};
    }
    return std::unexpected(err);
}

DatagramFd::SendToAwaiter::SendToAwaiter(DatagramFd &socket,
                                         const void *buf,
                                         size_t len,
                                         SocketAddress peer) noexcept
    : socket_(&socket), buf_(buf), len_(len), peer_(std::move(peer)) {
    event_ = fiber::event::IoEvent::Write;
}

DatagramFd::SendToAwaiter::~SendToAwaiter() {
    if (!waiting_) {
        return;
    }
    FIBER_ASSERT(socket_->loop_.in_loop());
    socket_->cancel_event<fiber::event::IoEvent::Write>(this);
}

bool DatagramFd::SendToAwaiter::await_suspend(std::coroutine_handle<> handle) {
    FIBER_ASSERT(socket_->loop_.in_loop());
    coro_ = handle;
    err_ = fiber::common::IoErr::None;
    completed_ = false;

    size_t out = 0;
    fiber::common::IoErr err = socket_->send_to_once(buf_, len_, peer_, out);
    if (err == fiber::common::IoErr::None) {
        result_ = out;
        completed_ = true;
        return false;
    }
    if (err != fiber::common::IoErr::WouldBlock) {
        err_ = err;
        completed_ = true;
        return false;
    }

    waiting_ = true;
    fiber::common::IoErr watch_err = socket_->begin_event<fiber::event::IoEvent::Write>(this);
    if (watch_err != fiber::common::IoErr::None) {
        err_ = watch_err;
        completed_ = true;
        waiting_ = false;
        return false;
    }
    return true;
}

fiber::common::IoResult<size_t> DatagramFd::SendToAwaiter::await_resume() noexcept {
    waiting_ = false;
    if (completed_) {
        completed_ = false;
        if (err_ == fiber::common::IoErr::None) {
            return result_;
        }
        return std::unexpected(err_);
    }

    if (err_ != fiber::common::IoErr::None) {
        return std::unexpected(err_);
    }

    size_t out = 0;
    fiber::common::IoErr err = socket_->send_to_once(buf_, len_, peer_, out);
    if (err == fiber::common::IoErr::None) {
        return out;
    }
    return std::unexpected(err);
}

} // namespace fiber::net::detail
