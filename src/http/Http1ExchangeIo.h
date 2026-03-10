#ifndef FIBER_HTTP_HTTP1_EXCHANGE_IO_H
#define FIBER_HTTP_HTTP1_EXCHANGE_IO_H

#include <string>

#include "../common/mem/IoBuf.h"
#include "HttpExchangeIo.h"

namespace fiber::http {

class HttpExchange;
class Http1Connection;

class Http1ExchangeIo final : public HttpExchangeIo {
public:
    Http1ExchangeIo(Http1Connection &connection, const HttpExchange &exchange);

    fiber::async::Task<common::IoResult<ReadBodyChunk>> read_body(HttpExchange &exchange,
                                                                  size_t max_bytes) noexcept override;
    fiber::async::Task<common::IoResult<void>> send_response_header(HttpExchange &exchange, int status,
                                                                    std::string_view reason) override;
    fiber::async::Task<common::IoResult<size_t>> write_body(HttpExchange &exchange, const uint8_t *buf, size_t len,
                                                            bool end) noexcept override;

    [[nodiscard]] bool request_body_complete() const noexcept { return request_body_done_; }
    [[nodiscard]] bool response_complete() const noexcept { return response_complete_; }
    [[nodiscard]] bool should_keep_alive(const HttpExchange &exchange) const noexcept;

private:
    fiber::async::Task<common::IoResult<size_t>> read_more() noexcept;
    fiber::async::Task<common::IoResult<std::string_view>> read_line() noexcept;
    fiber::async::Task<common::IoResult<void>> consume_chunk_ending() noexcept;
    common::IoResult<void> take_prefix(mem::IoBufChain &out, std::size_t len) noexcept;

    Http1Connection *connection_ = nullptr;

    bool request_body_done_ = false;
    size_t request_body_read_ = 0;
    size_t chunk_remaining_ = 0;
    bool chunk_done_ = false;
    std::string line_buffer_;

    bool response_header_sent_ = false;
    bool response_complete_ = false;
    size_t response_body_sent_ = 0;
    bool close_after_response_ = false;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP1_EXCHANGE_IO_H
