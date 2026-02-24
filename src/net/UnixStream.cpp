#include "UnixStream.h"

#include <cerrno>
#include <sys/socket.h>

namespace fiber::net {

UnixStream::ConnectAwaiter UnixStream::connect(fiber::event::EventLoop &loop, const UnixAddress &peer) noexcept {
    return detail::ConnectFd<UnixConnectTraits>::connect(loop, peer);
}

fiber::common::IoResult<int> UnixConnectTraits::create_socket(const UnixAddress &peer) {
    if (peer.kind() == UnixAddressKind::Unnamed) {
        return std::unexpected(fiber::common::IoErr::Invalid);
    }
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return std::unexpected(fiber::common::io_err_from_errno(errno));
    }
    return fd;
}

fiber::common::IoErr UnixConnectTraits::connect_once(int fd, const UnixAddress &peer) {
    sockaddr_storage storage{};
    socklen_t len = 0;
    if (!peer.to_sockaddr(storage, len)) {
        return fiber::common::IoErr::NotSupported;
    }
    for (;;) {
        int rc = ::connect(fd, reinterpret_cast<const sockaddr *>(&storage), len);
        if (rc == 0) {
            return fiber::common::IoErr::None;
        }
        int err = errno;
        if (err == EINTR) {
            continue;
        }
        if (err == EINPROGRESS || err == EALREADY) {
            return fiber::common::IoErr::WouldBlock;
        }
        return fiber::common::io_err_from_errno(err);
    }
}

UnixStream::UnixStream(fiber::event::EventLoop &loop, int fd) : stream_(loop, fd) {
}

UnixStream::UnixStream(fiber::event::EventLoop &loop, int fd, UnixAddress peer)
    : stream_(loop, fd), remote_addr_(std::move(peer)) {
}

UnixStream::UnixStream(ConnectInfant &&infant)
    : stream_(infant.loop(), infant.release_fd()), remote_addr_(infant.take_peer()) {
}

UnixStream::~UnixStream() {
}

bool UnixStream::valid() const noexcept {
    return stream_.valid();
}

int UnixStream::fd() const noexcept {
    return stream_.fd();
}

const UnixAddress &UnixStream::remote_addr() const noexcept {
    return remote_addr_;
}

void UnixStream::close() {
    stream_.close();
}

UnixStream::ReadAwaiter UnixStream::read(void *buf, size_t len) noexcept {
    return stream_.read(buf, len);
}

UnixStream::WriteAwaiter UnixStream::write(const void *buf, size_t len) noexcept {
    return stream_.write(buf, len);
}

UnixStream::ReadvAwaiter UnixStream::readv(const struct iovec *iov, int iovcnt) noexcept {
    return stream_.readv(iov, iovcnt);
}

UnixStream::WritevAwaiter UnixStream::writev(const struct iovec *iov, int iovcnt) noexcept {
    return stream_.writev(iov, iovcnt);
}

fiber::common::IoResult<size_t> UnixStream::try_read(void *buf, size_t len) noexcept {
    return stream_.try_read(buf, len);
}

fiber::common::IoResult<size_t> UnixStream::try_write(const void *buf, size_t len) noexcept {
    return stream_.try_write(buf, len);
}

fiber::common::IoResult<size_t> UnixStream::try_readv(const struct iovec *iov, int iovcnt) noexcept {
    return stream_.try_readv(iov, iovcnt);
}

fiber::common::IoResult<size_t> UnixStream::try_writev(const struct iovec *iov, int iovcnt) noexcept {
    return stream_.try_writev(iov, iovcnt);
}

} // namespace fiber::net
