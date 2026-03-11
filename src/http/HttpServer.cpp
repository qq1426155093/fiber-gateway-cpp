#include "HttpServer.h"

#include <memory>
#include <utility>
#include <vector>

#include "../async/Spawn.h"
#include "../common/IoError.h"
#include "../net/TcpStream.h"
#include "Http1Connection.h"
#include "HttpTransport.h"
#include "TlsAlpn.h"

namespace fiber::http {

HttpServer::HttpServer(event::EventLoop &loop, HttpHandler handler, HttpServerOptions options) :
    loop_(loop), handler_(std::move(handler)), options_(std::move(options)), listener_(loop) {}

fiber::common::IoResult<void> HttpServer::bind(const net::SocketAddress &addr, const net::ListenOptions &options) {
    auto result = listener_.bind(addr, options);
    if (!result) {
        return std::unexpected(result.error());
    }
    if (options_.tls.enabled) {
        normalize_http1_alpn(options_.tls);
        auto ctx = std::make_unique<TlsContext>(options_.tls, true);
        auto init_result = ctx->init();
        if (!init_result) {
            return std::unexpected(init_result.error());
        }
        tls_ctx_ = std::move(ctx);
    }
    return {};
}

fiber::async::DetachedTask HttpServer::serve() {
    while (listener_.valid()) {
        auto accept_result = co_await listener_.accept();
        if (!accept_result) {
            if (accept_result.error() == common::IoErr::Canceled || accept_result.error() == common::IoErr::BadFd) {
                break;
            }
            continue;
        }

        auto accept = std::move(*accept_result);
        fiber::async::spawn(loop_, [this, accept = std::move(accept)]() mutable -> fiber::async::DetachedTask {
            return handle_connection(std::move(accept));
        });
    }
    co_return;
}

fiber::async::DetachedTask HttpServer::handle_connection(net::AcceptResult accept) {
    std::unique_ptr<HttpTransport> transport;
    if (options_.tls.enabled) {
        if (!tls_ctx_) {
            co_return;
        }
        auto tls_result = TlsTransport::create(loop_, std::move(accept), *tls_ctx_);
        if (!tls_result) {
            co_return;
        }
        transport = std::move(*tls_result);
        auto hs_result = co_await transport->handshake(options_.tls.handshake_timeout);
        if (!hs_result) {
            transport->close();
            co_return;
        }
    } else {
        auto tcp_result = TcpTransport::create(loop_, std::move(accept));
        if (!tcp_result) {
            co_return;
        }
        transport = std::move(*tcp_result);
    }

    co_await serve_http1(std::move(transport));
    co_return;
}

fiber::async::Task<void> HttpServer::serve_http1(std::unique_ptr<HttpTransport> transport) {
    if (!transport) {
        co_return;
    }
    Http1Connection connection(nullptr, std::move(transport), handler_, options_);
    co_await connection.run();
    co_return;
}

void HttpServer::close() { listener_.close(); }

int HttpServer::fd() const noexcept { return listener_.fd(); }

} // namespace fiber::http
