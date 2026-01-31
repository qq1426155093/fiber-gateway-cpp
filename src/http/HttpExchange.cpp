#include "HttpExchange.h"

namespace fiber::http {

HttpExchange::HttpExchange(const HttpServerOptions &options) :request_headers_(pool_), response_headers_(pool_) {
    request_headers_.reserve_bytes(options.header_init_size);
    response_headers_.reserve_bytes(options.header_init_size);
}

std::string_view HttpExchange::header(std::string_view name) const noexcept { return request_headers_.get(name); }


fiber::async::Task<common::IoResult<ReadBodyResult>> HttpExchange::read_body(void *buf, size_t len) noexcept {
    (void) buf;
    (void) len;
    co_return std::unexpected(common::IoErr::NotSupported);
}

fiber::async::Task<common::IoResult<void>> HttpExchange::discard_body() noexcept {
    co_return std::unexpected(common::IoErr::NotSupported);
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

void HttpExchange::set_response_close() { response_close_ = true; }

fiber::async::Task<common::IoResult<void>> HttpExchange::send_response_header(int status, std::string_view reason) {
    (void) status;
    (void) reason;
    co_return std::unexpected(common::IoErr::NotSupported);
}

fiber::async::Task<common::IoResult<size_t>> HttpExchange::write_body(const void *buf, size_t len, bool end) noexcept {
    (void) buf;
    (void) len;
    (void) end;
    co_return std::unexpected(common::IoErr::NotSupported);
}

} // namespace fiber::http
