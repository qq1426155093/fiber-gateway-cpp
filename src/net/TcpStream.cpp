#include "TcpStream.h"

#include <cerrno>
#include <sys/socket.h>

namespace fiber::net {

TcpStream::ConnectAwaiter TcpStream::connect(fiber::event::EventLoop &loop, const SocketAddress &peer) noexcept {
    return detail::ConnectFd<TcpConnectTraits>::connect(loop, peer);
}

fiber::common::IoResult<int> TcpConnectTraits::create_socket(const SocketAddress &peer) {
    int domain = peer.family() == IpFamily::V4 ? AF_INET : AF_INET6;
    int fd = ::socket(domain, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return std::unexpected(fiber::common::io_err_from_errno(errno));
    }
    return fd;
}

fiber::common::IoErr TcpConnectTraits::connect_once(int fd, const SocketAddress &peer) {
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

TcpStream::TcpStream(fiber::event::EventLoop &loop, int fd) : stream_(loop, fd) {}

TcpStream::TcpStream(fiber::event::EventLoop &loop, int fd, SocketAddress peer) :
    stream_(loop, fd), remote_addr_(std::move(peer)) {}

TcpStream::TcpStream(ConnectInfant &&infant) :
    stream_(infant.loop(), infant.release_fd()), remote_addr_(infant.take_peer()) {}

TcpStream::~TcpStream() {}

bool TcpStream::valid() const noexcept { return stream_.valid(); }

int TcpStream::fd() const noexcept { return stream_.fd(); }

fiber::event::EventLoop &TcpStream::loop() const noexcept { return stream_.loop(); }

const SocketAddress &TcpStream::remote_addr() const noexcept { return remote_addr_; }

int TcpStream::release_fd() noexcept { return stream_.release_fd(); }

void TcpStream::close() { stream_.close(); }

TcpStream::ReadAwaiter TcpStream::read(void *buf, size_t len) noexcept { return stream_.read(buf, len); }

TcpStream::WriteAwaiter TcpStream::write(const void *buf, size_t len) noexcept { return stream_.write(buf, len); }

TcpStream::ReadvAwaiter TcpStream::readv(const struct iovec *iov, int iovcnt) noexcept {
    return stream_.readv(iov, iovcnt);
}

TcpStream::WritevAwaiter TcpStream::writev(const struct iovec *iov, int iovcnt) noexcept {
    return stream_.writev(iov, iovcnt);
}

fiber::common::IoResult<size_t> TcpStream::try_read(void *buf, size_t len) noexcept {
    return stream_.try_read(buf, len);
}

fiber::common::IoResult<size_t> TcpStream::try_write(const void *buf, size_t len) noexcept {
    return stream_.try_write(buf, len);
}

fiber::common::IoResult<size_t> TcpStream::try_readv(const struct iovec *iov, int iovcnt) noexcept {
    return stream_.try_readv(iov, iovcnt);
}

fiber::common::IoResult<size_t> TcpStream::try_writev(const struct iovec *iov, int iovcnt) noexcept {
    return stream_.try_writev(iov, iovcnt);
}

} // namespace fiber::net
