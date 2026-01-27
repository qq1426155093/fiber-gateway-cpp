#include "Http1Parser.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <string_view>

namespace fiber::http {

namespace {

constexpr int NGX_OK = 0;
constexpr int NGX_ERROR = -1;
constexpr int NGX_AGAIN = -2;
constexpr int NGX_DONE = -3;

constexpr int NGX_HTTP_PARSE_INVALID_METHOD = -10;
constexpr int NGX_HTTP_PARSE_INVALID_REQUEST = -11;
constexpr int NGX_HTTP_PARSE_INVALID_VERSION = -12;
constexpr int NGX_HTTP_PARSE_INVALID_09_METHOD = -13;
constexpr int NGX_HTTP_PARSE_INVALID_HEADER = -14;
constexpr int NGX_HTTP_PARSE_HEADER_DONE = -15;

constexpr size_t kMaxOffT = std::numeric_limits<size_t>::max();
constexpr int kChunkDataState = 4;

static const uint32_t usual[] = {
    0x00000000,
    0x7fff37d6,
#if defined(_WIN32)
    0xefffffff,
#else
    0xffffffff,
#endif
    0x7fffffff,
    0xffffffff,
    0xffffffff,
    0xffffffff,
    0xffffffff,
};

inline bool str3_cmp(const char *m, char c0, char c1, char c2, char) {
    return m[0] == c0 && m[1] == c1 && m[2] == c2;
}

inline bool str3Ocmp(const char *m, char c0, char, char c2, char c3) {
    return m[0] == c0 && m[2] == c2 && m[3] == c3;
}

inline bool str4cmp(const char *m, char c0, char c1, char c2, char c3) {
    return m[0] == c0 && m[1] == c1 && m[2] == c2 && m[3] == c3;
}

inline bool str5cmp(const char *m, char c0, char c1, char c2, char c3, char c4) {
    return m[0] == c0 && m[1] == c1 && m[2] == c2 && m[3] == c3 && m[4] == c4;
}

inline bool str6cmp(const char *m, char c0, char c1, char c2, char c3, char c4, char c5) {
    return m[0] == c0 && m[1] == c1 && m[2] == c2 && m[3] == c3 && m[4] == c4 && m[5] == c5;
}

inline bool str7cmp(const char *m, char c0, char c1, char c2, char c3, char c4, char c5, char c6) {
    return m[0] == c0 && m[1] == c1 && m[2] == c2 && m[3] == c3 && m[4] == c4 && m[5] == c5 && m[6] == c6;
}

inline bool str8cmp(const char *m, char c0, char c1, char c2, char c3, char c4, char c5, char c6, char c7) {
    return m[0] == c0 && m[1] == c1 && m[2] == c2 && m[3] == c3 && m[4] == c4 && m[5] == c5 && m[6] == c6 && m[7] == c7;
}

inline bool str9cmp(const char *m, char c0, char c1, char c2, char c3, char c4, char c5, char c6, char c7, char c8) {
    return m[0] == c0 && m[1] == c1 && m[2] == c2 && m[3] == c3 && m[4] == c4 && m[5] == c5 && m[6] == c6 && m[7] == c7 && m[8] == c8;
}

inline uint32_t ngx_hash(uint32_t key, unsigned char c) {
    return key * 31u + static_cast<uint32_t>(c);
}

bool has_token(std::string_view value, std::string_view token) {
    size_t pos = 0;
    while (pos < value.size()) {
        while (pos < value.size() && (value[pos] == ' ' || value[pos] == '\t' || value[pos] == ',')) {
            ++pos;
        }
        size_t start = pos;
        while (pos < value.size() && value[pos] != ',' && value[pos] != ' ' && value[pos] != '\t') {
            ++pos;
        }
        if (start < pos) {
            if (pos - start == token.size()) {
                bool match = true;
                for (size_t i = 0; i < token.size(); ++i) {
                    char a = value[start + i];
                    char b = token[i];
                    if (a >= 'A' && a <= 'Z') {
                        a = static_cast<char>(a - 'A' + 'a');
                    }
                    if (a != b) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    return true;
                }
            }
        }
        while (pos < value.size() && value[pos] != ',') {
            ++pos;
        }
        if (pos < value.size() && value[pos] == ',') {
            ++pos;
        }
    }
    return false;
}

bool equals_ascii_ci(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (size_t i = 0; i < left.size(); ++i) {
        char a = left[i];
        char b = right[i];
        if (a >= 'A' && a <= 'Z') {
            a = static_cast<char>(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = static_cast<char>(b - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

bool parse_transfer_encoding(std::string_view value, bool &chunked) {
    chunked = false;
    size_t pos = 0;
    while (pos < value.size()) {
        while (pos < value.size() && (value[pos] == ' ' || value[pos] == '\t' || value[pos] == ',')) {
            ++pos;
        }
        size_t start = pos;
        while (pos < value.size() && value[pos] != ',' && value[pos] != ' ' && value[pos] != '\t') {
            ++pos;
        }
        if (start < pos) {
            std::string token;
            token.reserve(pos - start);
            for (size_t i = start; i < pos; ++i) {
                char ch = value[i];
                if (ch >= 'A' && ch <= 'Z') {
                    ch = static_cast<char>(ch - 'A' + 'a');
                }
                token.push_back(ch);
            }
            if (token == "chunked") {
                chunked = true;
            } else if (token == "identity") {
                continue;
            } else {
                return false;
            }
        }
        while (pos < value.size() && value[pos] != ',') {
            ++pos;
        }
        if (pos < value.size() && value[pos] == ',') {
            ++pos;
        }
    }
    return chunked;
}

} // namespace

Http1Parser::Http1Parser() = default;

void Http1Parser::reset(HttpExchange &exchange, const HttpServerOptions &options) {
    exchange_ = &exchange;
    options_ = &options;
    reset_state();
}

void Http1Parser::resume() {
    paused_ = false;
}

HttpParseResult Http1Parser::execute(const char *data, size_t len) {
    HttpParseResult result{};
    if (!data || len == 0) {
        result.state = HttpParseState::NeedMore;
        return result;
    }

    if (!exchange_) {
        result.state = HttpParseState::Error;
        result.error = HttpParseError::BadRequest;
        return result;
    }

    parse_buffer_.append(data, len);
    result.consumed = len;

    auto fail = [&](HttpParseError error) {
        parse_error_ = error;
        result.state = HttpParseState::Error;
        result.error = error;
        return result;
    };

    auto check_header_limit = [&](size_t delta) -> bool {
        if (delta == 0) {
            return true;
        }
        header_bytes_ += delta;
        if (options_ && header_bytes_ > options_->max_header_bytes) {
            parse_error_ = HttpParseError::HeadersTooLarge;
            return false;
        }
        return true;
    };

    auto compact_buffer = [&]() {
        if (parse_offset_ == 0) {
            return;
        }
        parse_buffer_.erase(0, parse_offset_);
        parse_offset_ = 0;
    };

    for (;;) {
        if (paused_) {
            result.state = headers_complete_ ? HttpParseState::HeadersComplete : HttpParseState::NeedMore;
            return result;
        }

        if (stage_ == ParseStage::RequestLine) {
            ParseBuffer buffer{parse_buffer_.data(),
                               parse_buffer_.data() + parse_offset_,
                               parse_buffer_.data() + parse_buffer_.size()};
            size_t before = parse_offset_;
            int rc = parse_request_line(request_state_, buffer);
            parse_offset_ = static_cast<size_t>(buffer.pos - buffer.start);
            if (!check_header_limit(parse_offset_ - before)) {
                return fail(HttpParseError::HeadersTooLarge);
            }

            if (rc == NGX_OK) {
                if (request_state_.request_start == kInvalidPos || request_state_.method_end == kInvalidPos ||
                    request_state_.uri_start == kInvalidPos || request_state_.uri_end == kInvalidPos) {
                    return fail(HttpParseError::BadRequest);
                }
                const char *base = parse_buffer_.data();
                size_t method_len = request_state_.method_end - request_state_.request_start + 1;
                size_t uri_len = request_state_.uri_end - request_state_.uri_start;
                exchange_->method_.assign(base + request_state_.request_start, method_len);
                exchange_->target_.assign(base + request_state_.uri_start, uri_len);
                if (request_state_.http_major < 0 || request_state_.http_minor < 0) {
                    return fail(HttpParseError::BadRequest);
                }
                if (request_state_.http_version == 9) {
                    exchange_->version_ = "HTTP/0.9";
                    exchange_->request_keep_alive_ = false;
                    exchange_->body_complete_ = true;
                    message_complete_ = true;
                    headers_complete_ = true;
                    paused_ = true;
                    stage_ = ParseStage::Body;
                    compact_buffer();
                    result.state = HttpParseState::HeadersComplete;
                    return result;
                }
                compact_buffer();
                stage_ = ParseStage::Headers;
                continue;
            }

            if (rc == NGX_AGAIN) {
                result.state = HttpParseState::NeedMore;
                return result;
            }

            if (rc == NGX_HTTP_PARSE_INVALID_METHOD || rc == NGX_HTTP_PARSE_INVALID_09_METHOD ||
                rc == NGX_HTTP_PARSE_INVALID_REQUEST || rc == NGX_HTTP_PARSE_INVALID_VERSION) {
                return fail(HttpParseError::BadRequest);
            }

            return fail(HttpParseError::BadRequest);
        }

        if (stage_ == ParseStage::Headers) {
            ParseBuffer buffer{parse_buffer_.data(),
                               parse_buffer_.data() + parse_offset_,
                               parse_buffer_.data() + parse_buffer_.size()};
            size_t before = parse_offset_;
            int rc = parse_header_line(request_state_, buffer, true);
            parse_offset_ = static_cast<size_t>(buffer.pos - buffer.start);
            if (!check_header_limit(parse_offset_ - before)) {
                return fail(HttpParseError::HeadersTooLarge);
            }

            if (rc == NGX_OK) {
                if (request_state_.header_name_start != kInvalidPos &&
                    request_state_.header_name_end != kInvalidPos &&
                    request_state_.header_name_end >= request_state_.header_name_start) {
                    const char *base = parse_buffer_.data();
                    size_t name_len = request_state_.header_name_end - request_state_.header_name_start;
                    std::string_view name(base + request_state_.header_name_start, name_len);
                    std::string_view value;
                    if (request_state_.header_start != kInvalidPos && request_state_.header_end != kInvalidPos &&
                        request_state_.header_end >= request_state_.header_start) {
                        size_t value_len = request_state_.header_end - request_state_.header_start;
                        value = std::string_view(base + request_state_.header_start, value_len);
                    } else {
                        value = std::string_view();
                    }
                    if (!exchange_->request_headers_.add(name, value)) {
                        return fail(HttpParseError::HeadersTooLarge);
                    }

                    if (equals_ascii_ci(name, "content-length")) {
                        size_t length = 0;
                        auto res = std::from_chars(value.data(), value.data() + value.size(), length);
                        if (res.ec != std::errc() || res.ptr != value.data() + value.size()) {
                            return fail(HttpParseError::BadRequest);
                        }
                        if (exchange_->request_content_length_set_ && exchange_->request_content_length_ != length) {
                            return fail(HttpParseError::BadRequest);
                        }
                        exchange_->request_content_length_ = length;
                        exchange_->request_content_length_set_ = true;
                    } else if (equals_ascii_ci(name, "transfer-encoding")) {
                        bool chunked = false;
                        if (!parse_transfer_encoding(value, chunked)) {
                            return fail(HttpParseError::UnsupportedTransferEncoding);
                        }
                        exchange_->request_chunked_ = chunked;
                    } else if (equals_ascii_ci(name, "expect")) {
                        if (has_token(value, "100-continue")) {
                            exchange_->request_expect_continue_ = true;
                        }
                    } else if (equals_ascii_ci(name, "connection")) {
                        if (has_token(value, "close")) {
                            connection_close_ = true;
                        }
                        if (has_token(value, "keep-alive")) {
                            connection_keep_alive_ = true;
                        }
                    }
                }

                compact_buffer();
                continue;
            }

            if (rc == NGX_HTTP_PARSE_HEADER_DONE) {
                exchange_->version_.clear();
                exchange_->version_.append("HTTP/");
                exchange_->version_.append(std::to_string(request_state_.http_major));
                exchange_->version_.append(".");
                exchange_->version_.append(std::to_string(request_state_.http_minor));

                if (request_state_.http_major == 1 && request_state_.http_minor >= 1) {
                    exchange_->request_keep_alive_ = !connection_close_;
                } else if (request_state_.http_major == 1 && request_state_.http_minor == 0) {
                    exchange_->request_keep_alive_ = connection_keep_alive_;
                } else {
                    exchange_->request_keep_alive_ = false;
                }

                if (exchange_->request_chunked_) {
                    exchange_->request_content_length_ = 0;
                    exchange_->request_content_length_set_ = false;
                }

                if (options_ && exchange_->request_content_length_ > options_->max_body_bytes) {
                    return fail(HttpParseError::BodyTooLarge);
                }

                if (!exchange_->request_chunked_ && (!exchange_->request_content_length_set_ ||
                                                     exchange_->request_content_length_ == 0)) {
                    exchange_->body_complete_ = true;
                    message_complete_ = true;
                }

                headers_complete_ = true;
                paused_ = true;
                stage_ = ParseStage::Body;
                compact_buffer();

                result.state = HttpParseState::HeadersComplete;
                return result;
            }

            if (rc == NGX_AGAIN) {
                result.state = HttpParseState::NeedMore;
                return result;
            }

            if (rc == NGX_HTTP_PARSE_INVALID_HEADER) {
                return fail(HttpParseError::BadRequest);
            }

            return fail(HttpParseError::BadRequest);
        }

        if (stage_ == ParseStage::Body) {
            if (!exchange_->request_chunked_ && !exchange_->request_content_length_set_) {
                exchange_->body_complete_ = true;
                message_complete_ = true;
                result.state = HttpParseState::MessageComplete;
                return result;
            }

            if (!exchange_->request_chunked_) {
                size_t remaining = 0;
                if (exchange_->request_content_length_ > body_bytes_) {
                    remaining = exchange_->request_content_length_ - body_bytes_;
                }

                if (remaining == 0) {
                    exchange_->body_complete_ = true;
                    message_complete_ = true;
                    result.state = HttpParseState::MessageComplete;
                    return result;
                }

                size_t available = parse_buffer_.size() - parse_offset_;
                if (available == 0) {
                    result.state = HttpParseState::NeedMore;
                    return result;
                }

                size_t to_copy = std::min(remaining, available);
                if (options_ && body_bytes_ + to_copy > options_->max_body_bytes) {
                    return fail(HttpParseError::BodyTooLarge);
                }

                exchange_->body_buffer_.append(parse_buffer_.data() + parse_offset_, to_copy);
                parse_offset_ += to_copy;
                body_bytes_ += to_copy;
                compact_buffer();

                if (body_bytes_ >= exchange_->request_content_length_) {
                    exchange_->body_complete_ = true;
                    message_complete_ = true;
                    result.state = HttpParseState::MessageComplete;
                    return result;
                }

                result.state = HttpParseState::NeedMore;
                return result;
            }

            for (;;) {
                if (chunked_state_.state == kChunkDataState && chunked_state_.size > 0) {
                    size_t available = parse_buffer_.size() - parse_offset_;
                    if (available == 0) {
                        result.state = HttpParseState::NeedMore;
                        return result;
                    }
                    size_t to_copy = std::min(chunked_state_.size, available);
                    if (options_ && body_bytes_ + to_copy > options_->max_body_bytes) {
                        return fail(HttpParseError::BodyTooLarge);
                    }
                    exchange_->body_buffer_.append(parse_buffer_.data() + parse_offset_, to_copy);
                    parse_offset_ += to_copy;
                    body_bytes_ += to_copy;
                    chunked_state_.size -= to_copy;
                    compact_buffer();
                    if (chunked_state_.size > 0) {
                        result.state = HttpParseState::NeedMore;
                        return result;
                    }
                    continue;
                }

                ParseBuffer buffer{parse_buffer_.data(),
                                   parse_buffer_.data() + parse_offset_,
                                   parse_buffer_.data() + parse_buffer_.size()};
                int rc = parse_chunked(chunked_state_, buffer);
                parse_offset_ = static_cast<size_t>(buffer.pos - buffer.start);
                compact_buffer();

                if (rc == NGX_OK) {
                    if (options_ && chunked_state_.size > options_->max_chunk_bytes) {
                        return fail(HttpParseError::ChunkTooLarge);
                    }
                    continue;
                }

                if (rc == NGX_AGAIN) {
                    result.state = HttpParseState::NeedMore;
                    return result;
                }

                if (rc == NGX_DONE) {
                    exchange_->body_complete_ = true;
                    message_complete_ = true;
                    result.state = HttpParseState::MessageComplete;
                    return result;
                }

                return fail(HttpParseError::BadRequest);
            }
        }
    }
}

bool Http1Parser::should_keep_alive() const noexcept {
    return exchange_ ? exchange_->request_keep_alive_ : false;
}

size_t Http1Parser::buffer_offset(const ParseBuffer &buffer, const char *p) noexcept {
    return static_cast<size_t>(p - buffer.start);
}

int Http1Parser::parse_request_line(RequestState &r, ParseBuffer &b) {
    unsigned char c = 0;
    unsigned char ch = 0;
    const char *p = nullptr;
    const char *m = nullptr;
    enum {
        sw_start = 0,
        sw_method,
        sw_spaces_before_uri,
        sw_schema,
        sw_schema_slash,
        sw_schema_slash_slash,
        sw_host_start,
        sw_host,
        sw_host_end,
        sw_host_ip_literal,
        sw_port,
        sw_after_slash_in_uri,
        sw_check_uri,
        sw_uri,
        sw_http_09,
        sw_http_H,
        sw_http_HT,
        sw_http_HTT,
        sw_http_HTTP,
        sw_first_major_digit,
        sw_major_digit,
        sw_first_minor_digit,
        sw_minor_digit,
        sw_spaces_after_digit,
        sw_almost_done
    } state;

    state = static_cast<decltype(state)>(r.state);

    for (p = b.pos; p < b.last; p++) {
        ch = static_cast<unsigned char>(*p);

        switch (state) {

        case sw_start:
            r.request_start = buffer_offset(b, p);

            if (ch == '\r' || ch == '\n') {
                break;
            }

            if ((ch < 'A' || ch > 'Z') && ch != '_' && ch != '-') {
                return NGX_HTTP_PARSE_INVALID_METHOD;
            }

            state = sw_method;
            break;

        case sw_method:
            if (ch == ' ') {
                r.method_end = buffer_offset(b, p) - 1;
                m = b.start + r.request_start;

                switch (p - m) {
                case 3:
                    if (str3_cmp(m, 'G', 'E', 'T', ' ')) {
                        r.method_is_get = true;
                        break;
                    }
                    if (str3_cmp(m, 'P', 'U', 'T', ' ')) {
                        break;
                    }
                    break;
                case 4:
                    if (m[1] == 'O') {
                        if (str3Ocmp(m, 'P', 'O', 'S', 'T')) {
                            break;
                        }
                        if (str3Ocmp(m, 'C', 'O', 'P', 'Y')) {
                            break;
                        }
                        if (str3Ocmp(m, 'M', 'O', 'V', 'E')) {
                            break;
                        }
                        if (str3Ocmp(m, 'L', 'O', 'C', 'K')) {
                            break;
                        }
                    } else {
                        if (str4cmp(m, 'H', 'E', 'A', 'D')) {
                            break;
                        }
                    }
                    break;
                case 5:
                    if (str5cmp(m, 'M', 'K', 'C', 'O', 'L')) {
                        break;
                    }
                    if (str5cmp(m, 'P', 'A', 'T', 'C', 'H')) {
                        break;
                    }
                    if (str5cmp(m, 'T', 'R', 'A', 'C', 'E')) {
                        break;
                    }
                    break;
                case 6:
                    if (str6cmp(m, 'D', 'E', 'L', 'E', 'T', 'E')) {
                        break;
                    }
                    if (str6cmp(m, 'U', 'N', 'L', 'O', 'C', 'K')) {
                        break;
                    }
                    break;
                case 7:
                    if (str7cmp(m, 'O', 'P', 'T', 'I', 'O', 'N', 'S')) {
                        break;
                    }
                    if (str7cmp(m, 'C', 'O', 'N', 'N', 'E', 'C', 'T')) {
                        break;
                    }
                    break;
                case 8:
                    if (str8cmp(m, 'P', 'R', 'O', 'P', 'F', 'I', 'N', 'D')) {
                        break;
                    }
                    break;
                case 9:
                    if (str9cmp(m, 'P', 'R', 'O', 'P', 'P', 'A', 'T', 'C', 'H')) {
                        break;
                    }
                    break;
                }

                state = sw_spaces_before_uri;
                break;
            }

            if ((ch < 'A' || ch > 'Z') && ch != '_' && ch != '-') {
                return NGX_HTTP_PARSE_INVALID_METHOD;
            }

            break;

        case sw_spaces_before_uri:
            if (ch == '/') {
                r.uri_start = buffer_offset(b, p);
                state = sw_after_slash_in_uri;
                break;
            }

            c = static_cast<unsigned char>(ch | 0x20u);
            if (c >= 'a' && c <= 'z') {
                r.schema_start = buffer_offset(b, p);
                state = sw_schema;
                break;
            }

            switch (ch) {
            case ' ':
                break;
            default:
                return NGX_HTTP_PARSE_INVALID_REQUEST;
            }
            break;

        case sw_schema:
            c = static_cast<unsigned char>(ch | 0x20u);
            if (c >= 'a' && c <= 'z') {
                break;
            }
            if ((ch >= '0' && ch <= '9') || ch == '+' || ch == '-' || ch == '.') {
                break;
            }
            switch (ch) {
            case ':':
                r.schema_end = buffer_offset(b, p);
                state = sw_schema_slash;
                break;
            default:
                return NGX_HTTP_PARSE_INVALID_REQUEST;
            }
            break;

        case sw_schema_slash:
            switch (ch) {
            case '/':
                state = sw_schema_slash_slash;
                break;
            default:
                return NGX_HTTP_PARSE_INVALID_REQUEST;
            }
            break;

        case sw_schema_slash_slash:
            switch (ch) {
            case '/':
                state = sw_host_start;
                break;
            default:
                return NGX_HTTP_PARSE_INVALID_REQUEST;
            }
            break;

        case sw_host_start:
            r.host_start = buffer_offset(b, p);
            if (ch == '[') {
                state = sw_host_ip_literal;
                break;
            }
            state = sw_host;
            [[fallthrough]];

        case sw_host:
            c = static_cast<unsigned char>(ch | 0x20u);
            if (c >= 'a' && c <= 'z') {
                break;
            }
            if ((ch >= '0' && ch <= '9') || ch == '.' || ch == '-') {
                break;
            }
            [[fallthrough]];

        case sw_host_end:
            r.host_end = buffer_offset(b, p);
            switch (ch) {
            case ':':
                state = sw_port;
                break;
            case '/':
                r.uri_start = buffer_offset(b, p);
                state = sw_after_slash_in_uri;
                break;
            case '?':
                r.uri_start = buffer_offset(b, p);
                r.args_start = buffer_offset(b, p) + 1;
                r.empty_path_in_uri = true;
                state = sw_uri;
                break;
            case ' ':
                if (r.schema_end != kInvalidPos) {
                    r.uri_start = r.schema_end + 1;
                    r.uri_end = r.schema_end + 2;
                }
                state = sw_http_09;
                break;
            default:
                return NGX_HTTP_PARSE_INVALID_REQUEST;
            }
            break;

        case sw_host_ip_literal:
            if (ch >= '0' && ch <= '9') {
                break;
            }
            c = static_cast<unsigned char>(ch | 0x20u);
            if (c >= 'a' && c <= 'z') {
                break;
            }
            switch (ch) {
            case ':':
                break;
            case ']':
                state = sw_host_end;
                break;
            case '-':
            case '.':
            case '_':
            case '~':
                break;
            case '!':
            case '$':
            case '&':
            case '\'':
            case '(':
            case ')':
            case '*':
            case '+':
            case ',':
            case ';':
            case '=':
                break;
            default:
                return NGX_HTTP_PARSE_INVALID_REQUEST;
            }
            break;

        case sw_port:
            if (ch >= '0' && ch <= '9') {
                break;
            }
            switch (ch) {
            case '/':
                r.uri_start = buffer_offset(b, p);
                state = sw_after_slash_in_uri;
                break;
            case '?':
                r.uri_start = buffer_offset(b, p);
                r.args_start = buffer_offset(b, p) + 1;
                r.empty_path_in_uri = true;
                state = sw_uri;
                break;
            case ' ':
                if (r.schema_end != kInvalidPos) {
                    r.uri_start = r.schema_end + 1;
                    r.uri_end = r.schema_end + 2;
                }
                state = sw_http_09;
                break;
            default:
                return NGX_HTTP_PARSE_INVALID_REQUEST;
            }
            break;

        case sw_after_slash_in_uri:
            if (usual[ch >> 5] & (1U << (ch & 0x1f))) {
                state = sw_check_uri;
                break;
            }
            switch (ch) {
            case ' ':
                r.uri_end = buffer_offset(b, p);
                state = sw_http_09;
                break;
            case '\r':
                r.uri_end = buffer_offset(b, p);
                r.http_minor = 9;
                state = sw_almost_done;
                break;
            case '\n':
                r.uri_end = buffer_offset(b, p);
                r.http_minor = 9;
                goto done;
            case '.':
                r.complex_uri = true;
                state = sw_uri;
                break;
            case '%':
                r.quoted_uri = true;
                state = sw_uri;
                break;
            case '/':
                r.complex_uri = true;
                state = sw_uri;
                break;
#if defined(_WIN32)
            case '\\':
                r.complex_uri = true;
                state = sw_uri;
                break;
#endif
            case '?':
                r.args_start = buffer_offset(b, p) + 1;
                state = sw_uri;
                break;
            case '#':
                r.complex_uri = true;
                state = sw_uri;
                break;
            case '+':
                r.plus_in_uri = true;
                break;
            default:
                if (ch < 0x20 || ch == 0x7f) {
                    return NGX_HTTP_PARSE_INVALID_REQUEST;
                }
                state = sw_check_uri;
                break;
            }
            break;

        case sw_check_uri:
            if (usual[ch >> 5] & (1U << (ch & 0x1f))) {
                break;
            }
            switch (ch) {
            case '/':
#if defined(_WIN32)
                if (r.uri_ext != kInvalidPos && r.uri_ext == buffer_offset(b, p)) {
                    r.complex_uri = true;
                    state = sw_uri;
                    break;
                }
#endif
                r.uri_ext = kInvalidPos;
                state = sw_after_slash_in_uri;
                break;
            case '.':
                r.uri_ext = buffer_offset(b, p) + 1;
                break;
            case ' ':
                r.uri_end = buffer_offset(b, p);
                state = sw_http_09;
                break;
            case '\r':
                r.uri_end = buffer_offset(b, p);
                r.http_minor = 9;
                state = sw_almost_done;
                break;
            case '\n':
                r.uri_end = buffer_offset(b, p);
                r.http_minor = 9;
                goto done;
#if defined(_WIN32)
            case '\\':
                r.complex_uri = true;
                state = sw_after_slash_in_uri;
                break;
#endif
            case '%':
                r.quoted_uri = true;
                state = sw_uri;
                break;
            case '?':
                r.args_start = buffer_offset(b, p) + 1;
                state = sw_uri;
                break;
            case '#':
                r.complex_uri = true;
                state = sw_uri;
                break;
            case '+':
                r.plus_in_uri = true;
                break;
            default:
                if (ch < 0x20 || ch == 0x7f) {
                    return NGX_HTTP_PARSE_INVALID_REQUEST;
                }
                break;
            }
            break;

        case sw_uri:
            if (usual[ch >> 5] & (1U << (ch & 0x1f))) {
                break;
            }
            switch (ch) {
            case ' ':
                r.uri_end = buffer_offset(b, p);
                state = sw_http_09;
                break;
            case '\r':
                r.uri_end = buffer_offset(b, p);
                r.http_minor = 9;
                state = sw_almost_done;
                break;
            case '\n':
                r.uri_end = buffer_offset(b, p);
                r.http_minor = 9;
                goto done;
            case '#':
                r.complex_uri = true;
                break;
            default:
                if (ch < 0x20 || ch == 0x7f) {
                    return NGX_HTTP_PARSE_INVALID_REQUEST;
                }
                break;
            }
            break;

        case sw_http_09:
            switch (ch) {
            case ' ':
                break;
            case '\r':
                r.http_minor = 9;
                state = sw_almost_done;
                break;
            case '\n':
                r.http_minor = 9;
                goto done;
            case 'H':
                r.http_protocol_start = buffer_offset(b, p);
                state = sw_http_H;
                break;
            default:
                return NGX_HTTP_PARSE_INVALID_REQUEST;
            }
            break;

        case sw_http_H:
            switch (ch) {
            case 'T':
                state = sw_http_HT;
                break;
            default:
                return NGX_HTTP_PARSE_INVALID_REQUEST;
            }
            break;

        case sw_http_HT:
            switch (ch) {
            case 'T':
                state = sw_http_HTT;
                break;
            default:
                return NGX_HTTP_PARSE_INVALID_REQUEST;
            }
            break;

        case sw_http_HTT:
            switch (ch) {
            case 'P':
                state = sw_http_HTTP;
                break;
            default:
                return NGX_HTTP_PARSE_INVALID_REQUEST;
            }
            break;

        case sw_http_HTTP:
            switch (ch) {
            case '/':
                state = sw_first_major_digit;
                break;
            default:
                return NGX_HTTP_PARSE_INVALID_REQUEST;
            }
            break;

        case sw_first_major_digit:
            if (ch < '1' || ch > '9') {
                return NGX_HTTP_PARSE_INVALID_REQUEST;
            }
            r.http_major = ch - '0';
            if (r.http_major > 1) {
                return NGX_HTTP_PARSE_INVALID_VERSION;
            }
            state = sw_major_digit;
            break;

        case sw_major_digit:
            if (ch == '.') {
                state = sw_first_minor_digit;
                break;
            }
            if (ch < '0' || ch > '9') {
                return NGX_HTTP_PARSE_INVALID_REQUEST;
            }
            r.http_major = r.http_major * 10 + (ch - '0');
            if (r.http_major > 1) {
                return NGX_HTTP_PARSE_INVALID_VERSION;
            }
            break;

        case sw_first_minor_digit:
            if (ch < '0' || ch > '9') {
                return NGX_HTTP_PARSE_INVALID_REQUEST;
            }
            r.http_minor = ch - '0';
            state = sw_minor_digit;
            break;

        case sw_minor_digit:
            if (ch == '\r') {
                state = sw_almost_done;
                break;
            }
            if (ch == '\n') {
                goto done;
            }
            if (ch == ' ') {
                state = sw_spaces_after_digit;
                break;
            }
            if (ch < '0' || ch > '9') {
                return NGX_HTTP_PARSE_INVALID_REQUEST;
            }
            if (r.http_minor > 99) {
                return NGX_HTTP_PARSE_INVALID_REQUEST;
            }
            r.http_minor = r.http_minor * 10 + (ch - '0');
            break;

        case sw_spaces_after_digit:
            switch (ch) {
            case ' ':
                break;
            case '\r':
                state = sw_almost_done;
                break;
            case '\n':
                goto done;
            default:
                return NGX_HTTP_PARSE_INVALID_REQUEST;
            }
            break;

        case sw_almost_done:
            r.request_end = buffer_offset(b, p) - 1;
            switch (ch) {
            case '\n':
                goto done;
            default:
                return NGX_HTTP_PARSE_INVALID_REQUEST;
            }
        }
    }

    b.pos = p;
    r.state = static_cast<int>(state);
    return NGX_AGAIN;

done:
    b.pos = p + 1;
    if (r.request_end == kInvalidPos) {
        r.request_end = buffer_offset(b, p);
    }
    r.http_version = r.http_major * 1000 + r.http_minor;
    r.state = sw_start;
    if (r.http_version == 9 && !r.method_is_get) {
        return NGX_HTTP_PARSE_INVALID_09_METHOD;
    }
    return NGX_OK;
}

int Http1Parser::parse_header_line(RequestState &r, ParseBuffer &b, bool allow_underscores) {
    unsigned char c = 0;
    unsigned char ch = 0;
    const char *p = nullptr;
    uint32_t hash = 0;
    uint32_t i = 0;
    enum {
        sw_start = 0,
        sw_name,
        sw_space_before_value,
        sw_value,
        sw_space_after_value,
        sw_ignore_line,
        sw_almost_done,
        sw_header_almost_done
    } state;

    static const unsigned char lowcase[] =
        "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
        "\0\0\0\0\0\0\0\0\0\0\0\0\0-\0\0" "0123456789\0\0\0\0\0\0"
        "\0abcdefghijklmnopqrstuvwxyz\0\0\0\0\0"
        "\0abcdefghijklmnopqrstuvwxyz\0\0\0\0\0"
        "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
        "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
        "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
        "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0";

    state = static_cast<decltype(state)>(r.state);
    hash = r.header_hash;
    i = r.lowcase_index;

    for (p = b.pos; p < b.last; p++) {
        ch = static_cast<unsigned char>(*p);

        switch (state) {

        case sw_start:
            r.header_name_start = buffer_offset(b, p);
            r.invalid_header = false;

            switch (ch) {
            case '\r':
                r.header_end = buffer_offset(b, p);
                state = sw_header_almost_done;
                break;
            case '\n':
                r.header_end = buffer_offset(b, p);
                goto header_done;
            default:
                state = sw_name;
                c = lowcase[ch];
                if (c) {
                    hash = ngx_hash(0, c);
                    r.lowcase_header[0] = static_cast<char>(c);
                    i = 1;
                    break;
                }
                if (ch == '_') {
                    if (allow_underscores) {
                        hash = ngx_hash(0, ch);
                        r.lowcase_header[0] = static_cast<char>(ch);
                        i = 1;
                    } else {
                        hash = 0;
                        i = 0;
                        r.invalid_header = true;
                    }
                    break;
                }
                if (ch <= 0x20 || ch == 0x7f || ch == ':') {
                    r.header_end = buffer_offset(b, p);
                    return NGX_HTTP_PARSE_INVALID_HEADER;
                }
                hash = 0;
                i = 0;
                r.invalid_header = true;
                break;
            }
            break;

        case sw_name:
            c = lowcase[ch];
            if (c) {
                hash = ngx_hash(hash, c);
                r.lowcase_header[i++] = static_cast<char>(c);
                i &= (kLowcaseHeaderLen - 1);
                break;
            }
            if (ch == '_') {
                if (allow_underscores) {
                    hash = ngx_hash(hash, ch);
                    r.lowcase_header[i++] = static_cast<char>(ch);
                    i &= (kLowcaseHeaderLen - 1);
                } else {
                    r.invalid_header = true;
                }
                break;
            }
            if (ch == ':') {
                r.header_name_end = buffer_offset(b, p);
                state = sw_space_before_value;
                break;
            }
            if (ch == '\r') {
                r.header_name_end = buffer_offset(b, p);
                r.header_start = buffer_offset(b, p);
                r.header_end = buffer_offset(b, p);
                state = sw_almost_done;
                break;
            }
            if (ch == '\n') {
                r.header_name_end = buffer_offset(b, p);
                r.header_start = buffer_offset(b, p);
                r.header_end = buffer_offset(b, p);
                goto done;
            }
            if (ch == '/' && r.upstream && (p - b.start) - r.header_name_start == 4 &&
                std::memcmp(b.start + r.header_name_start, "HTTP", 4) == 0) {
                state = sw_ignore_line;
                break;
            }
            if (ch <= 0x20 || ch == 0x7f) {
                r.header_end = buffer_offset(b, p);
                return NGX_HTTP_PARSE_INVALID_HEADER;
            }
            r.invalid_header = true;
            break;

        case sw_space_before_value:
            switch (ch) {
            case ' ':
                break;
            case '\r':
                r.header_start = buffer_offset(b, p);
                r.header_end = buffer_offset(b, p);
                state = sw_almost_done;
                break;
            case '\n':
                r.header_start = buffer_offset(b, p);
                r.header_end = buffer_offset(b, p);
                goto done;
            case '\0':
                r.header_end = buffer_offset(b, p);
                return NGX_HTTP_PARSE_INVALID_HEADER;
            default:
                r.header_start = buffer_offset(b, p);
                state = sw_value;
                break;
            }
            break;

        case sw_value:
            switch (ch) {
            case ' ':
                r.header_end = buffer_offset(b, p);
                state = sw_space_after_value;
                break;
            case '\r':
                r.header_end = buffer_offset(b, p);
                state = sw_almost_done;
                break;
            case '\n':
                r.header_end = buffer_offset(b, p);
                goto done;
            case '\0':
                r.header_end = buffer_offset(b, p);
                return NGX_HTTP_PARSE_INVALID_HEADER;
            }
            break;

        case sw_space_after_value:
            switch (ch) {
            case ' ':
                break;
            case '\r':
                state = sw_almost_done;
                break;
            case '\n':
                goto done;
            case '\0':
                r.header_end = buffer_offset(b, p);
                return NGX_HTTP_PARSE_INVALID_HEADER;
            default:
                state = sw_value;
                break;
            }
            break;

        case sw_ignore_line:
            switch (ch) {
            case '\n':
                state = sw_start;
                break;
            default:
                break;
            }
            break;

        case sw_almost_done:
            switch (ch) {
            case '\n':
                goto done;
            case '\r':
                break;
            default:
                return NGX_HTTP_PARSE_INVALID_HEADER;
            }
            break;

        case sw_header_almost_done:
            switch (ch) {
            case '\n':
                goto header_done;
            default:
                return NGX_HTTP_PARSE_INVALID_HEADER;
            }
        }
    }

    b.pos = p;
    r.state = static_cast<int>(state);
    r.header_hash = hash;
    r.lowcase_index = i;
    return NGX_AGAIN;

done:
    b.pos = p + 1;
    r.state = sw_start;
    r.header_hash = hash;
    r.lowcase_index = i;
    return NGX_OK;

header_done:
    b.pos = p + 1;
    r.state = sw_start;
    return NGX_HTTP_PARSE_HEADER_DONE;
}

int Http1Parser::parse_status_line(StatusParseState &state, ParseBuffer &b, StatusLine &status) {
    unsigned char ch = 0;
    const char *p = nullptr;
    enum {
        sw_start = 0,
        sw_H,
        sw_HT,
        sw_HTT,
        sw_HTTP,
        sw_first_major_digit,
        sw_major_digit,
        sw_first_minor_digit,
        sw_minor_digit,
        sw_status,
        sw_space_after_status,
        sw_status_text,
        sw_almost_done
    } s;

    s = static_cast<decltype(s)>(state.state);

    for (p = b.pos; p < b.last; p++) {
        ch = static_cast<unsigned char>(*p);

        switch (s) {
        case sw_start:
            if (ch == 'H') {
                s = sw_H;
                break;
            }
            return NGX_ERROR;

        case sw_H:
            if (ch == 'T') {
                s = sw_HT;
                break;
            }
            return NGX_ERROR;

        case sw_HT:
            if (ch == 'T') {
                s = sw_HTT;
                break;
            }
            return NGX_ERROR;

        case sw_HTT:
            if (ch == 'P') {
                s = sw_HTTP;
                break;
            }
            return NGX_ERROR;

        case sw_HTTP:
            if (ch == '/') {
                s = sw_first_major_digit;
                break;
            }
            return NGX_ERROR;

        case sw_first_major_digit:
            if (ch < '1' || ch > '9') {
                return NGX_ERROR;
            }
            state.http_major = ch - '0';
            s = sw_major_digit;
            break;

        case sw_major_digit:
            if (ch == '.') {
                s = sw_first_minor_digit;
                break;
            }
            if (ch < '0' || ch > '9') {
                return NGX_ERROR;
            }
            if (state.http_major > 99) {
                return NGX_ERROR;
            }
            state.http_major = state.http_major * 10 + (ch - '0');
            break;

        case sw_first_minor_digit:
            if (ch < '0' || ch > '9') {
                return NGX_ERROR;
            }
            state.http_minor = ch - '0';
            s = sw_minor_digit;
            break;

        case sw_minor_digit:
            if (ch == ' ') {
                s = sw_status;
                break;
            }
            if (ch < '0' || ch > '9') {
                return NGX_ERROR;
            }
            if (state.http_minor > 99) {
                return NGX_ERROR;
            }
            state.http_minor = state.http_minor * 10 + (ch - '0');
            break;

        case sw_status:
            if (ch == ' ') {
                break;
            }
            if (ch < '0' || ch > '9') {
                return NGX_ERROR;
            }
            status.code = status.code * 10 + (ch - '0');
            if (++status.count == 3) {
                s = sw_space_after_status;
                status.start = buffer_offset(b, p) - 2;
            }
            break;

        case sw_space_after_status:
            switch (ch) {
            case ' ':
            case '.':
                s = sw_status_text;
                break;
            case '\r':
                s = sw_almost_done;
                break;
            case '\n':
                goto done;
            default:
                return NGX_ERROR;
            }
            break;

        case sw_status_text:
            switch (ch) {
            case '\r':
                s = sw_almost_done;
                break;
            case '\n':
                goto done;
            }
            break;

        case sw_almost_done:
            status.end = buffer_offset(b, p) - 1;
            if (ch == '\n') {
                goto done;
            }
            return NGX_ERROR;
        }
    }

    b.pos = p;
    state.state = static_cast<int>(s);
    return NGX_AGAIN;

done:
    b.pos = p + 1;
    if (status.end == kInvalidPos) {
        status.end = buffer_offset(b, p);
    }
    status.http_version = state.http_major * 1000 + state.http_minor;
    state.state = sw_start;
    return NGX_OK;
}

int Http1Parser::parse_chunked(ChunkedState &ctx, ParseBuffer &b) {
    unsigned char ch = 0;
    unsigned char c = 0;
    const char *pos = nullptr;
    int rc = NGX_AGAIN;
    enum {
        sw_chunk_start = 0,
        sw_chunk_size,
        sw_chunk_extension,
        sw_chunk_extension_almost_done,
        sw_chunk_data,
        sw_after_data,
        sw_after_data_almost_done,
        sw_last_chunk_extension,
        sw_last_chunk_extension_almost_done,
        sw_trailer,
        sw_trailer_almost_done,
        sw_trailer_header,
        sw_trailer_header_almost_done
    } state;

    state = static_cast<decltype(state)>(ctx.state);

    if (state == sw_chunk_data && ctx.size == 0) {
        state = sw_after_data;
    }

    for (pos = b.pos; pos < b.last; pos++) {
        ch = static_cast<unsigned char>(*pos);
        switch (state) {
        case sw_chunk_start:
            if (ch >= '0' && ch <= '9') {
                state = sw_chunk_size;
                ctx.size = ch - '0';
                break;
            }
            c = static_cast<unsigned char>(ch | 0x20u);
            if (c >= 'a' && c <= 'f') {
                state = sw_chunk_size;
                ctx.size = c - 'a' + 10;
                break;
            }
            goto invalid;

        case sw_chunk_size:
            if (ctx.size > kMaxOffT / 16) {
                goto invalid;
            }
            if (ch >= '0' && ch <= '9') {
                ctx.size = ctx.size * 16 + (ch - '0');
                break;
            }
            c = static_cast<unsigned char>(ch | 0x20u);
            if (c >= 'a' && c <= 'f') {
                ctx.size = ctx.size * 16 + (c - 'a' + 10);
                break;
            }
            if (ctx.size == 0) {
                switch (ch) {
                case '\r':
                    state = sw_last_chunk_extension_almost_done;
                    break;
                case '\n':
                    state = sw_trailer;
                    break;
                case ';':
                case ' ':
                case '\t':
                    state = sw_last_chunk_extension;
                    break;
                default:
                    goto invalid;
                }
                break;
            }
            switch (ch) {
            case '\r':
                state = sw_chunk_extension_almost_done;
                break;
            case '\n':
                state = sw_chunk_data;
                break;
            case ';':
            case ' ':
            case '\t':
                state = sw_chunk_extension;
                break;
            default:
                goto invalid;
            }
            break;

        case sw_chunk_extension:
            switch (ch) {
            case '\r':
                state = sw_chunk_extension_almost_done;
                break;
            case '\n':
                state = sw_chunk_data;
            }
            break;

        case sw_chunk_extension_almost_done:
            if (ch == '\n') {
                state = sw_chunk_data;
                break;
            }
            goto invalid;

        case sw_chunk_data:
            rc = NGX_OK;
            goto data;

        case sw_after_data:
            switch (ch) {
            case '\r':
                state = sw_after_data_almost_done;
                break;
            case '\n':
                state = sw_chunk_start;
                break;
            default:
                goto invalid;
            }
            break;

        case sw_after_data_almost_done:
            if (ch == '\n') {
                state = sw_chunk_start;
                break;
            }
            goto invalid;

        case sw_last_chunk_extension:
            switch (ch) {
            case '\r':
                state = sw_last_chunk_extension_almost_done;
                break;
            case '\n':
                state = sw_trailer;
            }
            break;

        case sw_last_chunk_extension_almost_done:
            if (ch == '\n') {
                state = sw_trailer;
                break;
            }
            goto invalid;

        case sw_trailer:
            switch (ch) {
            case '\r':
                state = sw_trailer_almost_done;
                break;
            case '\n':
                goto done;
            default:
                state = sw_trailer_header;
            }
            break;

        case sw_trailer_almost_done:
            if (ch == '\n') {
                goto done;
            }
            goto invalid;

        case sw_trailer_header:
            switch (ch) {
            case '\r':
                state = sw_trailer_header_almost_done;
                break;
            case '\n':
                state = sw_trailer;
            }
            break;

        case sw_trailer_header_almost_done:
            if (ch == '\n') {
                state = sw_trailer;
                break;
            }
            goto invalid;
        }
    }

data:
    ctx.state = static_cast<int>(state);
    b.pos = pos;

    if (ctx.size > kMaxOffT - 5) {
        goto invalid;
    }

    switch (state) {
    case sw_chunk_start:
        ctx.length = 3;
        break;
    case sw_chunk_size:
        ctx.length = 1 + (ctx.size ? ctx.size + 4 : 1);
        break;
    case sw_chunk_extension:
    case sw_chunk_extension_almost_done:
        ctx.length = 1 + ctx.size + 4;
        break;
    case sw_chunk_data:
        ctx.length = ctx.size + 4;
        break;
    case sw_after_data:
    case sw_after_data_almost_done:
        ctx.length = 4;
        break;
    case sw_last_chunk_extension:
    case sw_last_chunk_extension_almost_done:
        ctx.length = 2;
        break;
    case sw_trailer:
    case sw_trailer_almost_done:
        ctx.length = 1;
        break;
    case sw_trailer_header:
    case sw_trailer_header_almost_done:
        ctx.length = 2;
        break;
    }

    return rc;

done:
    ctx.state = 0;
    b.pos = pos + 1;
    return NGX_DONE;

invalid:
    return NGX_ERROR;
}

void Http1Parser::reset_state() {
    parse_error_ = HttpParseError::None;
    headers_complete_ = false;
    message_complete_ = false;
    paused_ = false;
    stage_ = ParseStage::RequestLine;
    header_bytes_ = 0;
    body_bytes_ = 0;
    connection_close_ = false;
    connection_keep_alive_ = false;
    parse_buffer_.clear();
    parse_offset_ = 0;
    reset_request_state();
    reset_chunked_state();
}

void Http1Parser::reset_request_state() {
    request_state_ = RequestState{};
}

void Http1Parser::reset_chunked_state() {
    chunked_state_ = ChunkedState{};
}

} // namespace fiber::http
