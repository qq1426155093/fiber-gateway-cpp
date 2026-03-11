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

constexpr std::size_t kMaxDirectBodyRead = 64 * 1024;
constexpr std::size_t kInlineFirstWriteBodyLimit = 128;

std::string_view default_reason_phrase(int status) noexcept {
    switch (status) {
        case 100:
            return "Continue";
        case 101:
            return "Switching Protocols";
        case 200:
            return "OK";
        case 201:
            return "Created";
        case 202:
            return "Accepted";
        case 204:
            return "No Content";
        case 301:
            return "Moved Permanently";
        case 302:
            return "Found";
        case 304:
            return "Not Modified";
        case 400:
            return "Bad Request";
        case 401:
            return "Unauthorized";
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 408:
            return "Request Timeout";
        case 409:
            return "Conflict";
        case 413:
            return "Payload Too Large";
        case 429:
            return "Too Many Requests";
        case 500:
            return "Internal Server Error";
        case 501:
            return "Not Implemented";
        case 502:
            return "Bad Gateway";
        case 503:
            return "Service Unavailable";
        case 504:
            return "Gateway Timeout";
        default:
            return "OK";
    }
}

std::size_t decimal_length(std::uint64_t value) noexcept {
    std::array<char, 32> buffer{};
    auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return ec == std::errc() ? static_cast<std::size_t>(ptr - buffer.data()) : 0;
}

std::size_t append_decimal(char *dst, std::uint64_t value) noexcept {
    std::array<char, 32> buffer{};
    auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (ec != std::errc()) {
        return 0;
    }
    std::size_t len = static_cast<std::size_t>(ptr - buffer.data());
    std::memcpy(dst, buffer.data(), len);
    return len;
}

std::size_t append_hex(char *dst, std::size_t value) noexcept {
    std::array<char, 32> buffer{};
    auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, 16);
    if (ec != std::errc()) {
        return 0;
    }
    std::size_t len = static_cast<std::size_t>(ptr - buffer.data());
    std::memcpy(dst, buffer.data(), len);
    return len;
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

fiber::async::Task<common::IoResult<void>> write_all(HttpTransport *transport, mem::IoBufChain &buf,
                                                     std::chrono::milliseconds timeout) {
    while (buf.readable_bytes() > 0) {
        auto result = co_await transport->writev(buf, timeout);
        if (!result) {
            co_return std::unexpected(result.error());
        }
        if (*result == 0) {
            co_return std::unexpected(common::IoErr::ConnReset);
        }
    }
    co_return common::IoResult<void>{};
}

std::size_t next_header_capacity(const HttpServerOptions &options, std::size_t current_capacity,
                                 std::size_t growth_count) noexcept {
    if (current_capacity == 0) {
        return options.header_init_size;
    }
    if (growth_count >= options.header_large_num) {
        return 0;
    }
    if (options.header_large_size > std::numeric_limits<std::size_t>::max() - current_capacity) {
        return 0;
    }
    return current_capacity + options.header_large_size;
}

common::IoResult<void> grow_header_buffer(const HttpServerOptions &options, mem::IoBuf &buffer,
                                          std::size_t &growth_count, HeaderLineParser &header_parser) noexcept {
    std::size_t next_capacity = next_header_capacity(options, buffer.capacity(), growth_count);
    if (next_capacity == 0) {
        return std::unexpected(common::IoErr::NoMem);
    }

    mem::IoBuf next = mem::IoBuf::allocate(next_capacity);
    if (!next) {
        return std::unexpected(common::IoErr::NoMem);
    }

    ParseCode code = header_parser.replace_buf_ptr(&buffer, &next);
    if (code != ParseCode::Ok) {
        return std::unexpected(code == ParseCode::HeaderTooLarge ? common::IoErr::Invalid : common::IoErr::NoMem);
    }

    buffer = std::move(next);
    ++growth_count;
    return {};
}

} // namespace

Http1ExchangeIo::Http1ExchangeIo(Http1Connection &connection, const HttpExchange &exchange) : connection_(&connection) {
    if (exchange.request_chunked_) {
        body_parser_.set_chunked();
        return;
    }
    if (!exchange.request_content_length_set_ || exchange.request_content_length_ == 0) {
        body_parser_.set_none();
        return;
    }
    body_parser_.set_content_length(exchange.request_content_length_);
}

