#ifndef FIBER_HTTP_HTTP1_EXCHANGE_IO_H
#define FIBER_HTTP_HTTP1_EXCHANGE_IO_H

#include <string>

#include "../common/mem/IoBuf.h"
#include "HttpExchangeIo.h"

namespace fiber::http {

class HttpExchange;
class HttpTransport;
struct HttpServerOptions;

class Http1ExchangeIo final : public HttpExchangeIo {
public:
    Http1ExchangeIo(HttpTransport &transport, const HttpServerOptions &options, mem::IoBufChain &&body_bufs);

    fiber::async::Task<common::IoResult<ReadBodyResult>> read_body(HttpExchange &exchange, void *buf,
                                                                   size_t len) noexcept override;
    fiber::async::Task<common::IoResult<void>> send_response_header(HttpExchange &exchange, int status,
                                                                    std::string_view reason) override;
    fiber::async::Task<common::IoResult<size_t>> write_body(HttpExchange &exchange, const uint8_t *buf, size_t len,
                                                            bool end) noexcept override;

private:
    fiber::async::Task<common::IoResult<size_t>> read_more() noexcept;
    fiber::async::Task<common::IoResult<std::string_view>> read_line() noexcept;
    fiber::async::Task<common::IoResult<void>> consume_chunk_ending() noexcept;
    [[nodiscard]] size_t copy_available(void *buf, size_t len) noexcept;

    HttpTransport *transport_ = nullptr;
    const HttpServerOptions *options_ = nullptr;
    mem::IoBufChain body_bufs_;

    bool request_body_done_ = false;
    size_t request_body_read_ = 0;
    size_t chunk_remaining_ = 0;
    bool chunk_done_ = false;
    std::string line_buffer_;

    bool response_header_sent_ = false;
    bool response_complete_ = false;
    size_t response_body_sent_ = 0;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP1_EXCHANGE_IO_H
