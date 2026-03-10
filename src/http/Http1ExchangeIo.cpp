#include "Http1ExchangeIo.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <limits>
#include <string>
#include <system_error>

#include "HttpExchange.h"
#include "HttpTransport.h"

namespace fiber::http {

namespace {

constexpr size_t kBodyReadChunk = 4096;

std::string_view trim_lws(std::string_view value) {
    while (!value.empty()) {
        char ch = value.front();
        if (ch != ' ' && ch != '\t') {
            break;
        }
        value.remove_prefix(1);
    }
    while (!value.empty()) {
        char ch = value.back();
        if (ch != ' ' && ch != '\t') {
            break;
        }
        value.remove_suffix(1);
    }
    return value;
}

bool parse_hex_size(std::string_view value, size_t &out) {
    value = trim_lws(value);
    if (value.empty()) {
        return false;
    }
    if (value.front() == '+') {
        value.remove_prefix(1);
    }
    if (value.empty()) {
        return false;
    }
    unsigned long long parsed = 0;
    auto result = std::from_chars(value.data(), value.data() + value.size(), parsed, 16);
    if (result.ec != std::errc() || result.ptr != value.data() + value.size()) {
        return false;
    }
    if (parsed > static_cast<unsigned long long>(std::numeric_limits<size_t>::max())) {
        return false;
    }
    out = static_cast<size_t>(parsed);
    return true;
}

fiber::async::Task<common::IoResult<void>> write_all(HttpTransport *transport, const void *buf, size_t len,
                                                     std::chrono::milliseconds timeout) {
    const auto *ptr = static_cast<const std::uint8_t *>(buf);
    size_t remaining = len;
    while (remaining > 0) {
        auto result = co_await transport->write(ptr, remaining, timeout);
        if (!result) {
            co_return std::unexpected(result.error());
        }
        if (*result == 0) {
            co_return std::unexpected(common::IoErr::ConnReset);
        }
        ptr += *result;
        remaining -= *result;
    }
    co_return common::IoResult<void>{};
}

} // namespace

Http1ExchangeIo::Http1ExchangeIo(HttpTransport &transport, const HttpServerOptions &options,
                                 mem::IoBufChain &&body_bufs) :
    transport_(&transport), options_(&options), body_bufs_(std::move(body_bufs)) {}

fiber::async::Task<common::IoResult<size_t>> Http1ExchangeIo::read_more() noexcept {
    mem::IoBuf buf = mem::IoBuf::allocate(kBodyReadChunk);
    if (!buf) {
        co_return std::unexpected(common::IoErr::NoMem);
    }
    auto result = co_await transport_->read_into(buf, options_->body_timeout);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    if (*result == 0) {
        co_return static_cast<size_t>(0);
    }
    if (!body_bufs_.append(std::move(buf))) {
        co_return std::unexpected(common::IoErr::NoMem);
    }
    co_return *result;
}

fiber::async::Task<common::IoResult<std::string_view>> Http1ExchangeIo::read_line() noexcept {
    static constexpr size_t kMaxLineSize = 16 * 1024;

    line_buffer_.clear();
    for (;;) {
        mem::IoBuf *front = body_bufs_.front();
        if (!front || front->readable() == 0) {
            auto more = co_await read_more();
            if (!more) {
                co_return std::unexpected(more.error());
            }
            if (*more == 0) {
                co_return std::unexpected(common::IoErr::ConnReset);
            }
            continue;
        }

        const char *data = reinterpret_cast<const char *>(front->readable_data());
        size_t readable = front->readable();
        const void *newline = std::memchr(data, '\n', readable);
        size_t take = newline ? static_cast<size_t>(static_cast<const char *>(newline) - data + 1) : readable;
        if (line_buffer_.size() + take > kMaxLineSize) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
        line_buffer_.append(data, take);
        body_bufs_.consume_and_compact(take);
        if (!newline) {
            continue;
        }

        if (line_buffer_.empty() || line_buffer_.back() != '\n') {
            co_return std::unexpected(common::IoErr::Invalid);
        }
        line_buffer_.pop_back();
        if (!line_buffer_.empty() && line_buffer_.back() == '\r') {
            line_buffer_.pop_back();
        }
        co_return std::string_view(line_buffer_);
    }
}

fiber::async::Task<common::IoResult<void>> Http1ExchangeIo::consume_chunk_ending() noexcept {
    char first = 0;
    for (;;) {
        mem::IoBuf *front = body_bufs_.front();
        if (!front || front->readable() == 0) {
            auto more = co_await read_more();
            if (!more) {
                co_return std::unexpected(more.error());
            }
            if (*more == 0) {
                co_return std::unexpected(common::IoErr::ConnReset);
            }
            continue;
        }
        first = static_cast<char>(*front->readable_data());
        body_bufs_.consume_and_compact(1);
        break;
    }

    if (first == '\n') {
        co_return common::IoResult<void>{};
    }
    if (first != '\r') {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    for (;;) {
        mem::IoBuf *front = body_bufs_.front();
        if (!front || front->readable() == 0) {
            auto more = co_await read_more();
            if (!more) {
                co_return std::unexpected(more.error());
            }
            if (*more == 0) {
                co_return std::unexpected(common::IoErr::ConnReset);
            }
            continue;
        }
        if (*front->readable_data() != '\n') {
            co_return std::unexpected(common::IoErr::Invalid);
        }
        body_bufs_.consume_and_compact(1);
        co_return common::IoResult<void>{};
    }
}

size_t Http1ExchangeIo::copy_available(void *buf, size_t len) noexcept {
    if (len == 0 || !buf) {
        return 0;
    }

    size_t copied = 0;
    auto *out = static_cast<std::uint8_t *>(buf);
    while (copied < len) {
        mem::IoBuf *front = body_bufs_.front();
        if (!front || front->readable() == 0) {
            break;
        }
        size_t take = std::min(len - copied, front->readable());
        std::memcpy(out + copied, front->readable_data(), take);
        body_bufs_.consume_and_compact(take);
        copied += take;
    }
    return copied;
}

fiber::async::Task<common::IoResult<ReadBodyResult>> Http1ExchangeIo::read_body(HttpExchange &exchange, void *buf,
                                                                                size_t len) noexcept {
    ReadBodyResult out{};
    if (len > 0 && buf == nullptr) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (request_body_done_) {
        out.end = true;
        co_return out;
    }
    if (!transport_ || !options_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    if (exchange.request_chunked_) {
        if (chunk_done_) {
            out.end = true;
            co_return out;
        }
        if (len == 0) {
            co_return out;
        }

        for (;;) {
            if (chunk_remaining_ == 0) {
                auto line_result = co_await read_line();
                if (!line_result) {
                    co_return std::unexpected(line_result.error());
                }
                std::string_view line = *line_result;
                size_t semi = line.find(';');
                if (semi != std::string_view::npos) {
                    line = line.substr(0, semi);
                }
                size_t chunk_size = 0;
                if (!parse_hex_size(line, chunk_size)) {
                    co_return std::unexpected(common::IoErr::Invalid);
                }
                if (chunk_size == 0) {
                    for (;;) {
                        auto trailer = co_await read_line();
                        if (!trailer) {
                            co_return std::unexpected(trailer.error());
                        }
                        if (trailer->empty()) {
                            chunk_done_ = true;
                            request_body_done_ = true;
                            out.end = true;
                            co_return out;
                        }
                    }
                }
                chunk_remaining_ = chunk_size;
            }

            while (body_bufs_.readable_bytes() == 0) {
                auto more = co_await read_more();
                if (!more) {
                    co_return std::unexpected(more.error());
                }
                if (*more == 0) {
                    co_return std::unexpected(common::IoErr::ConnReset);
                }
            }

            size_t take = std::min({len, chunk_remaining_, body_bufs_.readable_bytes()});
            out.size = copy_available(buf, take);
            chunk_remaining_ -= out.size;
            if (chunk_remaining_ == 0) {
                auto ending = co_await consume_chunk_ending();
                if (!ending) {
                    co_return std::unexpected(ending.error());
                }
            }
            co_return out;
        }
    }

    if (!exchange.request_content_length_set_) {
        request_body_done_ = true;
        out.end = true;
        co_return out;
    }
    if (request_body_read_ >= exchange.request_content_length_) {
        request_body_done_ = true;
        out.end = true;
        co_return out;
    }

    while (body_bufs_.readable_bytes() == 0) {
        auto more = co_await read_more();
        if (!more) {
            co_return std::unexpected(more.error());
        }
        if (*more == 0) {
            co_return std::unexpected(common::IoErr::ConnReset);
        }
    }

    size_t remaining = exchange.request_content_length_ - request_body_read_;
    size_t take = std::min(len, remaining);
    out.size = copy_available(buf, take);
    request_body_read_ += out.size;
    if (request_body_read_ >= exchange.request_content_length_) {
        request_body_done_ = true;
        out.end = true;
    }
    co_return out;
}

fiber::async::Task<common::IoResult<void>> Http1ExchangeIo::send_response_header(HttpExchange &exchange, int status,
                                                                                 std::string_view reason) {
    if (!transport_ || !options_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (response_header_sent_) {
        co_return std::unexpected(common::IoErr::Already);
    }

    bool close_conn = exchange.response_close_;
    if (!close_conn) {
        if (exchange.version_ == HttpVersion::HTTP_1_0) {
            close_conn = !exchange.request_keep_alive_;
        } else {
            close_conn = exchange.request_close_;
        }
    }

    if (!exchange.response_content_length_set_ && !exchange.response_chunked_) {
        exchange.response_content_length_set_ = true;
        exchange.response_content_length_ = 0;
    }

    std::string header;
    header.reserve(128 + exchange.response_headers_.size() * 32);
    if (exchange.version_ == HttpVersion::HTTP_1_0) {
        header.append("HTTP/1.0 ");
    } else {
        header.append("HTTP/1.1 ");
    }
    header.append(std::to_string(status));
    header.push_back(' ');
    header.append(reason.empty() ? "OK" : reason);
    header.append("\r\n");

    if (exchange.response_content_length_set_ && !exchange.response_headers_.contains("Content-Length")) {
        header.append("Content-Length: ");
        header.append(std::to_string(exchange.response_content_length_));
        header.append("\r\n");
    }
    if (exchange.response_chunked_ && !exchange.response_headers_.contains("Transfer-Encoding")) {
        header.append("Transfer-Encoding: chunked\r\n");
    }
    if (close_conn && !exchange.response_headers_.contains("Connection")) {
        header.append("Connection: close\r\n");
    }

    for (auto it = exchange.response_headers_.begin(); it != exchange.response_headers_.end(); ++it) {
        const auto &field = *it;
        if (field.name_len == 0) {
            continue;
        }
        header.append(field.name_view());
        header.append(": ");
        header.append(field.value_view());
        header.append("\r\n");
    }
    header.append("\r\n");

    auto result = co_await write_all(transport_, header.data(), header.size(), options_->write_timeout);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    response_header_sent_ = true;
    if (close_conn) {
        exchange.response_close_ = true;
    }
    co_return common::IoResult<void>{};
}

fiber::async::Task<common::IoResult<size_t>> Http1ExchangeIo::write_body(HttpExchange &exchange, const uint8_t *buf,
                                                                         size_t len, bool end) noexcept {
    if (len > 0 && buf == nullptr) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (!transport_ || !options_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (!response_header_sent_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (response_complete_) {
        co_return std::unexpected(common::IoErr::Already);
    }

    if (exchange.response_chunked_) {
        if (len > 0) {
            std::array<char, 32> size_buf{};
            auto [ptr, ec] = std::to_chars(size_buf.data(), size_buf.data() + size_buf.size(), len, 16);
            if (ec != std::errc()) {
                co_return std::unexpected(common::IoErr::Invalid);
            }
            std::string_view size_view(size_buf.data(), static_cast<size_t>(ptr - size_buf.data()));
            auto res = co_await write_all(transport_, size_view.data(), size_view.size(), options_->write_timeout);
            if (!res) {
                co_return std::unexpected(res.error());
            }
            res = co_await write_all(transport_, "\r\n", 2, options_->write_timeout);
            if (!res) {
                co_return std::unexpected(res.error());
            }
            res = co_await write_all(transport_, buf, len, options_->write_timeout);
            if (!res) {
                co_return std::unexpected(res.error());
            }
            res = co_await write_all(transport_, "\r\n", 2, options_->write_timeout);
            if (!res) {
                co_return std::unexpected(res.error());
            }
        }
        response_body_sent_ += len;
        if (end) {
            auto res = co_await write_all(transport_, "0\r\n\r\n", 5, options_->write_timeout);
            if (!res) {
                co_return std::unexpected(res.error());
            }
            response_complete_ = true;
        }
        co_return len;
    }

    if (exchange.response_content_length_set_) {
        size_t remaining = exchange.response_content_length_ - response_body_sent_;
        if (len > remaining) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
    }
    if (len > 0) {
        auto res = co_await write_all(transport_, buf, len, options_->write_timeout);
        if (!res) {
            co_return std::unexpected(res.error());
        }
    }
    response_body_sent_ += len;
    if (end) {
        if (exchange.response_content_length_set_ && response_body_sent_ != exchange.response_content_length_) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
        response_complete_ = true;
        if (!exchange.response_content_length_set_ && !exchange.response_chunked_) {
            exchange.response_close_ = true;
        }
    }
    co_return len;
}

} // namespace fiber::http
