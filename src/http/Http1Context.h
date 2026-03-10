#ifndef FIBER_HTTP_HTTP1_CONTEXT_H
#define FIBER_HTTP_HTTP1_CONTEXT_H

#include <chrono>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "Http1Parser.h"
#include "HttpExchange.h"
#include "HttpHeaders.h"
#include "HttpTransport.h"

namespace fiber::http {

template<typename V>
class HeaderMap;

class Http1Context : public common::NonCopyable, public common::NonMovable {
public:
    Http1Context(HttpTransport &transport, const HttpServerOptions &options);

    fiber::async::Task<fiber::common::IoResult<ParseCode>> parse_request(HttpExchange &exchange);
    [[nodiscard]] const HttpServerOptions &options() const noexcept { return options_; }

private:
    using HeaderHandler = bool (*)(HttpExchange &exchange, const HttpHeaders::HeaderField &field);

    static const HeaderMap<HeaderHandler> &header_handler_map();
    static bool handle_content_length(HttpExchange &exchange, const HttpHeaders::HeaderField &header);
    static bool handle_transfer_encoding(HttpExchange &exchange, const HttpHeaders::HeaderField &header);
    static bool handle_connection(HttpExchange &exchange, const HttpHeaders::HeaderField &header);

    HttpTransport *transport_ = nullptr;
    HttpServerOptions options_;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP1_CONTEXT_H
