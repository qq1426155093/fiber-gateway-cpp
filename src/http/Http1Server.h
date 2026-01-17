#ifndef FIBER_HTTP_HTTP1_SERVER_H
#define FIBER_HTTP_HTTP1_SERVER_H

#include <memory>

#include "../async/Spawn.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../event/EventLoop.h"
#include "../net/SocketAddress.h"
#include "../net/TcpListener.h"
#include "HttpExchange.h"

namespace fiber::http {

class TlsContext;

class Http1Server : public common::NonCopyable, public common::NonMovable {
public:
    Http1Server(event::EventLoop &loop,
                HttpHandler handler,
                HttpServerOptions options = {});
    ~Http1Server();

    common::IoResult<void> bind(const net::SocketAddress &addr,
                                const net::ListenOptions &options);
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] int fd() const noexcept;
    void close();

    fiber::async::DetachedTask serve();

private:
    event::EventLoop &loop_;
    net::TcpListener listener_;
    HttpHandler handler_;
    HttpServerOptions options_{};
    std::unique_ptr<TlsContext> tls_context_{};
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP1_SERVER_H