common::IoResult<void> Http1ExchangeIo::ensure_read_buf_writable(std::size_t min_writable) noexcept {
    if (min_writable == 0) {
        return {};
    }

    if (!read_buf_) {
        read_buf_ = mem::IoBuf::allocate(min_writable);
        if (!read_buf_) {
            return std::unexpected(common::IoErr::NoMem);
        }
        return {};
    }

    if (read_buf_.readable() == 0) {
        if (read_buf_.unique() && read_buf_.capacity() >= min_writable) {
            read_buf_.reset();
            return {};
        }

        mem::IoBuf next = mem::IoBuf::allocate(min_writable);
        if (!next) {
            return std::unexpected(common::IoErr::NoMem);
        }
        read_buf_ = std::move(next);
        return {};
    }

    if (read_buf_.writable() >= min_writable) {
        return {};
    }

    std::size_t unread = read_buf_.readable();
    mem::IoBuf next = mem::IoBuf::allocate(unread + min_writable);
    if (!next) {
        return std::unexpected(common::IoErr::NoMem);
    }
    std::memcpy(next.writable_data(), read_buf_.readable_data(), unread);
    next.commit(unread);
    read_buf_ = std::move(next);
    return {};
}

std::size_t Http1ExchangeIo::drain_body_input(mem::IoBuf &buffer) noexcept {
    std::size_t copied = 0;
    while (buffer.writable() > 0) {
        connection_->inbound_bufs().drop_empty_front();
        mem::IoBuf *front = connection_->inbound_bufs().front();
        if (front && front->readable() > 0) {
            std::size_t take = std::min(front->readable(), buffer.writable());
            std::memcpy(buffer.writable_data(), front->readable_data(), take);
            buffer.commit(take);
            connection_->inbound_bufs().consume_and_compact(take);
            copied += take;
            continue;
        }

        if (read_buf_.readable() == 0) {
            break;
        }

        std::size_t take = std::min(read_buf_.readable(), buffer.writable());
        std::memcpy(buffer.writable_data(), read_buf_.readable_data(), take);
        buffer.commit(take);
        read_buf_.consume(take);
        copied += take;
    }
    return copied;
}

std::size_t Http1ExchangeIo::body_input_readable() const noexcept {
    return connection_->inbound_bufs().readable_bytes() + read_buf_.readable();
}

mem::IoBuf *Http1ExchangeIo::front_body_input() noexcept {
    connection_->inbound_bufs().drop_empty_front();
    mem::IoBuf *front = connection_->inbound_bufs().front();
    if (front && front->readable() > 0) {
        return front;
    }
    if (read_buf_.readable() > 0) {
        return &read_buf_;
    }
    return nullptr;
}

common::IoResult<void> Http1ExchangeIo::spill_read_buf_to_inbound() noexcept {
    if (read_buf_.readable() == 0) {
        return {};
    }

    mem::IoBuf trailing = read_buf_.retain_slice(0, read_buf_.readable());
    if (!connection_->inbound_bufs().append(std::move(trailing))) {
        return std::unexpected(common::IoErr::NoMem);
    }
    read_buf_.consume(read_buf_.readable());
    return {};
}

