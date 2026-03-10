#include "Http2Server.h"

#include "../async/Spawn.h"
#include "../common/IoError.h"
#include "../net/TcpStream.h"
#include "Http2Connection.h"
#include "HttpTransport.h"

namespace fiber::http {

namespace {

bool has_h2_alpn(const std::vector<std::string> &alpn) {
    for (const auto &proto: alpn) {
        if (proto == "h2") {
            return true;
        }
    }
    return false;
}

} // namespace

Http2Server::Http2Server(event::EventLoop &loop, HttpHandler handler, HttpServerOptions options) :
    loop_(loop), handler_(std::move(handler)), options_(std::move(options)), listener_(loop) {}

fiber::common::IoResult<void> Http2Server::bind(const net::SocketAddress &addr, const net::ListenOptions &options) {
    auto result = listener_.bind(addr, options);
    if (!result) {
        return std::unexpected(result.error());
    }
    if (options_.tls.enabled) {
        if (!has_h2_alpn(options_.tls.alpn)) {
            options_.tls.alpn = {"h2"};
        }
        auto ctx = std::make_unique<TlsContext>(options_.tls, true);
        auto init_result = ctx->init();
        if (!init_result) {
            return std::unexpected(init_result.error());
        }
        tls_ctx_ = std::move(ctx);
    }
    return {};
}

fiber::async::DetachedTask Http2Server::serve() {
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

            Http2Connection conn(loop_, std::move(transport), handler_, options_);
            auto init_result = conn.init();
            if (!init_result) {
                conn.close();
                co_return;
            }
            co_await conn.run();
            co_return;
        });
    }
    co_return;
}

void Http2Server::close() { listener_.close(); }

int Http2Server::fd() const noexcept { return listener_.fd(); }

} // namespace fiber::http
