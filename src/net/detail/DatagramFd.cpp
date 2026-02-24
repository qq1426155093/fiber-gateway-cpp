#include "DatagramFd.h"

#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../UdpSocket.h"

namespace fiber::net::detail {

DatagramFd::DatagramFd(fiber::event::EventLoop &loop) : rwfd_(loop) {}

DatagramFd::~DatagramFd() {
    if (!rwfd_.valid()) {
        return;
    }
    if (rwfd_.loop().in_loop()) {
        close();
        return;
    }
    FIBER_ASSERT(false);
}

fiber::common::IoResult<void> DatagramFd::bind(const SocketAddress &addr,
                                               const UdpBindOptions &options) {
    if (rwfd_.valid()) {
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
    fiber::common::IoErr attach_err = rwfd_.attach(fd);
    if (attach_err != fiber::common::IoErr::None) {
        ::close(fd);
        return std::unexpected(attach_err);
    }
    return {};
}

bool DatagramFd::valid() const noexcept {
    return rwfd_.valid();
}

int DatagramFd::fd() const noexcept {
    return rwfd_.fd();
}

void DatagramFd::close() {
    rwfd_.close();
}

DatagramFd::RecvFromAwaiter DatagramFd::recv_from(void *buf, size_t len) noexcept {
    return {*this, buf, len};
}

DatagramFd::SendToAwaiter DatagramFd::send_to(const void *buf, size_t len, const SocketAddress &peer) noexcept {
    return {*this, buf, len, peer};
}

fiber::common::IoResult<UdpRecvResult> DatagramFd::try_recv_from(void *buf, size_t len) noexcept {
    SocketAddress peer{};
    size_t out = 0;
    fiber::common::IoErr err = recv_from_once(buf, len, peer, out);
    if (err == fiber::common::IoErr::None) {
        return UdpRecvResult{out, peer};
    }
    return std::unexpected(err);
}

fiber::common::IoResult<size_t> DatagramFd::try_send_to(const void *buf,
                                                        size_t len,
                                                        const SocketAddress &peer) noexcept {
    size_t out = 0;
    fiber::common::IoErr err = send_to_once(buf, len, peer, out);
    if (err == fiber::common::IoErr::None) {
        return out;
    }
    return std::unexpected(err);
}

fiber::common::IoErr DatagramFd::recv_from_once(void *buf,
                                                size_t len,
                                                SocketAddress &peer,
                                                size_t &out) {
    out = 0;
    int socket_fd = rwfd_.fd();
    if (socket_fd < 0) {
        return fiber::common::IoErr::BadFd;
    }
    for (;;) {
        sockaddr_storage addr{};
        socklen_t addr_len = sizeof(addr);
        ssize_t rc = ::recvfrom(socket_fd, buf, len, 0, reinterpret_cast<sockaddr *>(&addr), &addr_len);
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
    int socket_fd = rwfd_.fd();
    if (socket_fd < 0) {
        return fiber::common::IoErr::BadFd;
    }
    sockaddr_storage storage{};
    socklen_t addr_len = 0;
    if (!peer.to_sockaddr(storage, addr_len)) {
        return fiber::common::IoErr::NotSupported;
    }
    for (;;) {
        ssize_t rc = ::sendto(socket_fd, buf, len, 0,
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

DatagramFd::RecvFromAwaiter::RecvFromAwaiter(DatagramFd &socket, void *buf, size_t len) noexcept
    : socket_(&socket), buf_(buf), len_(len) {}

DatagramFd::RecvFromAwaiter::~RecvFromAwaiter() {
}

bool DatagramFd::RecvFromAwaiter::await_suspend(std::coroutine_handle<> handle) {
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
    waiter_.emplace(socket_->rwfd_);
    return waiter_->await_suspend(handle);
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

    if (waiter_) {
        fiber::common::IoResult<void> wait_result = waiter_->await_resume();
        waiter_.reset();
        if (!wait_result) {
            return std::unexpected(wait_result.error());
        }
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
    : socket_(&socket), buf_(buf), len_(len), peer_(std::move(peer)) {}

DatagramFd::SendToAwaiter::~SendToAwaiter() {
}

bool DatagramFd::SendToAwaiter::await_suspend(std::coroutine_handle<> handle) {
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
    waiter_.emplace(socket_->rwfd_);
    return waiter_->await_suspend(handle);
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

    if (waiter_) {
        fiber::common::IoResult<void> wait_result = waiter_->await_resume();
        waiter_.reset();
        if (!wait_result) {
            return std::unexpected(wait_result.error());
        }
    }

    size_t out = 0;
    fiber::common::IoErr err = socket_->send_to_once(buf_, len_, peer_, out);
    if (err == fiber::common::IoErr::None) {
        return out;
    }
    return std::unexpected(err);
}

} // namespace fiber::net::detail
