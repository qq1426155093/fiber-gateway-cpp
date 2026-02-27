#include "Http1Server.h"

#include "../async/Spawn.h"
#include "../common/IoError.h"
#include "../net/TcpStream.h"
#include "Http1Context.h"
#include "HttpTransport.h"

namespace fiber::http {

Http1Server::Http1Server(event::EventLoop &loop, HttpHandler handler, HttpServerOptions options) :
    loop_(loop), handler_(std::move(handler)), options_(std::move(options)), listener_(loop) {}

fiber::common::IoResult<void> Http1Server::bind(const net::SocketAddress &addr, const net::ListenOptions &options) {
    auto result = listener_.bind(addr, options);
    if (!result) {
        return std::unexpected(result.error());
    }
    if (options_.tls.enabled) {
        auto ctx = std::make_unique<TlsContext>(options_.tls, true);
        auto init_result = ctx->init();
        if (!init_result) {
            return std::unexpected(init_result.error());
        }
        tls_ctx_ = std::move(ctx);
    }
    return {};
}

fiber::async::DetachedTask Http1Server::serve() {
    while (listener_.valid()) {
        auto accept_result = co_await listener_.accept();
        if (!accept_result) {
            if (accept_result.error() == common::IoErr::Canceled || accept_result.error() == common::IoErr::BadFd) {
                break;
            }
            continue;
        }
        auto accept = *accept_result;
        fiber::async::spawn(loop_, [this, accept = std::move(accept)]() mutable -> fiber::async::DetachedTask {
            std::unique_ptr<net::TcpStream> stream =
                    std::make_unique<net::TcpStream>(loop_, accept.fd, std::move(accept.peer));
            std::unique_ptr<HttpTransport> transport;
            if (options_.tls.enabled) {
                if (!tls_ctx_) {
                    co_return;
                }
                int fd = stream->release_fd();
                if (fd < 0) {
                    co_return;
                }
                auto tls_stream = std::make_unique<net::TlsTcpStream>(stream->loop(), fd, stream->remote_addr());
                auto tls_result = TlsTransport::create(std::move(tls_stream), *tls_ctx_);
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
                transport = std::make_unique<TcpTransport>(std::move(stream));
            }

            Http1Context context(*transport, options_);
            HttpExchange exchange(options_);
            auto parse_result = co_await context.parse_request(exchange, nullptr);
            if (!parse_result || *parse_result != ParseCode::Ok) {
                transport->close();
                co_return;
            }
            co_await handler_(exchange);
            auto shutdown_result = co_await transport->shutdown(options_.write_timeout);
            if (!shutdown_result) {
                transport->close();
                co_return;
            }
            transport->close();
            co_return;
        });
    }
    co_return;
}

void Http1Server::close() { listener_.close(); }

int Http1Server::fd() const noexcept { return listener_.fd(); }

} // namespace fiber::http
