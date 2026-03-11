#include "Http1ExchangeIo.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <string>
#include <system_error>

#include "Http1Connection.h"
#include "HttpExchange.h"
#include "HttpTransport.h"

namespace fiber::http {

namespace {

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
        body_parser_.set_chunked();
        return;
    }
    if (!exchange.request_content_length_set_ || exchange.request_content_length_ == 0) {
        body_parser_.set_none();
        return;
    }
    body_parser_.set_content_length(exchange.request_content_length_);
}

fiber::async::Task<common::IoResult<size_t>> Http1ExchangeIo::read_more() noexcept {
    co_return co_await connection_->read_into_inbound(connection_->options().body_timeout);
}

fiber::async::Task<common::IoResult<ParseCode>> Http1ExchangeIo::advance_chunked_body() noexcept {
    for (;;) {
        connection_->inbound_bufs().drop_empty_front();
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

        mem::IoBuf cursor(*front);
        ParseCode code = body_parser_.execute(&cursor);
        std::size_t consumed = front->readable() - cursor.readable();
        if (consumed > 0) {
            connection_->inbound_bufs().consume_and_compact(consumed);
        }

        if (code == ParseCode::Again) {
            if (consumed == 0) {
                co_return std::unexpected(common::IoErr::Invalid);
            }
            continue;
        }
        if (code != ParseCode::Ok && code != ParseCode::Done) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
        co_return code;
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
    if (body_parser_.done()) {
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
        for (;;) {
            if (body_parser_.remaining() == 0) {
                auto parse_result = co_await advance_chunked_body();
                if (!parse_result) {
                    co_return std::unexpected(parse_result.error());
                }
                if (*parse_result == ParseCode::Done) {
                    out.last = true;
                    co_return out;
                }
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

            std::size_t take = std::min({remaining_budget, body_parser_.remaining(),
                                         connection_->inbound_bufs().readable_bytes()});
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

    while (connection_->inbound_bufs().readable_bytes() == 0) {
        auto more = co_await read_more();
        if (!more) {
            co_return std::unexpected(more.error());
        }
        if (*more == 0) {
            co_return std::unexpected(common::IoErr::ConnReset);
        }
    }

    std::size_t take = std::min({max_bytes, body_parser_.remaining(), connection_->inbound_bufs().readable_bytes()});
    auto take_result = take_prefix(out.data_chain, take);
    if (!take_result) {
        co_return std::unexpected(take_result.error());
    }
    body_parser_.consume(take);
    if (body_parser_.done()) {
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
        if (!connection_->options().drain_unread_body && !body_parser_.done()) {
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
        }
    }
    co_return len;
}

bool Http1ExchangeIo::should_keep_alive(const HttpExchange &) const noexcept {
    return response_header_sent_ && response_complete_ && !close_after_response_;
}

} // namespace fiber::http
