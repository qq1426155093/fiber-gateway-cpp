#ifndef FIBER_HTTP_HTTP1_CONNECTION_H
#define FIBER_HTTP_HTTP1_CONNECTION_H

#include <chrono>
#include <string>
#include <string_view>

#include "../async/Timeout.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../event/EventLoop.h"
#include "../net/SocketAddress.h"
#include "../net/TcpStream.h"
#include "HttpExchange.h"
#include "Http1Parser.h"

namespace fiber::http {

class Http1Connection : public common::NonCopyable, public common::NonMovable {
public:
    Http1Connection(fiber::event::EventLoop &loop,
                    int fd,
                    net::SocketAddress peer,
                    const HttpServerOptions &options,
                    HttpHandler handler);

    HttpTask<void> run();

    HttpTask<common::IoResult<ReadBodyResult>> read_body(HttpExchange &exchange,
                                                         void *buf,
                                                         size_t len);
    HttpTask<common::IoResult<void>> discard_body(HttpExchange &exchange);
    HttpTask<common::IoResult<void>> send_response_header(HttpExchange &exchange,
                                                          int status,
                                                          std::string_view reason);
    HttpTask<common::IoResult<size_t>> write_body(HttpExchange &exchange,
                                                  const void *buf,
                                                  size_t len,
                                                  bool end);

private:
    common::IoResult<void> ensure_header_defaults(HttpExchange &exchange);
    HttpTask<common::IoResult<void>> write_all(const void *data, size_t len);
    HttpTask<common::IoResult<void>> send_continue_if_needed(HttpExchange &exchange);
    HttpTask<common::IoResult<void>> drain_body(HttpExchange &exchange);
    common::IoResult<void> finalize_response_body(HttpExchange &exchange, bool end);
    static std::string default_reason(int status);

    HttpTask<common::IoResult<size_t>> read_from_stream(std::chrono::seconds timeout);
    void consume_buffer(size_t len);

    fiber::net::TcpStream stream_;
    HttpServerOptions options_{};
    HttpHandler handler_{};
    HttpExchange exchange_;
    Http1Parser parser_{};

    std::string recv_buffer_;
    size_t recv_offset_ = 0;
    bool closed_ = false;
    bool parsing_body_ = false;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP1_CONNECTION_H
