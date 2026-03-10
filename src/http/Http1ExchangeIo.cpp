#include "Http1ExchangeIo.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <limits>
#include <string>
#include <system_error>

#include "Http1Connection.h"
#include "HttpExchange.h"
#include "HttpTransport.h"

namespace fiber::http {

namespace {

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

Http1ExchangeIo::Http1ExchangeIo(Http1Connection &connection, const HttpExchange &exchange) : connection_(&connection) {
    if (exchange.request_chunked_) {
        request_body_done_ = false;
        return;
    }
    if (!exchange.request_content_length_set_ || exchange.request_content_length_ == 0) {
        request_body_done_ = true;
    }
}

fiber::async::Task<common::IoResult<size_t>> Http1ExchangeIo::read_more() noexcept {
    co_return co_await connection_->read_into_inbound(connection_->options().body_timeout);
}

fiber::async::Task<common::IoResult<std::string_view>> Http1ExchangeIo::read_line() noexcept {
    static constexpr size_t kMaxLineSize = 16 * 1024;

    line_buffer_.clear();
    for (;;) {
        mem::IoBuf *front = connection_->inbound_bufs().front();
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
        connection_->inbound_bufs().consume_and_compact(take);
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
        mem::IoBuf *front = connection_->inbound_bufs().front();
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
        connection_->inbound_bufs().consume_and_compact(1);
        break;
    }

    if (first == '\n') {
        co_return common::IoResult<void>{};
    }
    if (first != '\r') {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    for (;;) {
        mem::IoBuf *front = connection_->inbound_bufs().front();
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
        connection_->inbound_bufs().consume_and_compact(1);
        co_return common::IoResult<void>{};
    }
}

common::IoResult<void> Http1ExchangeIo::take_prefix(mem::IoBufChain &out, std::size_t len) noexcept {
    while (len > 0) {
        mem::IoBuf *front = connection_->inbound_bufs().front();
        if (!front || front->readable() == 0) {
            return std::unexpected(common::IoErr::Invalid);
        }
        std::size_t take = std::min(len, front->readable());
        mem::IoBuf piece = front->retain_slice(0, take);
        if (!out.append(std::move(piece))) {
            return std::unexpected(common::IoErr::NoMem);
        }
        connection_->inbound_bufs().consume_and_compact(take);
        len -= take;
    }
    return {};
}

fiber::async::Task<common::IoResult<ReadBodyChunk>> Http1ExchangeIo::read_body(HttpExchange &exchange,
                                                                               size_t max_bytes) noexcept {
    ReadBodyChunk out{};
    if (request_body_done_) {
        out.last = true;
        co_return out;
    }
    if (!connection_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    if (exchange.request_chunked_) {
        if (chunk_done_) {
            request_body_done_ = true;
            out.last = true;
            co_return out;
        }
        if (max_bytes == 0) {
            co_return out;
        }

        std::size_t remaining_budget = max_bytes;
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
                            out.last = true;
                            co_return out;
                        }
                    }
                }
                chunk_remaining_ = chunk_size;
            }

            while (connection_->inbound_bufs().readable_bytes() == 0) {
                auto more = co_await read_more();
                if (!more) {
                    co_return std::unexpected(more.error());
                }
                if (*more == 0) {
                    co_return std::unexpected(common::IoErr::ConnReset);
                }
            }

            std::size_t take =
                    std::min({remaining_budget, chunk_remaining_, connection_->inbound_bufs().readable_bytes()});
            auto take_result = take_prefix(out.data_chain, take);
            if (!take_result) {
                co_return std::unexpected(take_result.error());
            }
            chunk_remaining_ -= take;
            remaining_budget -= take;

            if (chunk_remaining_ == 0) {
                auto ending = co_await consume_chunk_ending();
                if (!ending) {
                    co_return std::unexpected(ending.error());
                }
            }

            if (remaining_budget == 0 || chunk_remaining_ > 0) {
                co_return out;
            }
        }
    }

    if (!exchange.request_content_length_set_) {
        request_body_done_ = true;
        out.last = true;
        co_return out;
    }
    if (request_body_read_ >= exchange.request_content_length_) {
        request_body_done_ = true;
        out.last = true;
        co_return out;
    }
    if (max_bytes == 0) {
        co_return out;
    }

    while (connection_->inbound_bufs().readable_bytes() == 0) {
        auto more = co_await read_more();
        if (!more) {
            co_return std::unexpected(more.error());
        }
        if (*more == 0) {
            co_return std::unexpected(common::IoErr::ConnReset);
        }
    }

    std::size_t remaining = exchange.request_content_length_ - request_body_read_;
    std::size_t take = std::min({max_bytes, remaining, connection_->inbound_bufs().readable_bytes()});
    auto take_result = take_prefix(out.data_chain, take);
    if (!take_result) {
        co_return std::unexpected(take_result.error());
    }
    request_body_read_ += take;
    if (request_body_read_ >= exchange.request_content_length_) {
        request_body_done_ = true;
        out.last = true;
    }
    co_return out;
}

fiber::async::Task<common::IoResult<void>> Http1ExchangeIo::send_response_header(HttpExchange &exchange, int status,
                                                                                 std::string_view reason) {
    if (!connection_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (response_header_sent_) {
        co_return std::unexpected(common::IoErr::Already);
    }

    bool close_conn = exchange.response_close_ || connection_->stopping();
    if (!close_conn) {
        if (!connection_->options().drain_unread_body && !request_body_done_) {
            close_conn = true;
        } else if (exchange.version_ == HttpVersion::HTTP_1_0) {
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

    auto result = co_await write_all(&connection_->transport(), header.data(), header.size(),
                                     connection_->options().write_timeout);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    response_header_sent_ = true;
    close_after_response_ = close_conn;
    if (close_conn) {
        exchange.response_close_ = true;
        connection_->request_connection_close();
    }
    co_return common::IoResult<void>{};
}

fiber::async::Task<common::IoResult<size_t>> Http1ExchangeIo::write_body(HttpExchange &exchange, const uint8_t *buf,
                                                                         size_t len, bool end) noexcept {
    if (len > 0 && buf == nullptr) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (!connection_) {
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
            auto res = co_await write_all(&connection_->transport(), size_view.data(), size_view.size(),
                                          connection_->options().write_timeout);
            if (!res) {
                co_return std::unexpected(res.error());
            }
            res = co_await write_all(&connection_->transport(), "\r\n", 2, connection_->options().write_timeout);
            if (!res) {
                co_return std::unexpected(res.error());
            }
            res = co_await write_all(&connection_->transport(), buf, len, connection_->options().write_timeout);
            if (!res) {
                co_return std::unexpected(res.error());
            }
            res = co_await write_all(&connection_->transport(), "\r\n", 2, connection_->options().write_timeout);
            if (!res) {
                co_return std::unexpected(res.error());
            }
        }
        response_body_sent_ += len;
        if (end) {
            auto res =
                    co_await write_all(&connection_->transport(), "0\r\n\r\n", 5, connection_->options().write_timeout);
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
        auto res = co_await write_all(&connection_->transport(), buf, len, connection_->options().write_timeout);
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
            close_after_response_ = true;
            connection_->request_connection_close();
        }
    }
    co_return len;
}

bool Http1ExchangeIo::should_keep_alive(const HttpExchange &) const noexcept {
    return response_header_sent_ && response_complete_ && !close_after_response_;
}

} // namespace fiber::http
