#ifndef FIBER_HTTP_HTTP1_CONNECTION_H
#define FIBER_HTTP_HTTP1_CONNECTION_H

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../event/EventLoop.h"
#include "HttpExchange.h"
#include "HttpTransport.h"
#include "Http1Parser.h"

namespace fiber::http {

class Http1Connection : public common::NonCopyable, public common::NonMovable {
public:
    Http1Connection(std::unique_ptr<HttpTransport> transport,
                    const HttpServerOptions &options,
                    HttpHandler handler);

    fiber::async::Task<void> run();

    fiber::async::Task<common::IoResult<ReadBodyResult>> read_body(HttpExchange &exchange,
                                                         void *buf,
                                                         size_t len);
    fiber::async::Task<common::IoResult<void>> discard_body(HttpExchange &exchange);
    fiber::async::Task<common::IoResult<void>> send_response_header(HttpExchange &exchange,
                                                          int status,
                                                          std::string_view reason);
    fiber::async::Task<common::IoResult<size_t>> write_body(HttpExchange &exchange,
                                                  const void *buf,
                                                  size_t len,
                                                  bool end);

private:
    struct HeaderBuffer {
        char *data = nullptr;
        size_t cap = 0;
        size_t size = 0;
        size_t pos = 0;
    };

    static constexpr size_t kHeaderInitialSize = 8 * 1024;
    static constexpr size_t kHeaderLargeSize = 32 * 1024;
    static constexpr size_t kHeaderLargeMax = 4;

    common::IoResult<void> ensure_header_defaults(HttpExchange &exchange);
    fiber::async::Task<common::IoResult<void>> write_all(const void *data, size_t len);
    fiber::async::Task<common::IoResult<void>> drain_body(HttpExchange &exchange);
    common::IoResult<void> finalize_response_body(HttpExchange &exchange, bool end);
    static std::string default_reason(int status);

    fiber::async::Task<common::IoResult<size_t>> read_from_stream(std::chrono::seconds timeout);
    void consume_buffer(size_t len);
    bool reset_header_buffer();
    bool grow_header_buffer();

    std::unique_ptr<HttpTransport> transport_;
    HttpServerOptions options_{};
    HttpHandler handler_{};
    mem::BufPool header_pool_;
    HttpExchange exchange_;
    HeaderBuffer header_buffer_{};
    size_t header_large_used_ = 0;
    size_t header_bytes_ = 0;

    std::string recv_buffer_;
    size_t recv_offset_ = 0;
    bool closed_ = false;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP1_CONNECTION_H
