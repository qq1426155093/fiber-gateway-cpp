#include "TcpStream.h"

namespace fiber::net {

TcpStream::TcpStream(fiber::event::EventLoop &loop, int fd) : stream_(loop, fd) {
}

TcpStream::~TcpStream() {
}

bool TcpStream::valid() const noexcept {
    return stream_.valid();
}

int TcpStream::fd() const noexcept {
    return stream_.fd();
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

} // namespace fiber::net
