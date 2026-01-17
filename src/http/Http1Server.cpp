#include "Http1Server.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <memory>

#include "../async/Spawn.h"
#include "../common/Assert.h"
#include "../net/TcpStream.h"
#include "Http1Connection.h"
#include "HttpTransport.h"
#include "TlsContext.h"

namespace fiber::http {

namespace {

int status_for_parse_error(HttpParseError error) {
    switch (error) {
    case HttpParseError::HeadersTooLarge:
        return 431;
    case HttpParseError::BodyTooLarge:
    case HttpParseError::ChunkTooLarge:
        return 413;
    case HttpParseError::UnsupportedTransferEncoding:
        return 501;
    case HttpParseError::BadRequest:
    case HttpParseError::None:
    default:
        return 400;
    }
}

fiber::async::DetachedTask run_connection(std::unique_ptr<Http1Connection> connection) {
    if (connection) {
        co_await connection->run();
    }
    co_return;
}

} // namespace

Http1Connection::Http1Connection(std::unique_ptr<HttpTransport> transport,
                                 const HttpServerOptions &options,
                                 HttpHandler handler)
    : transport_(std::move(transport)),
      options_(options),
      handler_(std::move(handler)),
      exchange_(*this, options_) {
}

HttpTask<void> Http1Connection::run() {
    if (transport_) {
        auto handshake_result = co_await transport_->handshake(options_.tls.handshake_timeout);
        if (!handshake_result) {
            transport_->close();
            closed_ = true;
            co_return;
        }
    }

    bool first_request = true;
    while (!closed_) {
        exchange_.reset();
        parser_.reset(exchange_, options_);
        parsing_body_ = false;

        std::chrono::seconds header_timeout =
            first_request ? options_.header_timeout : options_.keep_alive_timeout;
        first_request = false;

        for (;;) {
            if (recv_offset_ >= recv_buffer_.size()) {
                recv_buffer_.clear();
                recv_offset_ = 0;
                auto read_result = co_await read_from_stream(header_timeout);
                if (!read_result) {
                    closed_ = true;
                    transport_->close();
                    co_return;
                }
                if (*read_result == 0) {
                    closed_ = true;
                    transport_->close();
                    co_return;
                }
            }

            const char *data = recv_buffer_.data() + recv_offset_;
            size_t len = recv_buffer_.size() - recv_offset_;
            auto parse_result = parser_.execute(data, len);
            consume_buffer(parse_result.consumed);
            if (parse_result.state == HttpParseState::HeadersComplete) {
                break;
            }
            if (parse_result.state == HttpParseState::Error) {
                int status = status_for_parse_error(parse_result.error);
                exchange_.set_response_close();
                exchange_.set_response_content_length(0);
                co_await send_response_header(exchange_, status, "");
                co_await transport_->shutdown(options_.write_timeout);
                transport_->close();
                closed_ = true;
                co_return;
            }
        }

        if (!exchange_.request_keep_alive_) {
            exchange_.response_close_ = true;
        }

        co_await handler_(exchange_);

        if (!exchange_.response_header_sent_) {
            exchange_.set_response_close();
            exchange_.set_response_content_length(0);
            co_await send_response_header(exchange_, 500, "Internal Server Error");
        }

        if (!exchange_.body_complete_) {
            if (options_.drain_unread_body) {
                auto drain_result = co_await drain_body(exchange_);
                if (!drain_result) {
                    exchange_.response_close_ = true;
                }
            } else {
                exchange_.response_close_ = true;
            }
        }

        if (!exchange_.response_complete_) {
            if (exchange_.response_chunked_) {
                co_await write_body(exchange_, nullptr, 0, true);
            } else if (exchange_.response_content_length_set_ &&
                       exchange_.response_body_sent_ == exchange_.response_content_length_) {
                exchange_.response_complete_ = true;
            } else {
                exchange_.response_close_ = true;
            }
        }

        if (exchange_.response_close_ || !exchange_.request_keep_alive_) {
            co_await transport_->shutdown(options_.write_timeout);
            transport_->close();
            closed_ = true;
            co_return;
        }
    }
    co_return;
}

HttpTask<common::IoResult<ReadBodyResult>> Http1Connection::read_body(HttpExchange &exchange,
                                                                      void *buf,
                                                                      size_t len) {
    if (exchange.body_complete_ && exchange.body_buffer_.empty()) {
        co_return ReadBodyResult{0, true};
    }
    if (!exchange.body_buffer_.empty()) {
        size_t to_copy = std::min(len, exchange.body_buffer_.size());
        std::memcpy(buf, exchange.body_buffer_.data(), to_copy);
        exchange.body_buffer_.erase(0, to_copy);
        co_return ReadBodyResult{to_copy, exchange.body_complete_ && exchange.body_buffer_.empty()};
    }

    auto cont_result = co_await send_continue_if_needed(exchange);
    if (!cont_result) {
        co_return std::unexpected(cont_result.error());
    }

    if (!parsing_body_) {
        parser_.resume();
        parsing_body_ = true;
    }

    for (;;) {
        if (recv_offset_ >= recv_buffer_.size()) {
            recv_buffer_.clear();
            recv_offset_ = 0;
            auto read_result = co_await read_from_stream(options_.body_timeout);
            if (!read_result) {
                co_return std::unexpected(read_result.error());
            }
            if (*read_result == 0) {
                co_return std::unexpected(common::IoErr::ConnReset);
            }
        }

        const char *data = recv_buffer_.data() + recv_offset_;
        size_t data_len = recv_buffer_.size() - recv_offset_;
        auto parse_result = parser_.execute(data, data_len);
        consume_buffer(parse_result.consumed);
        if (parse_result.state == HttpParseState::Error) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
        if (!exchange.body_buffer_.empty() || exchange.body_complete_) {
            break;
        }
    }

    if (!exchange.body_buffer_.empty()) {
        size_t to_copy = std::min(len, exchange.body_buffer_.size());
        std::memcpy(buf, exchange.body_buffer_.data(), to_copy);
        exchange.body_buffer_.erase(0, to_copy);
        co_return ReadBodyResult{to_copy, exchange.body_complete_ && exchange.body_buffer_.empty()};
    }
    if (exchange.body_complete_) {
        co_return ReadBodyResult{0, true};
    }
    co_return ReadBodyResult{0, false};
}

HttpTask<common::IoResult<void>> Http1Connection::discard_body(HttpExchange &exchange) {
    std::array<char, 4096> buffer{};
    for (;;) {
        auto result = co_await read_body(exchange, buffer.data(), buffer.size());
        if (!result) {
            co_return std::unexpected(result.error());
        }
        if (result->end) {
            break;
        }
    }
    co_return common::IoResult<void>{};
}

HttpTask<common::IoResult<void>> Http1Connection::send_response_header(HttpExchange &exchange,
                                                                       int status,
                                                                       std::string_view reason) {
    if (exchange.response_header_sent_) {
        co_return std::unexpected(common::IoErr::Already);
    }

    auto defaults = ensure_header_defaults(exchange);
    if (!defaults) {
        co_return std::unexpected(defaults.error());
    }

    std::string status_reason;
    if (reason.empty()) {
        status_reason = default_reason(status);
    } else {
        status_reason = std::string(reason);
    }

    std::string response;
    response.reserve(256 + exchange.response_headers_.size() * 32);
    std::string_view version = exchange.version_.empty()
        ? std::string_view("HTTP/1.1")
        : std::string_view(exchange.version_);
    response.append(version);
    response.push_back(' ');
    response.append(std::to_string(status));
    response.push_back(' ');
    response.append(status_reason);
    response.append("\r\n");

    auto has_header = [&](std::string_view name) {
        return exchange.response_headers_.contains(name);
    };

    for (const auto &header : exchange.response_headers_) {
        response.append(header.name_view());
        response.append(": ");
        response.append(header.value_view());
        response.append("\r\n");
    }

    if (exchange.response_chunked_ && !has_header("transfer-encoding")) {
        response.append("Transfer-Encoding: chunked\r\n");
    }
    if (exchange.response_content_length_set_ && !has_header("content-length")) {
        response.append("Content-Length: ");
        response.append(std::to_string(exchange.response_content_length_));
        response.append("\r\n");
    }
    if (exchange.response_close_ && !has_header("connection")) {
        response.append("Connection: close\r\n");
    } else if (!exchange.response_close_ && !has_header("connection") &&
               exchange.version_ == "HTTP/1.0") {
        response.append("Connection: keep-alive\r\n");
    }

    response.append("\r\n");

    auto write_result = co_await write_all(response.data(), response.size());
    if (!write_result) {
        co_return std::unexpected(write_result.error());
    }
    exchange.response_header_sent_ = true;
    co_return common::IoResult<void>{};
}

HttpTask<common::IoResult<size_t>> Http1Connection::write_body(HttpExchange &exchange,
                                                               const void *buf,
                                                               size_t len,
                                                               bool end) {
    if (!exchange.response_header_sent_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (exchange.response_complete_) {
        co_return std::unexpected(common::IoErr::Already);
    }

    if (exchange.response_chunked_) {
        if (len > 0) {
            std::array<char, 32> chunk_header{};
            auto [ptr, ec] = std::to_chars(chunk_header.data(),
                                           chunk_header.data() + chunk_header.size(),
                                           len,
                                           16);
            if (ec != std::errc()) {
                co_return std::unexpected(common::IoErr::Invalid);
            }
            size_t header_len = static_cast<size_t>(ptr - chunk_header.data());
            std::string header_line(chunk_header.data(), header_len);
            header_line.append("\r\n");
            auto header_result = co_await write_all(header_line.data(), header_line.size());
            if (!header_result) {
                co_return std::unexpected(header_result.error());
            }
            auto data_result = co_await write_all(buf, len);
            if (!data_result) {
                co_return std::unexpected(data_result.error());
            }
            auto tail_result = co_await write_all("\r\n", 2);
            if (!tail_result) {
                co_return std::unexpected(tail_result.error());
            }
        }
        if (end) {
            auto end_result = co_await write_all("0\r\n\r\n", 5);
            if (!end_result) {
                co_return std::unexpected(end_result.error());
            }
            exchange.response_complete_ = true;
        }
        co_return len;
    }

    if (!exchange.response_content_length_set_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    if (len > 0) {
        auto write_result = co_await write_all(buf, len);
        if (!write_result) {
            co_return std::unexpected(write_result.error());
        }
        exchange.response_body_sent_ += len;
    }

    auto finalize = finalize_response_body(exchange, end);
    if (!finalize) {
        co_return std::unexpected(finalize.error());
    }
    co_return len;
}

common::IoResult<void> Http1Connection::ensure_header_defaults(HttpExchange &exchange) {
    if (exchange.response_chunked_ && exchange.response_content_length_set_) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (!exchange.response_chunked_ && !exchange.response_content_length_set_) {
        if (exchange.version_ == "HTTP/1.0") {
            exchange.response_close_ = true;
        } else {
            exchange.response_chunked_ = true;
        }
    }
    return {};
}

HttpTask<common::IoResult<void>> Http1Connection::write_all(const void *data, size_t len) {
    const char *ptr = static_cast<const char *>(data);
    size_t remaining = len;
    while (remaining > 0) {
        auto result = co_await transport_->write(ptr, remaining, options_.write_timeout);
        if (!result) {
            co_return std::unexpected(result.error());
        }
        if (*result == 0) {
            co_return std::unexpected(common::IoErr::BrokenPipe);
        }
        ptr += *result;
        remaining -= *result;
    }
    co_return common::IoResult<void>{};
}

HttpTask<common::IoResult<void>> Http1Connection::send_continue_if_needed(HttpExchange &exchange) {
    if (!exchange.request_expect_continue_ || exchange.continue_sent_ ||
        !options_.auto_100_continue) {
        co_return common::IoResult<void>{};
    }
    auto result = co_await write_all("HTTP/1.1 100 Continue\r\n\r\n", 25);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    exchange.continue_sent_ = true;
    co_return common::IoResult<void>{};
}

HttpTask<common::IoResult<void>> Http1Connection::drain_body(HttpExchange &exchange) {
    auto result = co_await discard_body(exchange);
    if (!result) {
        co_return std::unexpected(result.error());
    }
    co_return common::IoResult<void>{};
}

common::IoResult<void> Http1Connection::finalize_response_body(HttpExchange &exchange, bool end) {
    if (!exchange.response_content_length_set_) {
        return {};
    }
    if (exchange.response_body_sent_ > exchange.response_content_length_) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (end) {
        if (exchange.response_body_sent_ != exchange.response_content_length_) {
            return std::unexpected(common::IoErr::Invalid);
        }
        exchange.response_complete_ = true;
    }
    return {};
}

std::string Http1Connection::default_reason(int status) {
    switch (status) {
    case 200:
        return "OK";
    case 201:
        return "Created";
    case 204:
        return "No Content";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 413:
        return "Payload Too Large";
    case 431:
        return "Request Header Fields Too Large";
    case 500:
        return "Internal Server Error";
    case 501:
        return "Not Implemented";
    default:
        return "OK";
    }
}

HttpTask<common::IoResult<size_t>> Http1Connection::read_from_stream(std::chrono::seconds timeout) {
    std::array<char, 8192> buffer{};
    auto read_result = co_await transport_->read(buffer.data(), buffer.size(), timeout);
    if (!read_result) {
        co_return std::unexpected(read_result.error());
    }
    if (*read_result > 0) {
        recv_buffer_.append(buffer.data(), *read_result);
    }
    co_return *read_result;
}

void Http1Connection::consume_buffer(size_t len) {
    recv_offset_ += len;
    if (recv_offset_ >= recv_buffer_.size()) {
        recv_buffer_.clear();
        recv_offset_ = 0;
        return;
    }
    if (recv_offset_ > 4096) {
        recv_buffer_.erase(0, recv_offset_);
        recv_offset_ = 0;
    }
}

Http1Server::Http1Server(event::EventLoop &loop,
                         HttpHandler handler,
                         HttpServerOptions options)
    : loop_(loop),
      listener_(loop),
      handler_(std::move(handler)),
      options_(options) {
}

Http1Server::~Http1Server() {
}

common::IoResult<void> Http1Server::bind(const net::SocketAddress &addr,
                                         const net::ListenOptions &options) {
    if (options_.tls.enabled) {
        if (!tls_context_) {
            tls_context_ = std::make_unique<TlsContext>(options_.tls);
        }
        auto tls_result = tls_context_->init();
        if (!tls_result) {
            return std::unexpected(tls_result.error());
        }
    }
    return listener_.bind(addr, options);
}

bool Http1Server::valid() const noexcept {
    return listener_.valid();
}

int Http1Server::fd() const noexcept {
    return listener_.fd();
}

void Http1Server::close() {
    listener_.close();
}

fiber::async::DetachedTask Http1Server::serve() {
    for (;;) {
        auto accept_result = co_await listener_.accept();
        if (!accept_result) {
            auto err = accept_result.error();
            if (err == common::IoErr::BadFd || err == common::IoErr::Canceled) {
                break;
            }
            continue;
        }
        if (accept_result->fd < 0) {
            continue;
        }
        auto stream = std::make_unique<net::TcpStream>(loop_,
                                                       accept_result->fd,
                                                       accept_result->peer);
        std::unique_ptr<HttpTransport> transport;
        if (options_.tls.enabled) {
            if (!tls_context_) {
                stream->close();
                continue;
            }
            auto tls_transport = TlsTransport::create(std::move(stream), *tls_context_);
            if (!tls_transport) {
                continue;
            }
            transport = std::move(*tls_transport);
        } else {
            transport = std::make_unique<TcpTransport>(std::move(stream));
        }

        auto connection = std::make_unique<Http1Connection>(std::move(transport),
                                                            options_,
                                                            handler_);
        fiber::async::spawn(loop_, [connection = std::move(connection)]() mutable {
            return run_connection(std::move(connection));
        });
    }
    co_return;
}

} // namespace fiber::http
