#include "UnixStream.h"

namespace fiber::net {

UnixStream::UnixStream(fiber::event::EventLoop &loop, int fd) : stream_(loop, fd) {
}

UnixStream::~UnixStream() {
}

bool UnixStream::valid() const noexcept {
    return stream_.valid();
}

int UnixStream::fd() const noexcept {
    return stream_.fd();
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

} // namespace fiber::net
