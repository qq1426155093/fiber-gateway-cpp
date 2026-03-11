#ifndef FIBER_HTTP_HTTP_EXCHANGE_IO_H
#define FIBER_HTTP_HTTP_EXCHANGE_IO_H

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "../async/Task.h"
#include "../common/IoError.h"

namespace fiber::http {

class HttpExchange;
struct BodyChunk;

class HttpExchangeIo {
public:
    virtual ~HttpExchangeIo() = default;

    virtual fiber::async::Task<common::IoResult<BodyChunk>> read_body(HttpExchange &exchange,
                                                                      size_t max_bytes) noexcept = 0;
    virtual fiber::async::Task<common::IoResult<void>> send_response_header(HttpExchange &exchange, int status,
                                                                            std::string_view reason) = 0;
    virtual fiber::async::Task<common::IoResult<void>> finish_response(HttpExchange &exchange) noexcept = 0;
    virtual fiber::async::Task<common::IoResult<size_t>> write_body(HttpExchange &exchange,
                                                                    BodyChunk chunk) noexcept = 0;
    virtual fiber::async::Task<common::IoResult<size_t>> write_body(HttpExchange &exchange, const uint8_t *buf,
                                                                    size_t len, bool end) noexcept = 0;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP_EXCHANGE_IO_H
