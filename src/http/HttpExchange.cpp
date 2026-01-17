#include "HttpExchange.h"

#include <algorithm>

#include "Http1Connection.h"

namespace fiber::http {

HttpExchange::HttpExchange(Http1Connection &connection, const HttpServerOptions &options)
    : connection_(&connection), options_(&options) {
}

void HttpExchange::reset() {
    method_.clear();
    target_.clear();
    version_.clear();
    request_headers_.clear();
    request_chunked_ = false;
    request_expect_continue_ = false;
    request_keep_alive_ = true;
    request_content_length_ = 0;
    request_content_length_set_ = false;

    response_headers_.clear();
    response_chunked_ = false;
    response_header_sent_ = false;
    response_complete_ = false;
    response_close_ = false;
    response_content_length_set_ = false;
    response_content_length_ = 0;
    response_body_sent_ = 0;

    body_buffer_.clear();
    body_complete_ = false;
    continue_sent_ = false;
}

std::string_view HttpExchange::method() const noexcept {
    return method_;
}

std::string_view HttpExchange::target() const noexcept {
    return target_;
}

std::string_view HttpExchange::version() const noexcept {
    return version_;
}

std::string_view HttpExchange::header(std::string_view name) const noexcept {
    std::string lower;
    lower.reserve(name.size());
    for (char ch : name) {
        if (ch >= 'A' && ch <= 'Z') {
            lower.push_back(static_cast<char>(ch - 'A' + 'a'));
        } else {
            lower.push_back(ch);
        }
    }
    for (const auto &header : request_headers_) {
        if (header.name == lower) {
            return header.value;
        }
    }
    return {};
}

bool HttpExchange::request_chunked() const noexcept {
    return request_chunked_;
}

size_t HttpExchange::request_content_length() const noexcept {
    return request_content_length_;
}

HttpTask<common::IoResult<ReadBodyResult>> HttpExchange::read_body(void *buf, size_t len) noexcept {
    co_return co_await connection_->read_body(*this, buf, len);
}

HttpTask<common::IoResult<void>> HttpExchange::discard_body() noexcept {
    co_return co_await connection_->discard_body(*this);
}

void HttpExchange::set_response_header(std::string_view name, std::string_view value) {
    response_headers_.push_back(HttpHeader{std::string(name), std::string(value)});
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

HttpTask<common::IoResult<void>> HttpExchange::send_response_header(int status,
                                                                    std::string_view reason) {
    co_return co_await connection_->send_response_header(*this, status, reason);
}

HttpTask<common::IoResult<size_t>> HttpExchange::write_body(const void *buf,
                                                            size_t len,
                                                            bool end) noexcept {
    co_return co_await connection_->write_body(*this, buf, len, end);
}

} // namespace fiber::http
