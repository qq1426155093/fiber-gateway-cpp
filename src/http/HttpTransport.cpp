#include "HttpTransport.h"

#include "../async/Timeout.h"
#include "../net/detail/TlsStreamFd.h"

namespace fiber::http {

TcpTransport::TcpTransport(std::unique_ptr<net::TcpStream> stream) : stream_(std::move(stream)) {
}

fiber::async::Task<common::IoResult<void>> TcpTransport::handshake(std::chrono::seconds) {
    co_return common::IoResult<void>{};
}

fiber::async::Task<common::IoResult<void>> TcpTransport::shutdown(std::chrono::seconds) {
    co_return common::IoResult<void>{};
}

fiber::async::Task<common::IoResult<size_t>> TcpTransport::read(void *buf,
                                                      size_t len,
                                                      std::chrono::seconds timeout) {
    auto result = co_await fiber::async::timeout_for(
        [&]() { return stream_->read(buf, len); }, timeout);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    co_return *result;
}

fiber::async::Task<common::IoResult<size_t>> TcpTransport::write(const void *buf,
                                                       size_t len,
                                                       std::chrono::seconds timeout) {
    auto result = co_await fiber::async::timeout_for(
        [&]() { return stream_->write(buf, len); }, timeout);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    co_return *result;
}

void TcpTransport::close() {
    if (stream_) {
        stream_->close();
    }
}

bool TcpTransport::valid() const noexcept {
    return stream_ && stream_->valid();
}

int TcpTransport::fd() const noexcept {
    return stream_ ? stream_->fd() : -1;
}

const net::SocketAddress &TcpTransport::remote_addr() const noexcept {
    return stream_->remote_addr();
}

common::IoResult<std::unique_ptr<TlsTransport>> TlsTransport::create(
    std::unique_ptr<net::TcpStream> stream,
    TlsContext &context) {
    auto transport = std::unique_ptr<TlsTransport>(new TlsTransport(std::move(stream), context));
    auto init_result = transport->init();
    if (!init_result) {
        return std::unexpected(init_result.error());
    }
    return transport;
}

TlsTransport::TlsTransport(std::unique_ptr<net::TcpStream> stream, TlsContext &context)
    : stream_(std::move(stream)), context_(&context) {
}

TlsTransport::~TlsTransport() = default;

common::IoResult<void> TlsTransport::init() {
    if (!context_ || !context_->raw() || !stream_) {
        return std::unexpected(common::IoErr::Invalid);
    }
    tls_stream_ = std::make_unique<net::detail::TlsStreamFd>(stream_->loop(), stream_->fd());
    auto init_result = tls_stream_->init(context_->raw(), context_->is_server());
    if (!init_result) {
        tls_stream_.reset();
        return std::unexpected(init_result.error());
    }
    return {};
}

fiber::async::Task<common::IoResult<void>> TlsTransport::handshake(std::chrono::seconds timeout) {
    if (!tls_stream_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    auto result = co_await fiber::async::timeout_for(
        [&]() { return tls_stream_->handshake(); }, timeout);
    co_return result;
}

fiber::async::Task<common::IoResult<void>> TlsTransport::shutdown(std::chrono::seconds timeout) {
    if (!tls_stream_) {
        co_return common::IoResult<void>{};
    }
    auto result = co_await fiber::async::timeout_for(
        [&]() { return tls_stream_->shutdown(); }, timeout);
    co_return result;
}

fiber::async::Task<common::IoResult<size_t>> TlsTransport::read(void *buf,
                                                      size_t len,
                                                      std::chrono::seconds timeout) {
    auto hs_result = co_await handshake(context_->options().handshake_timeout);
    if (!hs_result) {
        co_return std::unexpected(hs_result.error());
    }
    auto result = co_await fiber::async::timeout_for(
        [&]() { return tls_stream_->read(buf, len); }, timeout);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    co_return *result;
}

fiber::async::Task<common::IoResult<size_t>> TlsTransport::write(const void *buf,
                                                       size_t len,
                                                       std::chrono::seconds timeout) {
    auto hs_result = co_await handshake(context_->options().handshake_timeout);
    if (!hs_result) {
        co_return std::unexpected(hs_result.error());
    }
    auto result = co_await fiber::async::timeout_for(
        [&]() { return tls_stream_->write(buf, len); }, timeout);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    co_return *result;
}

void TlsTransport::close() {
    if (!stream_) {
        return;
    }
    if (tls_stream_) {
        tls_stream_->close();
    }
    stream_->close();
}

bool TlsTransport::valid() const noexcept {
    return stream_ && stream_->valid();
}

int TlsTransport::fd() const noexcept {
    return stream_ ? stream_->fd() : -1;
}

const net::SocketAddress &TlsTransport::remote_addr() const noexcept {
    return stream_->remote_addr();
}

} // namespace fiber::http
