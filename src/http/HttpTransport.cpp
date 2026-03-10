#include "HttpTransport.h"

#include <algorithm>
#include <array>
#include <sys/uio.h>

#include "../async/Timeout.h"

namespace fiber::http {

namespace {

constexpr int kMaxIov = 16;

} // namespace

common::IoResult<std::unique_ptr<TcpTransport>> TcpTransport::create(event::EventLoop &loop,
                                                                     net::AcceptResult &&accept) {
    if (!accept.valid()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return std::unique_ptr<TcpTransport>(new TcpTransport(loop, accept.release_fd(), accept.take_peer()));
}

TcpTransport::TcpTransport(event::EventLoop &loop, int fd, net::SocketAddress remote_addr) :
    stream_(loop, fd, std::move(remote_addr)) {}

fiber::async::Task<common::IoResult<void>> TcpTransport::handshake(std::chrono::milliseconds) {
    co_return common::IoResult<void>{};
}

fiber::async::Task<common::IoResult<void>> TcpTransport::shutdown(std::chrono::milliseconds) {
    co_return common::IoResult<void>{};
}

fiber::async::Task<common::IoResult<size_t>> TcpTransport::read(void *buf, size_t len,
                                                                std::chrono::milliseconds timeout) {
    auto result = co_await fiber::async::timeout_for([&]() { return stream_.read(buf, len); }, timeout);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    co_return *result;
}

fiber::async::Task<common::IoResult<size_t>> TcpTransport::read_into(mem::IoBuf &buf,
                                                                     std::chrono::milliseconds timeout) {
    size_t writable = buf.writable();
    if (writable == 0) {
        co_return static_cast<size_t>(0);
    }
    auto result =
            co_await fiber::async::timeout_for([&]() { return stream_.read(buf.writable_data(), writable); }, timeout);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    buf.commit(*result);
    co_return *result;
}

fiber::async::Task<common::IoResult<size_t>> TcpTransport::readv_into(mem::IoBufChain &bufs,
                                                                      std::chrono::milliseconds timeout) {
    std::array<iovec, kMaxIov> iov{};
    int count = bufs.fill_write_iov(iov.data(), static_cast<int>(iov.size()));
    if (count == 0) {
        co_return static_cast<size_t>(0);
    }
    auto result = co_await fiber::async::timeout_for([&]() { return stream_.readv(iov.data(), count); }, timeout);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    bufs.commit(*result);
    co_return *result;
}

fiber::async::Task<common::IoResult<size_t>> TcpTransport::write(const void *buf, size_t len,
                                                                 std::chrono::milliseconds timeout) {
    auto result = co_await fiber::async::timeout_for([&]() { return stream_.write(buf, len); }, timeout);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    co_return *result;
}

fiber::async::Task<common::IoResult<size_t>> TcpTransport::write(mem::IoBuf &buf, std::chrono::milliseconds timeout) {
    size_t readable = buf.readable();
    if (readable == 0) {
        co_return static_cast<size_t>(0);
    }
    auto result =
            co_await fiber::async::timeout_for([&]() { return stream_.write(buf.readable_data(), readable); }, timeout);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    buf.consume(*result);
    co_return *result;
}

fiber::async::Task<common::IoResult<size_t>> TcpTransport::writev(mem::IoBufChain &buf,
                                                                  std::chrono::milliseconds timeout) {
    std::array<iovec, kMaxIov> iov{};
    int count = buf.fill_read_iov(iov.data(), static_cast<int>(iov.size()));
    if (count == 0) {
        co_return static_cast<size_t>(0);
    }
    auto result = co_await fiber::async::timeout_for([&]() { return stream_.writev(iov.data(), count); }, timeout);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    buf.consume_and_compact(*result);
    co_return *result;
}

void TcpTransport::close() { stream_.close(); }

bool TcpTransport::valid() const noexcept { return stream_.valid(); }

int TcpTransport::fd() const noexcept { return stream_.fd(); }

std::string TcpTransport::negotiated_alpn() const noexcept { return {}; }

const net::SocketAddress &TcpTransport::remote_addr() const noexcept { return stream_.remote_addr(); }

common::IoResult<std::unique_ptr<TlsTransport>> TlsTransport::create(event::EventLoop &loop, net::AcceptResult &&accept,
                                                                     TlsContext &context) {
    if (!accept.valid()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    auto transport =
            std::unique_ptr<TlsTransport>(new TlsTransport(loop, accept.release_fd(), accept.take_peer(), context));
    auto init_result = transport->init();
    if (!init_result) {
        return std::unexpected(init_result.error());
    }
    return transport;
}

TlsTransport::TlsTransport(event::EventLoop &loop, int fd, net::SocketAddress remote_addr, TlsContext &context) :
    stream_(loop, fd, std::move(remote_addr)), context_(&context) {}

TlsTransport::~TlsTransport() = default;

common::IoResult<void> TlsTransport::init() {
    if (!context_ || !context_->raw()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    auto init_result = stream_.init(context_->raw(), context_->is_server());
    if (!init_result) {
        return std::unexpected(init_result.error());
    }
    return {};
}

fiber::async::Task<common::IoResult<void>> TlsTransport::handshake(std::chrono::milliseconds timeout) {
    auto result = co_await fiber::async::timeout_for([&]() { return stream_.handshake(); }, timeout);
    co_return result;
}

fiber::async::Task<common::IoResult<void>> TlsTransport::shutdown(std::chrono::milliseconds timeout) {
    auto result = co_await fiber::async::timeout_for([&]() { return stream_.shutdown(); }, timeout);
    co_return result;
}

fiber::async::Task<common::IoResult<size_t>> TlsTransport::read(void *buf, size_t len,
                                                                std::chrono::milliseconds timeout) {
    auto hs_result = co_await handshake(context_->options().handshake_timeout);
    if (!hs_result) {
        co_return std::unexpected(hs_result.error());
    }
    auto result = co_await fiber::async::timeout_for([&]() { return stream_.read(buf, len); }, timeout);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    co_return *result;
}

fiber::async::Task<common::IoResult<size_t>> TlsTransport::read_into(mem::IoBuf &buf,
                                                                     std::chrono::milliseconds timeout) {
    size_t writable = buf.writable();
    if (writable == 0) {
        co_return static_cast<size_t>(0);
    }
    auto hs_result = co_await handshake(context_->options().handshake_timeout);
    if (!hs_result) {
        co_return std::unexpected(hs_result.error());
    }
    auto result =
            co_await fiber::async::timeout_for([&]() { return stream_.read(buf.writable_data(), writable); }, timeout);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    buf.commit(*result);
    co_return *result;
}

fiber::async::Task<common::IoResult<size_t>> TlsTransport::readv_into(mem::IoBufChain &bufs,
                                                                      std::chrono::milliseconds timeout) {
    mem::IoBuf *target = bufs.first_writable();
    if (!target) {
        co_return static_cast<size_t>(0);
    }
    co_return co_await read_into(*target, timeout);
}

fiber::async::Task<common::IoResult<size_t>> TlsTransport::write(const void *buf, size_t len,
                                                                 std::chrono::milliseconds timeout) {
    auto hs_result = co_await handshake(context_->options().handshake_timeout);
    if (!hs_result) {
        co_return std::unexpected(hs_result.error());
    }
    auto result = co_await fiber::async::timeout_for([&]() { return stream_.write(buf, len); }, timeout);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    co_return *result;
}

fiber::async::Task<common::IoResult<size_t>> TlsTransport::write(mem::IoBuf &buf, std::chrono::milliseconds timeout) {
    size_t readable = buf.readable();
    if (readable == 0) {
        co_return static_cast<size_t>(0);
    }
    auto hs_result = co_await handshake(context_->options().handshake_timeout);
    if (!hs_result) {
        co_return std::unexpected(hs_result.error());
    }
    auto result =
            co_await fiber::async::timeout_for([&]() { return stream_.write(buf.readable_data(), readable); }, timeout);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    buf.consume(*result);
    co_return *result;
}

fiber::async::Task<common::IoResult<size_t>> TlsTransport::writev(mem::IoBufChain &buf,
                                                                  std::chrono::milliseconds timeout) {
    buf.drop_empty_front();
    mem::IoBuf *target = buf.first_readable();
    if (!target) {
        co_return static_cast<size_t>(0);
    }
    co_return co_await write(*target, timeout);
}

void TlsTransport::close() { stream_.close(); }

bool TlsTransport::valid() const noexcept { return stream_.valid(); }

int TlsTransport::fd() const noexcept { return stream_.fd(); }

std::string TlsTransport::negotiated_alpn() const noexcept { return stream_.selected_alpn(); }

const net::SocketAddress &TlsTransport::remote_addr() const noexcept { return stream_.remote_addr(); }

} // namespace fiber::http
