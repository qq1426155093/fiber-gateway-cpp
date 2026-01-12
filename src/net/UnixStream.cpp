#include "UnixStream.h"

namespace fiber::net {

UnixStream::UnixStream(fiber::event::EventLoop &loop, int fd) : stream_(loop, fd) {
}

UnixStream::UnixStream(fiber::event::EventLoop &loop, int fd, UnixAddress peer)
    : stream_(loop, fd), remote_addr_(std::move(peer)) {
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

} // namespace fiber::net
