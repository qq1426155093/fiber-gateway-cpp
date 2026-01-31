#ifndef FIBER_HTTP_HTTP1_CONTEXT_H
#define FIBER_HTTP_HTTP1_CONTEXT_H

#include <chrono>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../common/mem/BufPool.h"
#include "HeadBuf.h"
#include "Http1Parser.h"
#include "HttpExchange.h"
#include "HttpTransport.h"

namespace fiber::http {

template <typename V>
class HeaderMap;

class Http1Context : public common::NonCopyable, public common::NonMovable {
public:
    Http1Context(HttpTransport &transport, const HttpServerOptions &options);

    fiber::async::Task<fiber::common::IoResult<ParseCode>> parse_request(HttpExchange &exchange, BufChain *chain);

    mem::BufPool &header_pool() noexcept { return header_pool_; }
    const HttpServerOptions &options() const noexcept { return options_; }

private:
    using HeaderHandler = bool (*)(HttpExchange &exchange, std::string_view value);

    static const HeaderMap<HeaderHandler> &header_handler_map();
    static bool handle_content_length(HttpExchange &exchange, std::string_view value);
    static bool handle_transfer_encoding(HttpExchange &exchange, std::string_view value);
    static bool handle_connection(HttpExchange &exchange, std::string_view value);

    HeaderBuffers header_bufs_;
    mem::BufPool header_pool_;
    HttpTransport *transport_ = nullptr;
    HttpServerOptions options_;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP1_CONTEXT_H
