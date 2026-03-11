#include "TlsTcpStream.h"

namespace fiber::net {

TlsTcpStream::TlsTcpStream(fiber::event::EventLoop &loop, int fd, SocketAddress remote_addr) :
    stream_(loop, fd), remote_addr_(std::move(remote_addr)) {}

TlsTcpStream::~TlsTcpStream() {}

common::IoResult<void> TlsTcpStream::init(SSL_CTX *ctx, bool is_server) { return stream_.init(ctx, is_server); }

bool TlsTcpStream::valid() const noexcept { return stream_.valid(); }

int TlsTcpStream::fd() const noexcept { return stream_.fd(); }

fiber::event::EventLoop &TlsTcpStream::loop() const noexcept { return stream_.loop(); }

const SocketAddress &TlsTcpStream::remote_addr() const noexcept { return remote_addr_; }

std::string TlsTcpStream::selected_alpn() const noexcept { return stream_.selected_alpn(); }

void TlsTcpStream::close() { stream_.close(); }

TlsTcpStream::ReadAwaiter TlsTcpStream::read(void *buf, size_t len) noexcept { return stream_.read(buf, len); }

TlsTcpStream::WriteAwaiter TlsTcpStream::write(const void *buf, size_t len) noexcept { return stream_.write(buf, len); }

fiber::common::IoResult<size_t> TlsTcpStream::try_read(void *buf, size_t len) noexcept {
    return stream_.try_read(buf, len);
}

fiber::common::IoResult<size_t> TlsTcpStream::try_write(const void *buf, size_t len) noexcept {
    return stream_.try_write(buf, len);
}

TlsTcpStream::HandshakeAwaiter TlsTcpStream::handshake() noexcept { return stream_.handshake(); }

TlsTcpStream::ShutdownAwaiter TlsTcpStream::shutdown() noexcept { return stream_.shutdown(); }

detail::StreamFd::WaitReadableAwaiter TlsTcpStream::wait_readable() noexcept { return stream_.wait_readable(); }

detail::StreamFd::WaitWritableAwaiter TlsTcpStream::wait_writable() noexcept { return stream_.wait_writable(); }

fiber::common::IoErr TlsTcpStream::poll_handshake(fiber::event::IoEvent &event) noexcept {
    return stream_.poll_handshake(event);
}

fiber::common::IoErr TlsTcpStream::poll_shutdown(fiber::event::IoEvent &event) noexcept {
    return stream_.poll_shutdown(event);
}

fiber::common::IoErr TlsTcpStream::poll_read(void *buf, size_t len, size_t &out,
                                             fiber::event::IoEvent &event) noexcept {
    return stream_.poll_read(buf, len, out, event);
}

fiber::common::IoErr TlsTcpStream::poll_write(const void *buf, size_t len, size_t &out,
                                              fiber::event::IoEvent &event) noexcept {
    return stream_.poll_write(buf, len, out, event);
}

} // namespace fiber::net
