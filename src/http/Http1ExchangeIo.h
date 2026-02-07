#ifndef FIBER_HTTP_HTTP1_EXCHANGE_IO_H
#define FIBER_HTTP_HTTP1_EXCHANGE_IO_H

#include <string>

#include "HeadBuf.h"
#include "HttpExchangeIo.h"

namespace fiber::http {

class HttpExchange;
class HttpTransport;
struct HttpServerOptions;

class Http1ExchangeIo final : public HttpExchangeIo {
public:
    Http1ExchangeIo(HttpTransport &transport, const HttpServerOptions &options, BufChain *header_adjacent_body);

    fiber::async::Task<common::IoResult<ReadBodyResult>> read_body(HttpExchange &exchange,
                                                                    void *buf,
                                                                    size_t len) noexcept override;
    fiber::async::Task<common::IoResult<void>> send_response_header(HttpExchange &exchange,
                                                                     int status,
                                                                     std::string_view reason) override;
    fiber::async::Task<common::IoResult<size_t>> write_body(HttpExchange &exchange,
                                                             const uint8_t *buf,
                                                             size_t len,
                                                             bool end) noexcept override;

private:
    HttpTransport *transport_ = nullptr;
    const HttpServerOptions *options_ = nullptr;
    BufChain *header_adjacent_body_ = nullptr;

    bool request_body_done_ = false;
    size_t request_body_read_ = 0;
    size_t chunk_remaining_ = 0;
    bool chunk_done_ = false;
    bool body_buffer_primed_ = false;
    size_t body_buffer_offset_ = 0;
    std::string body_buffer_;

    bool response_header_sent_ = false;
    bool response_complete_ = false;
    size_t response_body_sent_ = 0;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP1_EXCHANGE_IO_H
