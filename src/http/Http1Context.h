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

class Http1Context : public common::NonCopyable, public common::NonMovable {
public:
    Http1Context(HttpTransport &transport, const HttpServerOptions &options);

    fiber::async::Task<fiber::common::IoResult<ParseCode>> parse_request(HttpExchange &exchange, BufChain *chain);

    mem::BufPool &header_pool() noexcept { return header_pool_; }
    const HttpServerOptions &options() const noexcept { return options_; }

private:
    HeaderBuffers header_bufs_;
    mem::BufPool header_pool_;
    HttpTransport *transport_ = nullptr;
    HttpServerOptions options_;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP1_CONTEXT_H
