#include "Http1Context.h"

#include <charconv>
#include <cstring>
#include <limits>
#include <memory>
#include <system_error>

#include "HeaderMap.h"
#include "Http1ExchangeIo.h"

namespace fiber::http {

namespace {
unsigned char ascii_lower(unsigned char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return static_cast<unsigned char>(ch + ('a' - 'A'));
    }
    return ch;
}

bool equals_ascii_ci(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (ascii_lower(static_cast<unsigned char>(a[i])) != ascii_lower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

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

template<typename F>
void for_each_token(std::string_view value, F &&fn) {
    size_t start = 0;
    while (start < value.size()) {
        size_t comma = value.find(',', start);
        size_t end = comma == std::string_view::npos ? value.size() : comma;
        std::string_view token = trim_lws(value.substr(start, end - start));
        if (!token.empty()) {
            fn(token);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
}

} // namespace

bool Http1Context::handle_content_length(HttpExchange &exchange, const HttpHeaders::HeaderField &header) {
    std::string_view value = header.value_view();
    unsigned long long parsed = 0;
    auto result = std::from_chars(value.data(), value.data() + value.size(), parsed, 10);
    if (result.ec != std::errc() || result.ptr != value.data() + value.size()) {
        return false;
    }
    if (parsed > static_cast<unsigned long long>(std::numeric_limits<size_t>::max())) {
        return false;
    }
    size_t length = static_cast<size_t>(parsed);
    if (exchange.request_content_length_set_ && exchange.request_content_length_ != length) {
        return false;
    }
    if (!exchange.request_chunked_) {
        exchange.request_content_length_set_ = true;
        exchange.request_content_length_ = length;
    }
    return true;
}

bool Http1Context::handle_transfer_encoding(HttpExchange &exchange, const HttpHeaders::HeaderField &header) {
    std::string_view value = header.value_view();
    bool chunked = false;
    for_each_token(value, [&](std::string_view token) {
        if (equals_ascii_ci(token, "chunked")) {
            chunked = true;
        }
    });
    if (chunked) {
        exchange.request_chunked_ = true;
        exchange.request_content_length_set_ = false;
        exchange.request_content_length_ = 0;
    }
    return true;
}

bool Http1Context::handle_connection(HttpExchange &exchange, const HttpHeaders::HeaderField &header) {
    std::string_view value = header.value_view();
    for_each_token(value, [&](std::string_view token) {
        if (equals_ascii_ci(token, "close")) {
            exchange.request_close_ = true;
        } else if (equals_ascii_ci(token, "keep-alive")) {
            exchange.request_keep_alive_ = true;
        }
    });
    return true;
}

const HeaderMap<Http1Context::HeaderHandler> &Http1Context::header_handler_map() {
    static HeaderMap<HeaderHandler> handlers = []() {
        HeaderMap<HeaderHandler> map;
        map.insert("content-length", &Http1Context::handle_content_length);
        map.insert("transfer-encoding", &Http1Context::handle_transfer_encoding);
        map.insert("connection", &Http1Context::handle_connection);
        return map;
    }();
    return handlers;
}

namespace {

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

common::IoResult<void> grow_header_buffer(mem::IoBuf &buffer, std::size_t &growth_count,
                                          const HttpServerOptions &options, RequestLineParser *request_parser,
                                          HeaderLineParser *header_parser) {
    std::size_t next_capacity = next_header_capacity(options, buffer.capacity(), growth_count);
    if (next_capacity == 0) {
        return std::unexpected(common::IoErr::NoMem);
    }

    mem::IoBuf next = mem::IoBuf::allocate(next_capacity);
    if (!next) {
        return std::unexpected(common::IoErr::NoMem);
    }

    ParseCode code = ParseCode::Ok;
    if (request_parser) {
        code = request_parser->replace_buf_ptr(&buffer, &next);
    } else if (header_parser) {
        code = header_parser->replace_buf_ptr(&buffer, &next);
    }
    if (code != ParseCode::Ok) {
        return std::unexpected(code == ParseCode::HeaderTooLarge ? common::IoErr::Invalid : common::IoErr::NoMem);
    }

    buffer = std::move(next);
    ++growth_count;
    return {};
}

} // namespace

Http1Context::Http1Context(HttpTransport &transport, const HttpServerOptions &options) :
    transport_(&transport), options_(options) {}

fiber::async::Task<fiber::common::IoResult<ParseCode>> Http1Context::parse_request(HttpExchange &exchange) {
    exchange.request_headers_.clear();
    exchange.response_headers_.clear();
    exchange.header_bufs_.clear();
    exchange.method_ = HttpMethod::Unknown;
    exchange.version_ = HttpVersion::HTTP_1_1;
    exchange.uri_ = HttpUri{};
    exchange.method_view_ = {};
    exchange.version_view_ = {};
    exchange.set_io(nullptr);
    exchange.request_chunked_ = false;
    exchange.request_content_length_set_ = false;
    exchange.request_content_length_ = 0;
    exchange.request_close_ = false;
    exchange.request_keep_alive_ = false;
    exchange.response_chunked_ = false;
    exchange.response_close_ = false;
    exchange.response_content_length_set_ = false;
    exchange.response_content_length_ = 0;

    std::size_t growth_count = 0;
    mem::IoBuf parse_buf = mem::IoBuf::allocate(options_.header_init_size);
    if (!parse_buf) {
        co_return std::unexpected(fiber::common::IoErr::NoMem);
    }

    {
        RequestLineParser req_parser(options_);
        for (;;) {
            ParseCode code = req_parser.execute(&parse_buf);
            if (code == ParseCode::Again) {
                if (parse_buf.writable() == 0) {
                    auto grow_result = grow_header_buffer(parse_buf, growth_count, options_, &req_parser, nullptr);
                    if (!grow_result) {
                        if (next_header_capacity(options_, parse_buf.capacity(), growth_count) == 0) {
                            co_return ParseCode::HeaderTooLarge;
                        }
                        co_return std::unexpected(grow_result.error());
                    }
                }
                auto p = co_await transport_->read_into(parse_buf, options_.keep_alive_timeout);
                if (!p) {
                    co_return std::unexpected(p.error());
                }
                if (*p == 0) {
                    co_return std::unexpected(fiber::common::IoErr::ConnReset);
                }
                continue;
            }
            if (code != ParseCode::Ok) {
                co_return code;
            }
            const auto &line = req_parser.state();
            exchange.method_ = line.method;
            if (line.request_start && line.method_end && line.method_end >= line.request_start) {
                std::size_t method_len = static_cast<size_t>(line.method_end - line.request_start + 1);
                exchange.method_view_ = std::string_view(reinterpret_cast<char *>(line.request_start), method_len);
            } else {
                exchange.method_view_ = {};
            }

            exchange.version_ = static_cast<HttpVersion>(line.http_version);
            if (line.http_protocol_start && line.request_end && line.request_end >= line.http_protocol_start) {
                std::size_t version_len = static_cast<size_t>(line.request_end - line.http_protocol_start);
                exchange.version_view_ =
                        std::string_view(reinterpret_cast<char *>(line.http_protocol_start), version_len);
            } else {
                exchange.version_view_ = {};
            }

            if (line.uri_start && line.uri_end && line.uri_end >= line.uri_start) {
                std::size_t uri_len = static_cast<std::size_t>(line.uri_end - line.uri_start);
                exchange.uri_.unparsed_uri = std::string_view(reinterpret_cast<char *>(line.uri_start), uri_len);
                if (line.args_start && line.args_start <= line.uri_end) {
                    std::size_t path_len = static_cast<size_t>(line.args_start - line.uri_start - 1);
                    exchange.uri_.path = std::string_view(reinterpret_cast<char *>(line.uri_start), path_len);
                    std::size_t query_len = static_cast<size_t>(line.uri_end - line.args_start);
                    exchange.uri_.query = std::string_view(reinterpret_cast<char *>(line.args_start), query_len);
                } else {
                    exchange.uri_.path = exchange.uri_.unparsed_uri;
                }
                if (line.uri_ext && line.uri_ext < line.uri_end) {
                    size_t ext_len = static_cast<size_t>(line.uri_end - line.uri_ext);
                    exchange.uri_.exten = std::string_view(reinterpret_cast<char *>(line.uri_ext), ext_len);
                }
            }
            break;
        }
    }
    {
        HeaderLineParser hdr_parser(options_);
        for (;;) {
            ParseCode code = hdr_parser.execute(&parse_buf);
            if (code == ParseCode::Again) {
                if (parse_buf.writable() == 0) {
                    auto grow_result = grow_header_buffer(parse_buf, growth_count, options_, nullptr, &hdr_parser);
                    if (!grow_result) {
                        if (next_header_capacity(options_, parse_buf.capacity(), growth_count) == 0) {
                            co_return ParseCode::HeaderTooLarge;
                        }
                        co_return std::unexpected(grow_result.error());
                    }
                }
                auto p = co_await transport_->read_into(parse_buf, options_.keep_alive_timeout);
                if (!p) {
                    co_return std::unexpected(p.error());
                }
                if (*p == 0) {
                    co_return std::unexpected(fiber::common::IoErr::ConnReset);
                }
                continue;
            }
            if (code == ParseCode::Ok) {
                const auto &line = hdr_parser.state();
                if (!line.header_name_start || !line.header_name_end || line.header_name_end < line.header_name_start) {
                    co_return ParseCode::InvalidHeader;
                }
                std::size_t name_len = static_cast<std::size_t>(line.header_name_end - line.header_name_start);
                std::string_view name(reinterpret_cast<char *>(line.header_name_start), name_len);
                std::string_view value;
                if (line.header_start && line.header_end && line.header_end >= line.header_start) {
                    std::size_t value_len = static_cast<std::size_t>(line.header_end - line.header_start);
                    value = std::string_view(reinterpret_cast<char *>(line.header_start), value_len);
                }
                char *lowercase = static_cast<char *>(exchange.pool_.alloc(name_len));
                if (lowercase == nullptr) {
                    co_return std::unexpected(common::IoErr::NoMem);
                }
                if (line.lowcase_index == name_len) {
                    ::memcpy(lowercase, line.lowcase_header, name_len);
                } else {
                    to_lowercase(name, lowercase);
                }

                uint32_t hash = line.header_hash;

                HttpHeaders::HeaderField *field = exchange.request_headers_.add_view(name, value, lowercase, hash);
                if (!field) {
                    co_return std::unexpected(common::IoErr::NoMem);
                }
                if (auto *handler = header_handler_map().get(std::string_view(lowercase, name_len), hash)) {
                    if (!(*handler)(exchange, *field)) {
                        co_return ParseCode::InvalidHeader;
                    }
                }
                continue;
            }
            if (code == ParseCode::HeaderDone) {
                mem::IoBuf header_owner = parse_buf;
                const std::size_t header_bytes = static_cast<std::size_t>(parse_buf.readable_data() - parse_buf.data());
                header_owner.reset();
                header_owner.commit(header_bytes);
                if (!exchange.header_bufs_.append(std::move(header_owner))) {
                    co_return std::unexpected(common::IoErr::NoMem);
                }

                mem::IoBufChain body_bufs;
                if (parse_buf.readable() > 0) {
                    mem::IoBuf body_prefix = parse_buf.retain_slice(0, parse_buf.readable());
                    if (!body_bufs.append(std::move(body_prefix))) {
                        co_return std::unexpected(common::IoErr::NoMem);
                    }
                }

                exchange.set_io(std::make_unique<Http1ExchangeIo>(*transport_, options_, std::move(body_bufs)));
                co_return ParseCode::Ok;
            }
            if (code != ParseCode::Again) {
                co_return code;
            }
        }
    }
}

} // namespace fiber::http
