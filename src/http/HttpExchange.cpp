#include "HttpExchange.h"

#include "Http1Connection.h"

namespace fiber::http {

HttpExchange::HttpExchange(Http1Connection &connection, const HttpServerOptions &options) :
    connection_(&connection),  request_headers_(pool_), response_headers_(pool_) {
    request_headers_.reserve_bytes(options.max_header_bytes);
    response_headers_.reserve_bytes(options.max_header_bytes);
}

std::string_view HttpExchange::header(std::string_view name) const noexcept { return request_headers_.get(name); }

const HttpHeaders &HttpExchange::request_headers() const noexcept { return request_headers_; }

HttpHeaders &HttpExchange::response_headers() noexcept { return response_headers_; }

mem::BufPool &HttpExchange::pool() noexcept { return pool_; }

fiber::async::Task<common::IoResult<ReadBodyResult>> HttpExchange::read_body(void *buf, size_t len) noexcept {
    co_return co_await connection_->read_body(*this, buf, len);
}

fiber::async::Task<common::IoResult<void>> HttpExchange::discard_body() noexcept {
    co_return co_await connection_->discard_body(*this);
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
    co_return co_await connection_->send_response_header(*this, status, reason);
}

fiber::async::Task<common::IoResult<size_t>> HttpExchange::write_body(const void *buf, size_t len, bool end) noexcept {
    co_return co_await connection_->write_body(*this, buf, len, end);
}

} // namespace fiber::http
