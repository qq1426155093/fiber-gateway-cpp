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

fiber::async::Task<common::IoResult<void>> write_all(HttpTransport *transport,
                                                     const void *buf,
                                                     size_t len,
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

Http1ExchangeIo::Http1ExchangeIo(HttpTransport &transport,
                                 const HttpServerOptions &options,
                                 BufChain *header_adjacent_body)
    : transport_(&transport), options_(&options), header_adjacent_body_(header_adjacent_body) {
}

fiber::async::Task<common::IoResult<ReadBodyResult>> Http1ExchangeIo::read_body(HttpExchange &exchange,
                                                                                  void *buf,
                                                                                  size_t len) noexcept {
    ReadBodyResult out{};
    if (request_body_done_) {
        out.end = true;
        co_return out;
    }
    if (!transport_ || !options_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (!body_buffer_primed_) {
        for (auto *node = header_adjacent_body_; node; node = node->next) {
            size_t readable = node->readable();
            if (readable > 0) {
                body_buffer_.append(reinterpret_cast<const char *>(node->pos), readable);
                node->pos = node->last;
            }
        }
        header_adjacent_body_ = nullptr;
        body_buffer_offset_ = 0;
        body_buffer_primed_ = true;
    }

    auto available = [&]() -> size_t {
        return body_buffer_.size() > body_buffer_offset_ ? body_buffer_.size() - body_buffer_offset_ : 0;
    };
    auto data_ptr = [&]() -> const char * {
        return body_buffer_.data() + body_buffer_offset_;
    };
    auto consume = [&]() {
        if (body_buffer_offset_ >= body_buffer_.size()) {
            body_buffer_.clear();
            body_buffer_offset_ = 0;
            return;
        }
        if (body_buffer_offset_ > 4096 && body_buffer_offset_ > body_buffer_.size() / 2) {
            body_buffer_.erase(0, body_buffer_offset_);
            body_buffer_offset_ = 0;
        }
    };
    auto consume_bytes = [&](size_t n) {
        body_buffer_offset_ += n;
        consume();
    };

    auto read_more = [&]() -> fiber::async::Task<common::IoResult<size_t>> {
        std::array<char, kBodyReadChunk> tmp{};
        auto result = co_await transport_->read(tmp.data(), tmp.size(), options_->body_timeout);
        if (!result) {
            co_return std::unexpected(result.error());
        }
        if (*result == 0) {
            co_return static_cast<size_t>(0);
        }
        body_buffer_.append(tmp.data(), *result);
        co_return *result;
    };

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
                for (;;) {
                    auto view = std::string_view(data_ptr(), available());
                    auto pos = view.find("\r\n");
                    if (pos == std::string_view::npos) {
                        auto more = co_await read_more();
                        if (!more) {
                            co_return std::unexpected(more.error());
                        }
                        if (*more == 0) {
                            co_return std::unexpected(common::IoErr::ConnReset);
                        }
                        continue;
                    }
                    std::string_view line = view.substr(0, pos);
                    auto semi = line.find(';');
                    if (semi != std::string_view::npos) {
                        line = line.substr(0, semi);
                    }
                    size_t chunk_size = 0;
                    if (!parse_hex_size(line, chunk_size)) {
                        co_return std::unexpected(common::IoErr::Invalid);
                    }
                    consume_bytes(pos + 2);
                    if (chunk_size == 0) {
                        for (;;) {
                            auto tail_view = std::string_view(data_ptr(), available());
                            auto end_pos = tail_view.find("\r\n");
                            if (end_pos == std::string_view::npos) {
                                auto more = co_await read_more();
                                if (!more) {
                                    co_return std::unexpected(more.error());
                                }
                                if (*more == 0) {
                                    co_return std::unexpected(common::IoErr::ConnReset);
                                }
                                continue;
                            }
                            consume_bytes(end_pos + 2);
                            if (end_pos == 0) {
                                chunk_done_ = true;
                                request_body_done_ = true;
                                out.end = true;
                                co_return out;
                            }
                        }
                    }
                    chunk_remaining_ = chunk_size;
                    break;
                }
            }

            if (chunk_remaining_ == 0) {
                continue;
            }
            if (available() == 0) {
                auto more = co_await read_more();
                if (!more) {
                    co_return std::unexpected(more.error());
                }
                if (*more == 0) {
                    co_return std::unexpected(common::IoErr::ConnReset);
                }
                continue;
            }
            size_t take = std::min({len, chunk_remaining_, available()});
            std::memcpy(buf, data_ptr(), take);
            consume_bytes(take);
            chunk_remaining_ -= take;
            out.size = take;
            if (chunk_remaining_ == 0) {
                for (;;) {
                    if (available() >= 2) {
                        auto view = std::string_view(data_ptr(), available());
                        if (view[0] != '\r' || view[1] != '\n') {
                            co_return std::unexpected(common::IoErr::Invalid);
                        }
                        consume_bytes(2);
                        break;
                    }
                    auto more = co_await read_more();
                    if (!more) {
                        co_return std::unexpected(more.error());
                    }
                    if (*more == 0) {
                        co_return std::unexpected(common::IoErr::ConnReset);
                    }
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
    size_t remaining = exchange.request_content_length_ - request_body_read_;
    size_t want = std::min(len, remaining);
    size_t copied = 0;
    if (want == 0) {
        out.end = request_body_read_ >= exchange.request_content_length_;
        co_return out;
    }
    size_t avail = available();
    if (avail > 0) {
        size_t take = std::min(avail, want);
        std::memcpy(buf, data_ptr(), take);
        consume_bytes(take);
        copied += take;
    }
    while (copied < want) {
        auto result = co_await transport_->read(static_cast<char *>(buf) + copied,
                                                want - copied,
                                                options_->body_timeout);
        if (!result) {
            co_return std::unexpected(result.error());
        }
        if (*result == 0) {
            co_return std::unexpected(common::IoErr::ConnReset);
        }
        copied += *result;
    }
    request_body_read_ += copied;
    out.size = copied;
    if (request_body_read_ >= exchange.request_content_length_) {
        request_body_done_ = true;
        out.end = true;
    }
    co_return out;
}

fiber::async::Task<common::IoResult<void>> Http1ExchangeIo::send_response_header(HttpExchange &exchange,
                                                                                   int status,
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

fiber::async::Task<common::IoResult<size_t>> Http1ExchangeIo::write_body(HttpExchange &exchange,
                                                                          const uint8_t *buf,
                                                                          size_t len,
                                                                          bool end) noexcept {
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
