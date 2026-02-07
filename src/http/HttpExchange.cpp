#include "HttpExchange.h"

#include <array>
#include <utility>

#include "HttpExchangeIo.h"

namespace fiber::http {

HttpExchange::HttpExchange(const HttpServerOptions &options) : request_headers_(pool_), response_headers_(pool_) {
    request_headers_.reserve_bytes(options.header_init_size);
    response_headers_.reserve_bytes(options.header_init_size);
}

HttpExchange::~HttpExchange() = default;

std::string_view HttpExchange::header(std::string_view name) const noexcept {
    return request_headers_.get(name);
}

void HttpExchange::set_io(std::unique_ptr<HttpExchangeIo> io) noexcept {
    io_ = std::move(io);
}

fiber::async::Task<common::IoResult<ReadBodyResult>> HttpExchange::read_body(void *buf, size_t len) noexcept {
    if (!io_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return co_await io_->read_body(*this, buf, len);
}

fiber::async::Task<common::IoResult<void>> HttpExchange::discard_body() noexcept {
    std::array<char, 4096> buf{};
    for (;;) {
        auto result = co_await read_body(buf.data(), buf.size());
        if (!result) {
            co_return std::unexpected(result.error());
        }
        if (result->end) {
            break;
        }
    }
    co_return common::IoResult<void>{};
}

void HttpExchange::set_response_header(std::string_view name, std::string_view value) {
    response_headers_.set(name, value);
}

void HttpExchange::set_response_content_length(size_t len) {
    response_content_length_set_ = true;
    response_content_length_ = len;
    response_chunked_ = false;
}

void HttpExchange::set_response_chunked() {
    response_chunked_ = true;
    response_content_length_set_ = false;
}

void HttpExchange::set_response_close() {
    response_close_ = true;
}

fiber::async::Task<common::IoResult<void>> HttpExchange::send_response_header(int status, std::string_view reason) {
    if (!io_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return co_await io_->send_response_header(*this, status, reason);
}

fiber::async::Task<common::IoResult<size_t>> HttpExchange::write_body(const uint8_t *buf,
                                                                       size_t len,
                                                                       bool end) noexcept {
    if (!io_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return co_await io_->write_body(*this, buf, len, end);
}

} // namespace fiber::http
