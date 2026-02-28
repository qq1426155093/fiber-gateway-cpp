#include "TcpListener.h"

#include <cerrno>
#include <netinet/in.h>
#include <unistd.h>

namespace fiber::net {

TcpListener::TcpListener(fiber::event::EventLoop &loop) : acceptor_(loop) {
}

TcpListener::~TcpListener() {
}

fiber::common::IoResult<void> TcpListener::bind(const SocketAddress &addr,
                                                const ListenOptions &options) {
    return acceptor_.bind(addr, options);
}

bool TcpListener::valid() const noexcept {
    return acceptor_.valid();
}

int TcpListener::fd() const noexcept {
    return acceptor_.fd();
}

void TcpListener::close() {
    acceptor_.close();
}

TcpListener::AcceptAwaiter TcpListener::accept() noexcept {
    return acceptor_.accept();
}

fiber::common::IoResult<int> TcpTraits::bind(const SocketAddress &addr,
                                             const ListenOptions &options) {
    sockaddr_storage storage{};
    socklen_t len = 0;
    if (!addr.to_sockaddr(storage, len)) {
        return std::unexpected(fiber::common::IoErr::NotSupported);
    }
    int domain = addr.family() == IpFamily::V4 ? AF_INET : AF_INET6;
    int fd = ::socket(domain, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
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
    if (::listen(fd, options.backlog) != 0) {
        fiber::common::IoErr err = fiber::common::io_err_from_errno(errno);
        ::close(fd);
        return std::unexpected(err);
    }
    return fd;
}

fiber::common::IoErr TcpTraits::accept_once(int fd, AcceptResult &out) {
    out = {};
    if (fd < 0) {
        return fiber::common::IoErr::BadFd;
    }
    for (;;) {
        sockaddr_storage addr{};
        socklen_t len = sizeof(addr);
        int client = ::accept4(fd, reinterpret_cast<sockaddr *>(&addr), &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client >= 0) {
            SocketAddress peer;
            if (!SocketAddress::from_sockaddr(reinterpret_cast<const sockaddr *>(&addr), len, peer)) {
                ::close(client);
                return fiber::common::IoErr::NotSupported;
            }
            out = AcceptResult(client, std::move(peer));
            return fiber::common::IoErr::None;
        }
        int err = errno;
        if (err == EINTR) {
            continue;
        }
        if (err == ECONNABORTED) {
            continue;
        }
        if (err == EAGAIN || err == EWOULDBLOCK) {
            return fiber::common::IoErr::WouldBlock;
        }
        return fiber::common::io_err_from_errno(err);
    }
}

} // namespace fiber::net
