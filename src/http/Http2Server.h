#ifndef FIBER_HTTP_HTTP2_SERVER_H
#define FIBER_HTTP_HTTP2_SERVER_H

#include <memory>

#include "../async/Spawn.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../event/EventLoop.h"
#include "../net/TcpListener.h"
#include "HttpExchange.h"
#include "TlsContext.h"

namespace fiber::http {

class Http2Server : public common::NonCopyable, public common::NonMovable {
public:
    Http2Server(event::EventLoop &loop, HttpHandler handler, HttpServerOptions options = {});

    fiber::common::IoResult<void> bind(const net::SocketAddress &addr,
                                       const net::ListenOptions &options);
    fiber::async::DetachedTask serve();
    void close();
    [[nodiscard]] int fd() const noexcept;

private:
    event::EventLoop &loop_;
    HttpHandler handler_;
    HttpServerOptions options_;
    net::TcpListener listener_;
    std::unique_ptr<TlsContext> tls_ctx_;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_SERVER_H
