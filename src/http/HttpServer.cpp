#include "HttpServer.h"

#include <algorithm>
#include <string_view>
#include <utility>
#include <vector>

#include "../async/Spawn.h"
#include "../common/IoError.h"
#include "../net/TcpStream.h"
#include "Http1Context.h"
#include "Http2Connection.h"
#include "HttpTransport.h"

namespace fiber::http {

namespace {

constexpr std::string_view kAlpnH2 = "h2";
constexpr std::string_view kAlpnHttp11 = "http/1.1";

bool contains_alpn(const std::vector<std::string> &alpn, std::string_view target) {
    for (const auto &proto: alpn) {
        if (proto == target) {
            return true;
        }
    }
    return false;
}

void append_unique_alpn(std::vector<std::string> &alpn, std::string_view proto) {
    if (!contains_alpn(alpn, proto)) {
        alpn.emplace_back(proto);
    }
}

void normalize_auto_alpn(TlsOptions &options) {
    std::vector<std::string> normalized;
    normalized.reserve(options.alpn.size() + 2);

    normalized.emplace_back(kAlpnH2);
    normalized.emplace_back(kAlpnHttp11);

    for (const auto &proto: options.alpn) {
        if (proto.empty() || proto == kAlpnH2 || proto == kAlpnHttp11) {
            continue;
        }
        append_unique_alpn(normalized, proto);
    }

    options.alpn = std::move(normalized);
}

} // namespace

HttpServer::HttpServer(event::EventLoop &loop, HttpHandler handler, HttpServerOptions options) :
    loop_(loop), handler_(std::move(handler)), options_(std::move(options)), listener_(loop) {}

fiber::common::IoResult<void> HttpServer::bind(const net::SocketAddress &addr, const net::ListenOptions &options) {
    auto result = listener_.bind(addr, options);
    if (!result) {
        return std::unexpected(result.error());
    }
    if (options_.tls.enabled) {
        normalize_auto_alpn(options_.tls);
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
    bool use_http2 = false;
    if (options_.tls.enabled) {
        auto tls_stream = std::make_unique<net::TlsTcpStream>(loop_, accept.release_fd(), accept.take_peer());
        if (!tls_ctx_) {
            co_return;
        }
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
        use_http2 = transport->negotiated_alpn() == kAlpnH2;
    } else {
        std::unique_ptr<net::TcpStream> stream =
                std::make_unique<net::TcpStream>(loop_, accept.release_fd(), accept.take_peer());
        transport = std::make_unique<TcpTransport>(std::move(stream));
    }

    if (use_http2) {
        co_await serve_http2(std::move(transport));
        co_return;
    }

    co_await serve_http1(std::move(transport));
    co_return;
}

fiber::async::Task<void> HttpServer::serve_http1(std::unique_ptr<HttpTransport> transport) {
    if (!transport) {
        co_return;
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
}

fiber::async::Task<void> HttpServer::serve_http2(std::unique_ptr<HttpTransport> transport) {
    if (!transport) {
        co_return;
    }
    Http2Connection conn(loop_, std::move(transport), handler_, options_);
    auto init_result = conn.init();
    if (!init_result) {
        conn.close();
        co_return;
    }
    co_await conn.run();
    co_return;
}

void HttpServer::close() { listener_.close(); }

int HttpServer::fd() const noexcept { return listener_.fd(); }

} // namespace fiber::http
