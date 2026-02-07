#include "HttpTransport.h"

#include <algorithm>
#include <array>
#include <sys/uio.h>

#include "../async/Timeout.h"
#include "../net/detail/TlsStreamFd.h"

namespace fiber::http {

namespace {

constexpr int kMaxIov = 16;

int build_read_iov(BufChain *head, std::array<iovec, kMaxIov> &iov) {
    int count = 0;
    for (auto *node = head; node && count < kMaxIov; node = node->next) {
        size_t len = node->writable();
        if (len == 0) {
            continue;
        }
        iov[count].iov_base = node->last;
        iov[count].iov_len = len;
        ++count;
    }
    return count;
}

int build_write_iov(BufChain *head, std::array<iovec, kMaxIov> &iov) {
    int count = 0;
    for (auto *node = head; node && count < kMaxIov; node = node->next) {
        size_t len = node->readable();
        if (len == 0) {
            continue;
        }
        iov[count].iov_base = const_cast<std::uint8_t *>(node->pos);
        iov[count].iov_len = len;
        ++count;
    }
    return count;
}

void advance_write(BufChain *head, size_t bytes) {
    for (auto *node = head; node && bytes > 0; node = node->next) {
        size_t len = node->writable();
        if (len == 0) {
            continue;
        }
        size_t take = std::min(len, bytes);
        node->last += take;
        bytes -= take;
    }
}

void advance_read(BufChain *head, size_t bytes) {
    for (auto *node = head; node && bytes > 0; node = node->next) {
        size_t len = node->readable();
        if (len == 0) {
            continue;
        }
        size_t take = std::min(len, bytes);
        node->pos += take;
        bytes -= take;
    }
}

BufChain *first_writable(BufChain *head) {
    for (auto *node = head; node; node = node->next) {
        if (node->writable() > 0) {
            return node;
        }
    }
    return nullptr;
}

BufChain *first_readable(BufChain *head) {
    for (auto *node = head; node; node = node->next) {
        if (node->readable() > 0) {
            return node;
        }
    }
    return nullptr;
}

} // namespace

TcpTransport::TcpTransport(std::unique_ptr<net::TcpStream> stream) : stream_(std::move(stream)) {
}

fiber::async::Task<common::IoResult<void>> TcpTransport::handshake(std::chrono::milliseconds) {
    co_return common::IoResult<void>{};
}

fiber::async::Task<common::IoResult<void>> TcpTransport::shutdown(std::chrono::milliseconds) {
    co_return common::IoResult<void>{};
}

fiber::async::Task<common::IoResult<size_t>> TcpTransport::read(void *buf,
                                                      size_t len,
                                                      std::chrono::milliseconds timeout) {
    auto result = co_await fiber::async::timeout_for(
        [&]() { return stream_->read(buf, len); }, timeout);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    co_return *result;
}

fiber::async::Task<common::IoResult<size_t>> TcpTransport::read_into(BufChain *buf,
                                                                     std::chrono::milliseconds timeout) {
    if (!buf) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    size_t writable = buf->writable();
    if (writable == 0) {
        co_return static_cast<size_t>(0);
    }
    auto result = co_await fiber::async::timeout_for(
        [&]() { return stream_->read(buf->last, writable); }, timeout);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    buf->last += *result;
    co_return *result;
}

fiber::async::Task<common::IoResult<size_t>> TcpTransport::readv_into(BufChain *bufs,
                                                                      std::chrono::milliseconds timeout) {
    if (!bufs) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    std::array<iovec, kMaxIov> iov{};
    int count = build_read_iov(bufs, iov);
    if (count == 0) {
        co_return static_cast<size_t>(0);
    }
    auto result = co_await fiber::async::timeout_for(
        [&]() { return stream_->readv(iov.data(), count); }, timeout);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    advance_write(bufs, *result);
    co_return *result;
}

fiber::async::Task<common::IoResult<size_t>> TcpTransport::write(const void *buf,
                                                       size_t len,
                                                       std::chrono::milliseconds timeout) {
    auto result = co_await fiber::async::timeout_for(
        [&]() { return stream_->write(buf, len); }, timeout);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    co_return *result;
}

fiber::async::Task<common::IoResult<size_t>> TcpTransport::write(BufChain *buf,
                                                                 std::chrono::milliseconds timeout) {
    if (!buf) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    size_t readable = buf->readable();
    if (readable == 0) {
        co_return static_cast<size_t>(0);
    }
    auto result = co_await fiber::async::timeout_for(
        [&]() { return stream_->write(buf->pos, readable); }, timeout);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    buf->pos += *result;
    co_return *result;
}

fiber::async::Task<common::IoResult<size_t>> TcpTransport::writev(BufChain *buf,
                                                                  std::chrono::milliseconds timeout) {
    if (!buf) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    std::array<iovec, kMaxIov> iov{};
    int count = build_write_iov(buf, iov);
    if (count == 0) {
        co_return static_cast<size_t>(0);
    }
    auto result = co_await fiber::async::timeout_for(
        [&]() { return stream_->writev(iov.data(), count); }, timeout);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    advance_read(buf, *result);
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

fiber::async::Task<common::IoResult<void>> TlsTransport::handshake(std::chrono::milliseconds timeout) {
    if (!tls_stream_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    auto result = co_await fiber::async::timeout_for(
        [&]() { return tls_stream_->handshake(); }, timeout);
    co_return result;
}

fiber::async::Task<common::IoResult<void>> TlsTransport::shutdown(std::chrono::milliseconds timeout) {
    if (!tls_stream_) {
        co_return common::IoResult<void>{};
    }
    auto result = co_await fiber::async::timeout_for(
        [&]() { return tls_stream_->shutdown(); }, timeout);
    co_return result;
}

fiber::async::Task<common::IoResult<size_t>> TlsTransport::read(void *buf,
                                                      size_t len,
                                                      std::chrono::milliseconds timeout) {
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

fiber::async::Task<common::IoResult<size_t>> TlsTransport::read_into(BufChain *buf,
                                                                     std::chrono::milliseconds timeout) {
    if (!buf) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    size_t writable = buf->writable();
    if (writable == 0) {
        co_return static_cast<size_t>(0);
    }
    auto hs_result = co_await handshake(context_->options().handshake_timeout);
    if (!hs_result) {
        co_return std::unexpected(hs_result.error());
    }
    auto result = co_await fiber::async::timeout_for(
        [&]() { return tls_stream_->read(buf->last, writable); }, timeout);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    buf->last += *result;
    co_return *result;
}

fiber::async::Task<common::IoResult<size_t>> TlsTransport::readv_into(BufChain *bufs,
                                                                      std::chrono::milliseconds timeout) {
    if (!bufs) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    BufChain *target = first_writable(bufs);
    if (!target) {
        co_return static_cast<size_t>(0);
    }
    co_return co_await read_into(target, timeout);
}

fiber::async::Task<common::IoResult<size_t>> TlsTransport::write(const void *buf,
                                                       size_t len,
                                                       std::chrono::milliseconds timeout) {
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

fiber::async::Task<common::IoResult<size_t>> TlsTransport::write(BufChain *buf,
                                                                 std::chrono::milliseconds timeout) {
    if (!buf) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    size_t readable = buf->readable();
    if (readable == 0) {
        co_return static_cast<size_t>(0);
    }
    auto hs_result = co_await handshake(context_->options().handshake_timeout);
    if (!hs_result) {
        co_return std::unexpected(hs_result.error());
    }
    auto result = co_await fiber::async::timeout_for(
        [&]() { return tls_stream_->write(buf->pos, readable); }, timeout);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    buf->pos += *result;
    co_return *result;
}

fiber::async::Task<common::IoResult<size_t>> TlsTransport::writev(BufChain *buf,
                                                                  std::chrono::milliseconds timeout) {
    if (!buf) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    BufChain *target = first_readable(buf);
    if (!target) {
        co_return static_cast<size_t>(0);
    }
    co_return co_await write(target, timeout);
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
