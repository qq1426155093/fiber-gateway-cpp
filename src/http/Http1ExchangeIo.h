#ifndef FIBER_HTTP_HTTP1_EXCHANGE_IO_H
#define FIBER_HTTP_HTTP1_EXCHANGE_IO_H

#include <string>

#include "../common/mem/IoBuf.h"
#include "HttpExchangeIo.h"
#include "Http1Parser.h"

namespace fiber::http {

class HttpExchange;
class Http1Connection;

enum class ResponsePhase : std::uint8_t {
    Init,
    HeaderSent,
    BodyStreaming,
    Finished,
};

class Http1ExchangeIo final : public HttpExchangeIo {
public:
    Http1ExchangeIo(Http1Connection &connection, const HttpExchange &exchange);

    fiber::async::Task<common::IoResult<BodyChunk>> read_body(HttpExchange &exchange,
                                                              size_t max_bytes) noexcept override;
    fiber::async::Task<common::IoResult<void>> send_response_header(HttpExchange &exchange, int status,
                                                                    std::string_view reason) override;
    fiber::async::Task<common::IoResult<void>> finish_response(HttpExchange &exchange) noexcept override;
    fiber::async::Task<common::IoResult<size_t>> write_body(HttpExchange &exchange, BodyChunk chunk) noexcept override;
    fiber::async::Task<common::IoResult<size_t>> write_body(HttpExchange &exchange, const uint8_t *buf, size_t len,
                                                            bool end) noexcept override;

    [[nodiscard]] bool request_body_complete() const noexcept { return body_parser_.done(); }
    [[nodiscard]] bool response_complete() const noexcept { return response_phase_ == ResponsePhase::Finished; }
    [[nodiscard]] bool should_keep_alive(const HttpExchange &exchange) const noexcept;

private:
    fiber::async::Task<common::IoResult<size_t>> read_more(std::size_t max_bytes) noexcept;
    fiber::async::Task<common::IoResult<ParseCode>> advance_chunked_body(std::size_t max_bytes,
                                                                         bool allow_read) noexcept;
    fiber::async::Task<common::IoResult<void>> read_request_trailers(HttpExchange &exchange) noexcept;
    fiber::async::Task<common::IoResult<void>> write_chunked_trailer_block(HttpExchange &exchange) noexcept;
    common::IoResult<void> ensure_read_buf_writable(std::size_t min_writable) noexcept;
    std::size_t drain_body_input(mem::IoBuf &buffer) noexcept;
    common::IoResult<void> take_prefix(mem::IoBufChain &out, std::size_t len) noexcept;
    common::IoResult<void> spill_read_buf_to_inbound() noexcept;
    [[nodiscard]] mem::IoBuf *front_body_input() noexcept;
    [[nodiscard]] std::size_t body_input_readable() const noexcept;

    Http1Connection *connection_ = nullptr;
    BodyParser body_parser_;
    mem::IoBuf read_buf_;
    bool read_call_used_io_ = false;

    ResponsePhase response_phase_ = ResponsePhase::Init;
    size_t response_body_sent_ = 0;
    bool close_after_response_ = false;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP1_EXCHANGE_IO_H
