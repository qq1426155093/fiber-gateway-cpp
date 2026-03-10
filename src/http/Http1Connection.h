#ifndef FIBER_HTTP_HTTP1_CONNECTION_H
#define FIBER_HTTP_HTTP1_CONNECTION_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../common/mem/IoBuf.h"
#include "../event/EventLoop.h"
#include "Http1Parser.h"
#include "HttpExchange.h"

namespace fiber::http {

template<typename V>
class HeaderMap;

class Http1Server;
class HttpTransport;

class Http1Connection : public common::NonCopyable, public common::NonMovable {
public:
    Http1Connection(Http1Server *server, std::unique_ptr<HttpTransport> transport, HttpHandler handler,
                    HttpServerOptions options);
    ~Http1Connection();

    fiber::async::Task<void> run();
    void request_connection_close() noexcept;

    [[nodiscard]] event::EventLoop &loop() const noexcept { return loop_; }
    [[nodiscard]] HttpTransport &transport() noexcept { return *transport_; }
    [[nodiscard]] const HttpServerOptions &options() const noexcept { return options_; }
    [[nodiscard]] mem::IoBufChain &inbound_bufs() noexcept { return inbound_bufs_; }
    [[nodiscard]] bool stopping() const noexcept;

    fiber::async::Task<common::IoResult<size_t>> read_into_inbound(std::chrono::milliseconds timeout) noexcept;

private:
    using HeaderHandler = bool (*)(HttpExchange &exchange, const HttpHeaders::HeaderField &field);

    static const HeaderMap<HeaderHandler> &header_handler_map();
    static bool handle_content_length(HttpExchange &exchange, const HttpHeaders::HeaderField &header);
    static bool handle_transfer_encoding(HttpExchange &exchange, const HttpHeaders::HeaderField &header);
    static bool handle_connection(HttpExchange &exchange, const HttpHeaders::HeaderField &header);

    fiber::async::Task<common::IoResult<ParseCode>> parse_request(HttpExchange &exchange);
    std::size_t drain_inbound(mem::IoBuf &buffer) noexcept;
    common::IoResult<void> grow_header_buffer(mem::IoBuf &buffer, std::size_t &growth_count,
                                              RequestLineParser *request_parser,
                                              HeaderLineParser *header_parser) noexcept;
    void finish() noexcept;

    Http1Server *server_ = nullptr;
    event::EventLoop &loop_;
    std::unique_ptr<HttpTransport> transport_;
    HttpHandler handler_;
    HttpServerOptions options_;
    mem::IoBufChain inbound_bufs_;

    std::atomic<bool> close_after_response_{false};
    std::atomic<bool> finished_{false};
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP1_CONNECTION_H
