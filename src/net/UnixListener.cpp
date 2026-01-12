#include "UnixListener.h"

#include <cerrno>
#include <sys/un.h>
#include <unistd.h>

namespace fiber::net {

UnixListener::UnixListener(fiber::event::EventLoop &loop) : acceptor_(loop) {
}

UnixListener::~UnixListener() {
}

fiber::common::IoResult<void> UnixListener::bind(const UnixAddress &addr,
                                                 const UnixListenOptions &options) {
    return acceptor_.bind(addr, options);
}

bool UnixListener::valid() const noexcept {
    return acceptor_.valid();
}

int UnixListener::fd() const noexcept {
    return acceptor_.fd();
}

void UnixListener::close() {
    acceptor_.close();
}

UnixListener::AcceptAwaiter UnixListener::accept() noexcept {
    return acceptor_.accept();
}

fiber::common::IoResult<int> UnixTraits::bind(const UnixAddress &addr,
                                              const UnixListenOptions &options) {
    if (addr.kind() == UnixAddressKind::Unnamed) {
        return std::unexpected(fiber::common::IoErr::NotSupported);
    }
    if (addr.kind() == UnixAddressKind::Filesystem && options.unlink_existing) {
        if (!addr.path().empty()) {
            if (::unlink(addr.path().c_str()) != 0 && errno != ENOENT) {
                return std::unexpected(fiber::common::io_err_from_errno(errno));
            }
        }
    }
    sockaddr_storage storage{};
    socklen_t len = 0;
    if (!addr.to_sockaddr(storage, len)) {
        return std::unexpected(fiber::common::IoErr::NotSupported);
    }
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return std::unexpected(fiber::common::io_err_from_errno(errno));
    }
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

fiber::common::IoErr UnixTraits::accept_once(int fd, UnixAcceptResult &out) {
    out = {};
    out.fd = -1;
    if (fd < 0) {
        return fiber::common::IoErr::BadFd;
    }
    for (;;) {
        sockaddr_storage addr{};
        socklen_t len = sizeof(addr);
        int client = ::accept4(fd, reinterpret_cast<sockaddr *>(&addr), &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client >= 0) {
            out.fd = client;
            UnixAddress peer = UnixAddress::unnamed();
            if (!UnixAddress::from_sockaddr(reinterpret_cast<const sockaddr *>(&addr), len, peer)) {
                ::close(client);
                out.fd = -1;
                return fiber::common::IoErr::NotSupported;
            }
            out.peer = peer;
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
