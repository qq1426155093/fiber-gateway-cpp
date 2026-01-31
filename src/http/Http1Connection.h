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
#include "HeadBuf.h"
#include "HttpExchange.h"
#include "HttpTransport.h"

namespace fiber::http {

class Http1Connection : public common::NonCopyable, public common::NonMovable {
public:
    Http1Connection(std::unique_ptr<HttpTransport> transport, const HttpServerOptions &options, HttpHandler handler);

    fiber::async::Task<void> run();

    fiber::async::Task<common::IoResult<ReadBodyResult>> read_body(HttpExchange &exchange, void *buf, size_t len);
    fiber::async::Task<common::IoResult<void>> discard_body(HttpExchange &exchange);
    fiber::async::Task<common::IoResult<void>> send_response_header(HttpExchange &exchange, int status,
                                                                    std::string_view reason);
    fiber::async::Task<common::IoResult<size_t>> write_body(HttpExchange &exchange, const void *buf, size_t len,
                                                            bool end);

private:
    common::IoResult<void> ensure_header_defaults(HttpExchange &exchange);
    fiber::async::Task<common::IoResult<void>> write_all(const void *data, size_t len);
    fiber::async::Task<common::IoResult<void>> drain_body(HttpExchange &exchange);
    common::IoResult<void> finalize_response_body(HttpExchange &exchange, bool end);
    static std::string default_reason(int status);

    fiber::async::Task<common::IoResult<size_t>> read_from_stream(std::chrono::seconds timeout, BufChain *dst);

    std::unique_ptr<HttpTransport> transport_;
    HttpServerOptions options_{};
    mem::BufPool header_pool_;
    HeaderBuffers header_buffers_{};
    bool closed_ = false;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP1_CONNECTION_H
