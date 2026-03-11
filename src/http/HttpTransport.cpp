#include "HttpTransport.h"

#include <algorithm>
#include <array>
#include <sys/uio.h>

#include "../async/Timeout.h"

namespace fiber::http {

namespace {

constexpr int kMaxIov = 16;

common::IoResult<std::chrono::milliseconds> remaining_timeout(event::EventLoop &loop,
                                                              std::chrono::steady_clock::time_point deadline) {
    auto now = loop.now();
    if (deadline <= now) {
        return std::unexpected(common::IoErr::TimedOut);
    }
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    if (remaining <= std::chrono::milliseconds::zero()) {
        remaining = std::chrono::milliseconds(1);
    }
    return remaining;
}

fiber::async::Task<common::IoResult<void>> wait_tls_event(net::TlsTcpStream &stream, event::IoEvent io_event,
                                                          std::chrono::steady_clock::time_point deadline) {
    auto timeout_result = remaining_timeout(stream.loop(), deadline);
    if (!timeout_result) {
        co_return std::unexpected(timeout_result.error());
    }
    common::IoResult<void> wait_result;
    if (io_event == event::IoEvent::Read) {
        wait_result = co_await fiber::async::timeout_for([&]() { return stream.wait_readable(); }, *timeout_result);
    } else if (io_event == event::IoEvent::Write) {
        wait_result = co_await fiber::async::timeout_for([&]() { return stream.wait_writable(); }, *timeout_result);
    } else {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (!wait_result) {
        co_return std::unexpected(wait_result.error());
    }
    co_return common::IoResult<void>{};
}

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
    auto deadline = stream_.loop().now() + timeout;
    for (;;) {
        event::IoEvent wait_event = event::IoEvent::None;
        common::IoErr err = stream_.poll_handshake(wait_event);
        if (err == common::IoErr::None) {
            co_return common::IoResult<void>{};
        }
        if (err != common::IoErr::WouldBlock) {
            co_return std::unexpected(err);
        }
        auto wait_result = co_await wait_tls_event(stream_, wait_event, deadline);
        if (!wait_result) {
            co_return std::unexpected(wait_result.error());
        }
    }
}

fiber::async::Task<common::IoResult<void>> TlsTransport::shutdown(std::chrono::milliseconds timeout) {
    auto deadline = stream_.loop().now() + timeout;
    for (;;) {
        event::IoEvent wait_event = event::IoEvent::None;
        common::IoErr err = stream_.poll_shutdown(wait_event);
        if (err == common::IoErr::None) {
            co_return common::IoResult<void>{};
        }
        if (err != common::IoErr::WouldBlock) {
            co_return std::unexpected(err);
        }
        auto wait_result = co_await wait_tls_event(stream_, wait_event, deadline);
        if (!wait_result) {
            co_return std::unexpected(wait_result.error());
        }
    }
}

fiber::async::Task<common::IoResult<size_t>> TlsTransport::read(void *buf, size_t len,
                                                                std::chrono::milliseconds timeout) {
    auto hs_result = co_await handshake(context_->options().handshake_timeout);
    if (!hs_result) {
        co_return std::unexpected(hs_result.error());
    }
    auto deadline = stream_.loop().now() + timeout;
    for (;;) {
        size_t out = 0;
        event::IoEvent wait_event = event::IoEvent::None;
        common::IoErr err = stream_.poll_read(buf, len, out, wait_event);
        if (err == common::IoErr::None) {
            co_return out;
        }
        if (err != common::IoErr::WouldBlock) {
            co_return std::unexpected(err);
        }
        auto wait_result = co_await wait_tls_event(stream_, wait_event, deadline);
        if (!wait_result) {
            co_return std::unexpected(wait_result.error());
        }
    }
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
    auto deadline = stream_.loop().now() + timeout;
    for (;;) {
        size_t out = 0;
        event::IoEvent wait_event = event::IoEvent::None;
        common::IoErr err = stream_.poll_read(buf.writable_data(), writable, out, wait_event);
        if (err == common::IoErr::None) {
            buf.commit(out);
            co_return out;
        }
        if (err != common::IoErr::WouldBlock) {
            co_return std::unexpected(err);
        }
        auto wait_result = co_await wait_tls_event(stream_, wait_event, deadline);
        if (!wait_result) {
            co_return std::unexpected(wait_result.error());
        }
    }
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
    auto deadline = stream_.loop().now() + timeout;
    for (;;) {
        size_t out = 0;
        event::IoEvent wait_event = event::IoEvent::None;
        common::IoErr err = stream_.poll_write(buf, len, out, wait_event);
        if (err == common::IoErr::None) {
            co_return out;
        }
        if (err != common::IoErr::WouldBlock) {
            co_return std::unexpected(err);
        }
        auto wait_result = co_await wait_tls_event(stream_, wait_event, deadline);
        if (!wait_result) {
            co_return std::unexpected(wait_result.error());
        }
    }
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
    auto deadline = stream_.loop().now() + timeout;
    for (;;) {
        size_t out = 0;
        event::IoEvent wait_event = event::IoEvent::None;
        common::IoErr err = stream_.poll_write(buf.readable_data(), readable, out, wait_event);
        if (err == common::IoErr::None) {
            buf.consume(out);
            co_return out;
        }
        if (err != common::IoErr::WouldBlock) {
            co_return std::unexpected(err);
        }
        auto wait_result = co_await wait_tls_event(stream_, wait_event, deadline);
        if (!wait_result) {
            co_return std::unexpected(wait_result.error());
        }
    }
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
