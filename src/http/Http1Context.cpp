#include "Http1Context.h"

#include <cstring>

namespace fiber::http {

Http1Context::Http1Context(HttpTransport &transport, const HttpServerOptions &options) :
    header_bufs_(HeaderBuffers::Opt{options.header_init_size, options.header_large_size, options.header_large_num}),
    header_pool_(options.header_init_size), transport_(&transport), options_(options) {}

fiber::async::Task<fiber::common::IoResult<ParseCode>> Http1Context::parse_request(HttpExchange &exchange,
                                                                                   BufChain *chain) {
    if (!chain) {
        chain = header_bufs_.alloc(header_pool_);
        if (!chain) {
            co_return std::unexpected(fiber::common::IoErr::NoMem);
        }
    }
    std::size_t header_len = 0;
    {
        RequestLineParser req_parser(options_);
        {
        parse_request:
            if (chain->writable() == 0) {
                BufChain *next = header_bufs_.alloc(header_pool_);
                if (!next) {
                    if (header_bufs_.exhausted()) {
                        co_return ParseCode::HeaderTooLarge;
                    }
                    co_return std::unexpected(fiber::common::IoErr::NoMem);
                }
                ParseCode code = req_parser.replace_buf_ptr(chain, next);
                if (code != ParseCode::Ok) {
                    co_return code;
                }
                chain = next;
            }
            auto p = co_await transport_->read_into(chain, options_.keep_alive_timeout);
            if (!p) {
                co_return std::unexpected(p.error());
            }
            header_len += *p;
            ParseCode code = req_parser.execute(chain);
            if (code == ParseCode::Again) {
                goto parse_request;
            }
            if (code != ParseCode::Ok) {
                co_return code;
            }
            const auto &line = req_parser.state();
            exchange.method_ = line.method;
            if (line.request_start && line.method_end && line.method_end >= line.request_start) {
                size_t method_len = static_cast<size_t>(line.method_end - line.request_start + 1);
                exchange.method_view_ = std::string_view(reinterpret_cast<char *>(line.request_start), method_len);
            } else {
                exchange.method_view_ = {};
            }

            exchange.version_ = static_cast<HttpVersion>(line.http_version);
            if (line.http_protocol_start && line.request_end && line.request_end >= line.http_protocol_start) {
                size_t version_len = static_cast<size_t>(line.request_end - line.http_protocol_start + 1);
                exchange.version_view_ =
                        std::string_view(reinterpret_cast<char *>(line.http_protocol_start), version_len);
            } else {
                exchange.version_view_ = {};
            }

            exchange.uri_ = HttpUri{};
            if (line.uri_start && line.uri_end && line.uri_end >= line.uri_start) {
                size_t uri_len = static_cast<size_t>(line.uri_end - line.uri_start);
                exchange.uri_.unparsed_uri = std::string_view(reinterpret_cast<char *>(line.uri_start), uri_len);
                if (line.args_start && line.args_start <= line.uri_end) {
                    size_t path_len = static_cast<size_t>(line.args_start - line.uri_start - 1);
                    exchange.uri_.path = std::string_view(reinterpret_cast<char *>(line.uri_start), path_len);
                    size_t query_len = static_cast<size_t>(line.uri_end - line.args_start);
                    exchange.uri_.query = std::string_view(reinterpret_cast<char *>(line.args_start), query_len);
                } else {
                    exchange.uri_.path = exchange.uri_.unparsed_uri;
                }
                if (line.uri_ext && line.uri_ext < line.uri_end) {
                    size_t ext_len = static_cast<size_t>(line.uri_end - line.uri_ext);
                    exchange.uri_.exten = std::string_view(reinterpret_cast<char *>(line.uri_ext), ext_len);
                }
            }
        }
    }
    {
        HeaderLineParser hdr_parser(options_);
    parse_line:
        if (chain->writable() == 0) {
            BufChain *next = header_bufs_.alloc(header_pool_);
            if (!next) {
                if (header_bufs_.exhausted()) {
                    co_return ParseCode::HeaderTooLarge;
                }
                co_return std::unexpected(fiber::common::IoErr::NoMem);
            }
            ParseCode code = hdr_parser.replace_buf_ptr(chain, next);
            if (code != ParseCode::Ok) {
                co_return code;
            }
            chain = next;
        }
        auto p = co_await transport_->read_into(chain, options_.header_timeout);
        if (!p) {
            co_return std::unexpected(p.error());
        }
        header_len += *p;
        ParseCode code = hdr_parser.execute(chain);
        if (code == ParseCode::Ok) {
            const auto &line = hdr_parser.state();
            if (!line.header_name_start || !line.header_name_end || line.header_name_end < line.header_name_start) {
                co_return ParseCode::InvalidHeader;
            }
            size_t name_len = static_cast<size_t>(line.header_name_end - line.header_name_start);
            std::string_view name(reinterpret_cast<char *>(line.header_name_start), name_len);
            std::string_view value;
            if (line.header_start && line.header_end && line.header_end >= line.header_start) {
                size_t value_len = static_cast<size_t>(line.header_end - line.header_start);
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
            if (!exchange.request_headers_.add_view(name, value, lowercase, hash)) {
                co_return ParseCode::Error;
            }

            goto parse_line;
        }
        if (code == ParseCode::Again) {
            goto parse_line;
        }
        if (code == ParseCode::HeaderDone) {
            if (chain->readable() > 0) {
                exchange.header_adjacent_body_ = chain;
            }
            co_return ParseCode::Ok;
        }
        co_return ParseCode::Error;
    }
}

} // namespace fiber::http
