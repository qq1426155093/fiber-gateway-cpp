#ifndef FIBER_HTTP_HTTP1_SERVER_H
#define FIBER_HTTP_HTTP1_SERVER_H

#include <atomic>

#include "../async/Spawn.h"
#include "../async/Task.h"
#include "../async/WaitGroup.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../event/EventLoop.h"
#include "../event/EventLoopGroup.h"
#include "../net/TcpListener.h"
#include "HttpExchange.h"
#include "TlsContext.h"

namespace fiber::http {

class Http1Connection;

class Http1Server : public common::NonCopyable, public common::NonMovable {
public:
    Http1Server(event::EventLoop &loop, HttpHandler handler, HttpServerOptions options = {},
                event::EventLoopGroup *worker_group = nullptr);

    fiber::common::IoResult<void> bind(const net::SocketAddress &addr, const net::ListenOptions &options);
    fiber::async::DetachedTask serve();
    void close();
    fiber::async::Task<void> shutdown_and_wait();
    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] bool shutting_down() const noexcept { return shutdown_.load(std::memory_order_acquire); }

private:
    [[nodiscard]] event::EventLoop &select_connection_loop() noexcept;
    void on_connection_finished() noexcept;

    friend class Http1Connection;

    event::EventLoopGroup *worker_group_ = nullptr;
    HttpHandler handler_;
    HttpServerOptions options_;
    net::TcpListener listener_;
    std::unique_ptr<TlsContext> tls_ctx_;
    fiber::async::WaitGroup connections_wg_{};
    std::atomic<bool> shutdown_{false};
    std::atomic<std::size_t> next_loop_index_{0};
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP1_SERVER_H
