#include "TcpStream.h"

namespace fiber::net {

TcpStream::TcpStream(fiber::event::EventLoop &loop, int fd) : stream_(loop, fd) {
}

TcpStream::TcpStream(fiber::event::EventLoop &loop, int fd, SocketAddress peer)
    : stream_(loop, fd), remote_addr_(std::move(peer)) {
}

TcpStream::~TcpStream() {
}

bool TcpStream::valid() const noexcept {
    return stream_.valid();
}

int TcpStream::fd() const noexcept {
    return stream_.fd();
}

const SocketAddress &TcpStream::remote_addr() const noexcept {
    return remote_addr_;
}

void TcpStream::close() {
    stream_.close();
}

TcpStream::ReadAwaiter TcpStream::read(void *buf, size_t len) noexcept {
    return stream_.read(buf, len);
}

TcpStream::WriteAwaiter TcpStream::write(const void *buf, size_t len) noexcept {
    return stream_.write(buf, len);
}

TcpStream::ReadvAwaiter TcpStream::readv(const struct iovec *iov, int iovcnt) noexcept {
    return stream_.readv(iov, iovcnt);
}

TcpStream::WritevAwaiter TcpStream::writev(const struct iovec *iov, int iovcnt) noexcept {
    return stream_.writev(iov, iovcnt);
}

} // namespace fiber::net