fiber::async::Task<common::IoResult<size_t>> Http1ExchangeIo::read_more(std::size_t max_bytes) noexcept {
    if (!connection_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    std::size_t read_size = std::min(max_bytes, kMaxDirectBodyRead);
    if (read_size == 0) {
        co_return static_cast<size_t>(0);
    }

    auto ensure_result = ensure_read_buf_writable(read_size);
    if (!ensure_result) {
        co_return std::unexpected(ensure_result.error());
    }

    auto result =
            co_await connection_->transport().read(read_buf_.writable_data(), read_size, connection_->options().body_timeout);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    read_call_used_io_ = true;
    read_buf_.commit(*result);
    co_return *result;
}

fiber::async::Task<common::IoResult<ParseCode>> Http1ExchangeIo::advance_chunked_body(std::size_t max_bytes,
                                                                                       bool allow_read) noexcept {
    for (;;) {
        mem::IoBuf *front = front_body_input();
        if (!front || front->readable() == 0) {
            if (!allow_read) {
                co_return ParseCode::Again;
            }
            auto more = co_await read_more(max_bytes);
            if (!more) {
                co_return std::unexpected(more.error());
            }
            if (*more == 0) {
                co_return std::unexpected(common::IoErr::ConnReset);
            }
            allow_read = false;
            continue;
        }

        mem::IoBuf cursor(*front);
        ParseCode code = body_parser_.execute(&cursor);
        std::size_t consumed = front->readable() - cursor.readable();
        if (consumed > 0) {
            if (front == &read_buf_) {
                read_buf_.consume(consumed);
            } else {
                connection_->inbound_bufs().consume_and_compact(consumed);
            }
        }

        if (code == ParseCode::Again) {
            if (consumed == 0) {
                co_return std::unexpected(common::IoErr::Invalid);
            }
            continue;
        }
        if (code != ParseCode::Ok && code != ParseCode::Done && code != ParseCode::BodyDone) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
        co_return code;
    }
}

common::IoResult<void> Http1ExchangeIo::take_prefix(mem::IoBufChain &out, std::size_t len) noexcept {
    while (len > 0) {
        connection_->inbound_bufs().drop_empty_front();
        mem::IoBuf *front = connection_->inbound_bufs().front();
        if (front && front->readable() > 0) {
            std::size_t take = std::min(len, front->readable());
            mem::IoBuf piece = front->retain_slice(0, take);
            if (!out.append(std::move(piece))) {
                return std::unexpected(common::IoErr::NoMem);
            }
            connection_->inbound_bufs().consume_and_compact(take);
            len -= take;
            continue;
        }

        if (read_buf_.readable() == 0) {
            return std::unexpected(common::IoErr::Invalid);
        }

        std::size_t take = std::min(len, read_buf_.readable());
        mem::IoBuf piece = read_buf_.retain_slice(0, take);
        if (!out.append(std::move(piece))) {
            return std::unexpected(common::IoErr::NoMem);
        }
        read_buf_.consume(take);
        len -= take;
    }
    return {};
}

fiber::async::Task<common::IoResult<BodyChunk>> Http1ExchangeIo::read_body(HttpExchange &exchange,
                                                                           size_t max_bytes) noexcept {
    BodyChunk out{};
    if (body_parser_.done()) {
        exchange.request_trailers_complete_ = true;
        out.last = true;
        co_return out;
    }
    if (!connection_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    if (body_parser_.type() == BodyParser::Type::Chunked) {
        if (max_bytes == 0) {
            co_return out;
        }

        std::size_t remaining_budget = max_bytes;
        read_call_used_io_ = false;
        for (;;) {
            if (body_parser_.remaining() == 0) {
                auto parse_result = co_await advance_chunked_body(remaining_budget, !read_call_used_io_);
                if (!parse_result) {
                    co_return std::unexpected(parse_result.error());
                }
                if (*parse_result == ParseCode::BodyDone) {
                    auto trailer_result = co_await read_request_trailers(exchange);
                    if (!trailer_result) {
                        co_return std::unexpected(trailer_result.error());
                    }
                    out.last = true;
                    co_return out;
                }
                if (*parse_result == ParseCode::Again) {
                    co_return out;
                }
            }

            if (body_input_readable() == 0) {
                if (read_call_used_io_) {
                    co_return out;
                }
                auto more = co_await read_more(std::min(remaining_budget, body_parser_.remaining()));
                if (!more) {
                    co_return std::unexpected(more.error());
                }
                if (*more == 0) {
                    co_return std::unexpected(common::IoErr::ConnReset);
                }
            }

            std::size_t take = std::min({remaining_budget, body_parser_.remaining(), body_input_readable()});
            auto take_result = take_prefix(out.data_chain, take);
            if (!take_result) {
                co_return std::unexpected(take_result.error());
            }
            body_parser_.consume(take);
            remaining_budget -= take;

            if (remaining_budget == 0 || body_parser_.remaining() > 0) {
                co_return out;
            }
        }
    }

    if (max_bytes == 0) {
        co_return out;
    }

    read_call_used_io_ = false;
    if (body_input_readable() == 0) {
        auto more = co_await read_more(std::min(max_bytes, body_parser_.remaining()));
        if (!more) {
            co_return std::unexpected(more.error());
        }
        if (*more == 0) {
            co_return std::unexpected(common::IoErr::ConnReset);
        }
    }

    std::size_t take = std::min({max_bytes, body_parser_.remaining(), body_input_readable()});
    auto take_result = take_prefix(out.data_chain, take);
    if (!take_result) {
        co_return std::unexpected(take_result.error());
    }
    body_parser_.consume(take);
    if (body_parser_.done()) {
        exchange.request_trailers_complete_ = true;
        out.last = true;
    }
    co_return out;
}

fiber::async::Task<common::IoResult<void>> Http1ExchangeIo::read_request_trailers(HttpExchange &exchange) noexcept {
    if (!connection_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    std::size_t growth_count = 0;
    mem::IoBuf parse_buf = mem::IoBuf::allocate(connection_->options().header_init_size);
    if (!parse_buf) {
        co_return std::unexpected(common::IoErr::NoMem);
    }

    HeaderLineParser parser(connection_->options());
    for (;;) {
        ParseCode code = parser.execute(&parse_buf);
        if (code == ParseCode::Again) {
            if (parse_buf.writable() == 0) {
                auto grow_result = grow_header_buffer(connection_->options(), parse_buf, growth_count, parser);
                if (!grow_result) {
                    co_return std::unexpected(grow_result.error());
                }
            }
            std::size_t copied = drain_body_input(parse_buf);
            if (copied == 0) {
                auto more = co_await connection_->transport().read_into(parse_buf, connection_->options().body_timeout);
                if (!more) {
                    co_return std::unexpected(more.error());
                }
                if (*more == 0) {
                    co_return std::unexpected(common::IoErr::ConnReset);
                }
            }
            continue;
        }

        if (code == ParseCode::Ok) {
            const auto &line = parser.state();
            if (!line.header_name_start || !line.header_name_end || line.header_name_end < line.header_name_start) {
                co_return std::unexpected(common::IoErr::Invalid);
            }

            std::size_t name_len = static_cast<std::size_t>(line.header_name_end - line.header_name_start);
            std::string_view name(reinterpret_cast<char *>(line.header_name_start), name_len);
            std::string_view value;
            if (line.header_start && line.header_end && line.header_end >= line.header_start) {
                std::size_t value_len = static_cast<std::size_t>(line.header_end - line.header_start);
                value = std::string_view(reinterpret_cast<char *>(line.header_start), value_len);
            }

            if (!exchange.request_trailers_.add(name, value)) {
                co_return std::unexpected(common::IoErr::NoMem);
            }
            continue;
        }

        if (code == ParseCode::HeaderDone) {
            if (parse_buf.readable() > 0) {
                mem::IoBuf trailing = parse_buf.retain_slice(0, parse_buf.readable());
                if (!connection_->inbound_bufs().append(std::move(trailing))) {
                    co_return std::unexpected(common::IoErr::NoMem);
                }
            }
            auto spill_result = spill_read_buf_to_inbound();
            if (!spill_result) {
                co_return std::unexpected(spill_result.error());
            }
            exchange.request_trailers_complete_ = true;
            body_parser_.finish_chunked_trailers();
            co_return common::IoResult<void>{};
        }

        co_return std::unexpected(common::IoErr::Invalid);
    }
}

common::IoResult<mem::IoBuf> Http1ExchangeIo::build_response_header(HttpExchange &exchange, bool body_end,
                                                                    std::size_t first_body_len,
                                                                    bool infer_body_mode,
                                                                    bool &close_conn) noexcept {
    if (infer_body_mode && exchange.response_body_mode_ == ResponseBodyMode::Auto) {
        if (body_end) {
            exchange.response_body_mode_ = ResponseBodyMode::ContentLength;
            exchange.response_content_length_ = first_body_len;
        } else {
            exchange.response_body_mode_ = ResponseBodyMode::Chunked;
        }
    }

    if (exchange.response_body_mode_ == ResponseBodyMode::Auto) {
        exchange.response_body_mode_ = ResponseBodyMode::ContentLength;
        exchange.response_content_length_ = 0;
    }

    close_conn = exchange.response_connection_mode_ == ResponseConnectionMode::Close || connection_->stopping();
    if (!close_conn) {
        if (!connection_->options().drain_unread_body && !body_parser_.done()) {
            close_conn = true;
        } else if (exchange.version_ == HttpVersion::HTTP_1_0) {
            close_conn = !exchange.request_keep_alive_;
        } else {
            close_conn = exchange.request_close_;
        }
    }

    std::string_view reason = exchange.response_reason_.empty() ? default_reason_phrase(exchange.response_status_code_)
                                                                : std::string_view(exchange.response_reason_);
    constexpr std::string_view kHttp10 = "HTTP/1.0 ";
    constexpr std::string_view kHttp11 = "HTTP/1.1 ";
    constexpr std::string_view kCrLf = "\r\n";
    constexpr std::string_view kContentLength = "Content-Length: ";
    constexpr std::string_view kTransferEncoding = "Transfer-Encoding: chunked\r\n";
    constexpr std::string_view kConnectionClose = "Connection: close\r\n";
    std::string_view version = exchange.version_ == HttpVersion::HTTP_1_0 ? kHttp10 : kHttp11;

    bool write_content_length = exchange.response_body_mode_ == ResponseBodyMode::ContentLength &&
                                !exchange.response_headers_.contains("Content-Length");
    bool write_transfer_encoding = exchange.response_body_mode_ == ResponseBodyMode::Chunked &&
                                   !exchange.response_headers_.contains("Transfer-Encoding");
    bool write_connection_close = close_conn && !exchange.response_headers_.contains("Connection");

    std::size_t header_len = version.size();
    header_len += decimal_length(static_cast<std::uint64_t>(exchange.response_status_code_));
    header_len += 1 + reason.size() + kCrLf.size();
    if (write_content_length) {
        header_len += kContentLength.size();
        header_len += decimal_length(exchange.response_content_length_);
        header_len += kCrLf.size();
    }
    if (write_transfer_encoding) {
        header_len += kTransferEncoding.size();
    }
    if (write_connection_close) {
        header_len += kConnectionClose.size();
    }
    for (auto it = exchange.response_headers_.begin(); it != exchange.response_headers_.end(); ++it) {
        const auto &field = *it;
        if (field.name_len == 0) {
            continue;
        }
        header_len += static_cast<std::size_t>(field.name_len) + 2 + static_cast<std::size_t>(field.value_len) +
                      kCrLf.size();
    }
    header_len += kCrLf.size();

    mem::IoBuf header = mem::IoBuf::allocate(header_len);
    if (!header) {
        return std::unexpected(common::IoErr::NoMem);
    }

    char *out = reinterpret_cast<char *>(header.writable_data());
    std::memcpy(out, version.data(), version.size());
    out += version.size();
    out += append_decimal(out, static_cast<std::uint64_t>(exchange.response_status_code_));
    *out++ = ' ';
    std::memcpy(out, reason.data(), reason.size());
    out += reason.size();
    std::memcpy(out, kCrLf.data(), kCrLf.size());
    out += kCrLf.size();

    if (write_content_length) {
        std::memcpy(out, kContentLength.data(), kContentLength.size());
        out += kContentLength.size();
        out += append_decimal(out, exchange.response_content_length_);
        std::memcpy(out, kCrLf.data(), kCrLf.size());
        out += kCrLf.size();
    }
    if (write_transfer_encoding) {
        std::memcpy(out, kTransferEncoding.data(), kTransferEncoding.size());
        out += kTransferEncoding.size();
    }
    if (write_connection_close) {
        std::memcpy(out, kConnectionClose.data(), kConnectionClose.size());
        out += kConnectionClose.size();
    }

    for (auto it = exchange.response_headers_.begin(); it != exchange.response_headers_.end(); ++it) {
        const auto &field = *it;
        if (field.name_len == 0) {
            continue;
        }
        std::memcpy(out, field.name, field.name_len);
        out += field.name_len;
        *out++ = ':';
        *out++ = ' ';
        std::memcpy(out, field.value, field.value_len);
        out += field.value_len;
        std::memcpy(out, kCrLf.data(), kCrLf.size());
        out += kCrLf.size();
    }
    std::memcpy(out, kCrLf.data(), kCrLf.size());
    out += kCrLf.size();
    header.commit(static_cast<std::size_t>(out - reinterpret_cast<char *>(header.writable_data())));

    return header;
}

fiber::async::Task<common::IoResult<void>> Http1ExchangeIo::write_response_header(HttpExchange &exchange, bool body_end,
                                                                                  std::size_t first_body_len,
                                                                                  bool infer_body_mode) noexcept {
    if (!connection_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (response_phase_ != ResponsePhase::Init) {
        co_return std::unexpected(common::IoErr::Already);
    }

    if (infer_body_mode && exchange.response_body_mode_ == ResponseBodyMode::Auto) {
        if (body_end) {
            exchange.response_body_mode_ = ResponseBodyMode::ContentLength;
            exchange.response_content_length_ = first_body_len;
        } else {
            exchange.response_body_mode_ = ResponseBodyMode::Chunked;
        }
    }

    if (exchange.response_body_mode_ == ResponseBodyMode::Auto) {
        exchange.response_body_mode_ = ResponseBodyMode::ContentLength;
        exchange.response_content_length_ = 0;
    }

    bool close_conn = exchange.response_connection_mode_ == ResponseConnectionMode::Close || connection_->stopping();
    if (!close_conn) {
        if (!connection_->options().drain_unread_body && !body_parser_.done()) {
            close_conn = true;
        } else if (exchange.version_ == HttpVersion::HTTP_1_0) {
            close_conn = !exchange.request_keep_alive_;
        } else {
            close_conn = exchange.request_close_;
        }
    }

    std::string_view reason = exchange.response_reason_.empty() ? default_reason_phrase(exchange.response_status_code_)
                                                                : std::string_view(exchange.response_reason_);
    std::string header;
    header.reserve(128 + exchange.response_headers_.size() * 32);
    if (exchange.version_ == HttpVersion::HTTP_1_0) {
        header.append("HTTP/1.0 ");
    } else {
        header.append("HTTP/1.1 ");
    }
    header.append(std::to_string(exchange.response_status_code_));
    header.push_back(' ');
    header.append(reason);
    header.append("\r\n");

    if (exchange.response_body_mode_ == ResponseBodyMode::ContentLength &&
        !exchange.response_headers_.contains("Content-Length")) {
        header.append("Content-Length: ");
        header.append(std::to_string(exchange.response_content_length_));
        header.append("\r\n");
    }
    if (exchange.response_body_mode_ == ResponseBodyMode::Chunked &&
        !exchange.response_headers_.contains("Transfer-Encoding")) {
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
    response_phase_ = ResponsePhase::HeaderSent;
    close_after_response_ = close_conn;
    if (close_conn) {
        exchange.response_connection_mode_ = ResponseConnectionMode::Close;
    }
    co_return common::IoResult<void>{};
}

fiber::async::Task<common::IoResult<void>> Http1ExchangeIo::send_response_header(HttpExchange &exchange) {
    co_return co_await write_response_header(exchange, true, 0, false);
}

fiber::async::Task<common::IoResult<void>> Http1ExchangeIo::write_chunked_trailer_block(HttpExchange &exchange) noexcept {
    std::string block;
    block.reserve(exchange.response_trailers_.size() * 32 + 2);
    for (auto it = exchange.response_trailers_.begin(); it != exchange.response_trailers_.end(); ++it) {
        const auto &field = *it;
        if (field.name_len == 0) {
            continue;
        }
        block.append(field.name_view());
        block.append(": ");
        block.append(field.value_view());
        block.append("\r\n");
    }
    block.append("\r\n");

    auto result = co_await write_all(&connection_->transport(), block.data(), block.size(),
                                     connection_->options().write_timeout);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    co_return common::IoResult<void>{};
}

fiber::async::Task<common::IoResult<void>> Http1ExchangeIo::finish_response(HttpExchange &exchange) noexcept {
    if (!connection_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (response_phase_ == ResponsePhase::Init) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (response_phase_ == ResponsePhase::Finished) {
        co_return std::unexpected(common::IoErr::Already);
    }

    if (exchange.response_body_mode_ == ResponseBodyMode::Chunked) {
        auto zero_result = co_await write_all(&connection_->transport(), "0\r\n", 3, connection_->options().write_timeout);
        if (!zero_result) {
            co_return std::unexpected(zero_result.error());
        }
        auto trailer_result = co_await write_chunked_trailer_block(exchange);
        if (!trailer_result) {
            co_return std::unexpected(trailer_result.error());
        }
        response_phase_ = ResponsePhase::Finished;
        co_return common::IoResult<void>{};
    }

    if (exchange.response_trailers_.size() > 0) {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    if (exchange.response_body_mode_ == ResponseBodyMode::ContentLength &&
        response_body_sent_ != exchange.response_content_length_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    response_phase_ = ResponsePhase::Finished;
    co_return common::IoResult<void>{};
}

fiber::async::Task<common::IoResult<size_t>> Http1ExchangeIo::write_body(HttpExchange &exchange,
                                                                         BodyChunk chunk) noexcept {
    if (!connection_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (response_phase_ == ResponsePhase::Finished) {
        co_return std::unexpected(common::IoErr::Already);
    }

    size_t len = chunk.data_chain.readable_bytes();
    if (response_phase_ == ResponsePhase::Init) {
        bool close_conn = false;
        auto header_result = build_response_header(exchange, chunk.last, len, true, close_conn);
        if (!header_result) {
            co_return std::unexpected(header_result.error());
        }

        if (exchange.response_body_mode_ == ResponseBodyMode::ContentLength) {
            size_t remaining = exchange.response_content_length_ - response_body_sent_;
            if (len > remaining) {
                co_return std::unexpected(common::IoErr::Invalid);
            }
        }

        if (exchange.response_body_mode_ == ResponseBodyMode::Chunked && len > 0) {
            std::size_t prefix_len = decimal_length(0);
            {
                std::array<char, 32> buffer{};
                prefix_len = append_hex(buffer.data(), len);
            }

            mem::IoBuf prefix = mem::IoBuf::allocate(prefix_len + 2);
            if (!prefix) {
                co_return std::unexpected(common::IoErr::NoMem);
            }
            char *prefix_out = reinterpret_cast<char *>(prefix.writable_data());
            prefix_out += append_hex(prefix_out, len);
            *prefix_out++ = '\r';
            *prefix_out++ = '\n';
            prefix.commit(static_cast<std::size_t>(prefix_out - reinterpret_cast<char *>(prefix.writable_data())));

            mem::IoBuf suffix = mem::IoBuf::allocate(2);
            if (!suffix) {
                co_return std::unexpected(common::IoErr::NoMem);
            }
            char *suffix_out = reinterpret_cast<char *>(suffix.writable_data());
            *suffix_out++ = '\r';
            *suffix_out++ = '\n';
            suffix.commit(2);

            if (!chunk.data_chain.append(std::move(suffix)) || !chunk.data_chain.prepend(std::move(prefix))) {
                co_return std::unexpected(common::IoErr::NoMem);
            }
        }
        if (!chunk.data_chain.prepend(std::move(*header_result))) {
            co_return std::unexpected(common::IoErr::NoMem);
        }

        auto res = co_await write_all(&connection_->transport(), chunk.data_chain, connection_->options().write_timeout);
        if (!res) {
            co_return std::unexpected(res.error());
        }

        close_after_response_ = close_conn;
        if (close_conn) {
            exchange.response_connection_mode_ = ResponseConnectionMode::Close;
        }
        response_body_sent_ += len;
        response_phase_ = len > 0 ? ResponsePhase::BodyStreaming : ResponsePhase::HeaderSent;
        if (chunk.last) {
            auto finish_result = co_await finish_response(exchange);
            if (!finish_result) {
                co_return std::unexpected(finish_result.error());
            }
        }
        co_return len;
    }

    if (exchange.response_body_mode_ == ResponseBodyMode::Chunked) {
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
            res = co_await write_all(&connection_->transport(), chunk.data_chain, connection_->options().write_timeout);
            if (!res) {
                co_return std::unexpected(res.error());
            }
            res = co_await write_all(&connection_->transport(), "\r\n", 2, connection_->options().write_timeout);
            if (!res) {
                co_return std::unexpected(res.error());
            }
        }
        response_body_sent_ += len;
        if (len > 0) {
            response_phase_ = ResponsePhase::BodyStreaming;
        }
        if (chunk.last) {
            auto finish_result = co_await finish_response(exchange);
            if (!finish_result) {
                co_return std::unexpected(finish_result.error());
            }
        }
        co_return len;
    }

    if (exchange.response_body_mode_ == ResponseBodyMode::ContentLength) {
        size_t remaining = exchange.response_content_length_ - response_body_sent_;
        if (len > remaining) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
    }
    if (len > 0) {
        auto res = co_await write_all(&connection_->transport(), chunk.data_chain, connection_->options().write_timeout);
        if (!res) {
            co_return std::unexpected(res.error());
        }
    }
    response_body_sent_ += len;
    if (len > 0) {
        response_phase_ = ResponsePhase::BodyStreaming;
    }
    if (chunk.last) {
        auto finish_result = co_await finish_response(exchange);
        if (!finish_result) {
            co_return std::unexpected(finish_result.error());
        }
    }
    co_return len;
}

fiber::async::Task<common::IoResult<size_t>> Http1ExchangeIo::write_body(HttpExchange &exchange, const uint8_t *buf,
                                                                         size_t len, bool end) noexcept {
    if (len > 0 && buf == nullptr) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (!connection_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (response_phase_ == ResponsePhase::Finished) {
        co_return std::unexpected(common::IoErr::Already);
    }

    if (response_phase_ == ResponsePhase::Init) {
        bool close_conn = false;
        auto header_result = build_response_header(exchange, end, len, true, close_conn);
        if (!header_result) {
            co_return std::unexpected(header_result.error());
        }

        if (exchange.response_body_mode_ == ResponseBodyMode::ContentLength) {
            size_t remaining = exchange.response_content_length_ - response_body_sent_;
            if (len > remaining) {
                co_return std::unexpected(common::IoErr::Invalid);
            }
        }

        if (len <= kInlineFirstWriteBodyLimit) {
            std::size_t combined_len = header_result->readable() + len;
            if (exchange.response_body_mode_ == ResponseBodyMode::Chunked && len > 0) {
                combined_len += 32 + 4;
            }

            mem::IoBuf combined = mem::IoBuf::allocate(combined_len);
            if (!combined) {
                co_return std::unexpected(common::IoErr::NoMem);
            }

            char *out = reinterpret_cast<char *>(combined.writable_data());
            std::memcpy(out, header_result->readable_data(), header_result->readable());
            out += header_result->readable();

            if (exchange.response_body_mode_ == ResponseBodyMode::Chunked && len > 0) {
                out += append_hex(out, len);
                *out++ = '\r';
                *out++ = '\n';
                std::memcpy(out, buf, len);
                out += len;
                *out++ = '\r';
                *out++ = '\n';
            } else if (len > 0) {
                std::memcpy(out, buf, len);
                out += len;
            }

            combined.commit(static_cast<std::size_t>(out - reinterpret_cast<char *>(combined.writable_data())));
            auto res = co_await write_all(&connection_->transport(), combined.readable_data(), combined.readable(),
                                          connection_->options().write_timeout);
            if (!res) {
                co_return std::unexpected(res.error());
            }
        } else {
            auto res = co_await write_all(&connection_->transport(), header_result->readable_data(),
                                          header_result->readable(), connection_->options().write_timeout);
            if (!res) {
                co_return std::unexpected(res.error());
            }

            if (exchange.response_body_mode_ == ResponseBodyMode::Chunked && len > 0) {
                std::array<char, 32> size_buf{};
                std::size_t size_len = append_hex(size_buf.data(), len);
                res = co_await write_all(&connection_->transport(), size_buf.data(), size_len,
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
            } else if (len > 0) {
                res = co_await write_all(&connection_->transport(), buf, len, connection_->options().write_timeout);
                if (!res) {
                    co_return std::unexpected(res.error());
                }
            }
        }

        close_after_response_ = close_conn;
        if (close_conn) {
            exchange.response_connection_mode_ = ResponseConnectionMode::Close;
        }
        response_body_sent_ += len;
        response_phase_ = len > 0 ? ResponsePhase::BodyStreaming : ResponsePhase::HeaderSent;
        if (end) {
            auto finish_result = co_await finish_response(exchange);
            if (!finish_result) {
                co_return std::unexpected(finish_result.error());
            }
        }
        co_return len;
    }

    if (exchange.response_body_mode_ == ResponseBodyMode::Chunked) {
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
        if (len > 0) {
            response_phase_ = ResponsePhase::BodyStreaming;
        }
        if (end) {
            auto finish_result = co_await finish_response(exchange);
            if (!finish_result) {
                co_return std::unexpected(finish_result.error());
            }
        }
        co_return len;
    }

    if (exchange.response_body_mode_ == ResponseBodyMode::ContentLength) {
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
    if (len > 0) {
        response_phase_ = ResponsePhase::BodyStreaming;
    }
    if (end) {
        auto finish_result = co_await finish_response(exchange);
        if (!finish_result) {
            co_return std::unexpected(finish_result.error());
        }
    }
    co_return len;
}

bool Http1ExchangeIo::should_keep_alive(const HttpExchange &) const noexcept {
    return response_phase_ == ResponsePhase::Finished && !close_after_response_;
}

} // namespace fiber::http
