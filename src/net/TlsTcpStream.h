#ifndef FIBER_NET_TLS_TCP_STREAM_H
#define FIBER_NET_TLS_TCP_STREAM_H

#include <cstddef>
#include <string>
#include <utility>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../event/EventLoop.h"
#include "SocketAddress.h"
#include "detail/TlsStreamFd.h"

struct ssl_ctx_st;
typedef struct ssl_ctx_st SSL_CTX;

namespace fiber::net {

class TlsTcpStream : public common::NonCopyable, public common::NonMovable {
public:
    using ReadAwaiter = detail::TlsStreamFd::ReadAwaiter;
    using WriteAwaiter = detail::TlsStreamFd::WriteAwaiter;
    using HandshakeAwaiter = detail::TlsStreamFd::HandshakeAwaiter;
    using ShutdownAwaiter = detail::TlsStreamFd::ShutdownAwaiter;

    TlsTcpStream(fiber::event::EventLoop &loop, int fd, SocketAddress remote_addr);
    ~TlsTcpStream();

    common::IoResult<void> init(SSL_CTX *ctx, bool is_server);

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] fiber::event::EventLoop &loop() const noexcept;
    [[nodiscard]] const SocketAddress &remote_addr() const noexcept;
    [[nodiscard]] std::string selected_alpn() const noexcept;
    void close();

    [[nodiscard]] ReadAwaiter read(void *buf, size_t len) noexcept;
    [[nodiscard]] WriteAwaiter write(const void *buf, size_t len) noexcept;
    [[nodiscard]] fiber::common::IoResult<size_t> try_read(void *buf, size_t len) noexcept;
    [[nodiscard]] fiber::common::IoResult<size_t> try_write(const void *buf, size_t len) noexcept;
    [[nodiscard]] HandshakeAwaiter handshake() noexcept;
    [[nodiscard]] ShutdownAwaiter shutdown() noexcept;

private:
    detail::TlsStreamFd stream_;
    SocketAddress remote_addr_{};
};

} // namespace fiber::net

#endif // FIBER_NET_TLS_TCP_STREAM_H
