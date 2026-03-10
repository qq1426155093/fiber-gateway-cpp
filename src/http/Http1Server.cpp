#include "Http1Server.h"

#include <utility>

#include "../common/Assert.h"
#include "../common/IoError.h"
#include "../net/TcpStream.h"
#include "Http1Connection.h"
#include "HttpTransport.h"

namespace fiber::http {

Http1Server::Http1Server(event::EventLoop &loop, HttpHandler handler, HttpServerOptions options,
                         event::EventLoopGroup *worker_group) :
    worker_group_(worker_group), handler_(std::move(handler)), options_(std::move(options)), listener_(loop) {}

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
    auto *accept_loop = event::EventLoop::current_or_null();
    FIBER_ASSERT(accept_loop != nullptr);

    while (listener_.valid() && !shutdown_.load(std::memory_order_acquire)) {
        auto accept_result = co_await listener_.accept();
        if (!accept_result) {
            if (accept_result.error() == common::IoErr::Canceled || accept_result.error() == common::IoErr::BadFd) {
                break;
            }
            continue;
        }
        if (shutdown_.load(std::memory_order_acquire)) {
            continue;
        }

        auto accept = std::move(*accept_result);
        connections_wg_.add();

        fiber::async::spawn(
                select_connection_loop(), [this, accept = std::move(accept)]() mutable -> fiber::async::DetachedTask {
                    struct PendingConnectionGuard {
                        Http1Server *server = nullptr;

                        ~PendingConnectionGuard() { server->on_connection_finished(); }
                    } guard{this};

                    std::unique_ptr<HttpTransport> transport;
                    if (options_.tls.enabled) {
                        if (!tls_ctx_) {
                            co_return;
                        }
                        auto tls_result =
                                TlsTransport::create(event::EventLoop::current(), std::move(accept), *tls_ctx_);
                        if (!tls_result) {
                            co_return;
                        }
                        transport = std::move(*tls_result);
                    } else {
                        auto tcp_result = TcpTransport::create(event::EventLoop::current(), std::move(accept));
                        if (!tcp_result) {
                            co_return;
                        }
                        transport = std::move(*tcp_result);
                    }

                    Http1Connection connection(this, std::move(transport), handler_, options_);
                    co_await connection.run();
                });
    }
    co_return;
}

void Http1Server::close() { listener_.close(); }

fiber::async::Task<void> Http1Server::shutdown_and_wait() {
    shutdown_.store(true, std::memory_order_release);
    listener_.close();
    co_await connections_wg_.join();
}

int Http1Server::fd() const noexcept { return listener_.fd(); }

event::EventLoop &Http1Server::select_connection_loop() noexcept {
    if (!worker_group_ || worker_group_->size() == 0) {
        return event::EventLoop::current();
    }
    std::size_t index = next_loop_index_.fetch_add(1, std::memory_order_relaxed);
    return worker_group_->at(index % worker_group_->size());
}

void Http1Server::on_connection_finished() noexcept { connections_wg_.done(); }

} // namespace fiber::http
