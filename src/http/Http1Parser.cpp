#include "Http1Parser.h"

#include <charconv>

#include "../common/Assert.h"

namespace fiber::http {

namespace {

std::string to_lower_ascii(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (char ch : input) {
        if (ch >= 'A' && ch <= 'Z') {
            out.push_back(static_cast<char>(ch - 'A' + 'a'));
        } else {
            out.push_back(ch);
        }
    }
    return out;
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

Http1Parser::Http1Parser() {
    llhttp_settings_init(&settings_);
    settings_.on_message_begin = &Http1Parser::on_message_begin;
    settings_.on_method = &Http1Parser::on_method;
    settings_.on_url = &Http1Parser::on_url;
    settings_.on_version = &Http1Parser::on_version;
    settings_.on_header_field = &Http1Parser::on_header_field;
    settings_.on_header_value = &Http1Parser::on_header_value;
    settings_.on_header_field_complete = &Http1Parser::on_header_field_complete;
    settings_.on_header_value_complete = &Http1Parser::on_header_value_complete;
    settings_.on_headers_complete = &Http1Parser::on_headers_complete;
    settings_.on_body = &Http1Parser::on_body;
    settings_.on_chunk_header = &Http1Parser::on_chunk_header;
    settings_.on_message_complete = &Http1Parser::on_message_complete;
}

void Http1Parser::reset(HttpExchange &exchange, const HttpServerOptions &options) {
    exchange_ = &exchange;
    options_ = &options;
    reset_state();
    llhttp_init(&parser_, HTTP_REQUEST, &settings_);
    parser_.data = this;
}

void Http1Parser::resume() {
    llhttp_resume(&parser_);
}

HttpParseResult Http1Parser::execute(const char *data, size_t len) {
    HttpParseResult result{};
    if (!data || len == 0) {
        result.state = HttpParseState::NeedMore;
        return result;
    }
    pause_after_headers_ = false;
    pause_after_message_ = false;
    llhttp_errno_t err = llhttp_execute(&parser_, data, len);
    result.llhttp_error = err;
    if (err == HPE_OK) {
        result.consumed = len;
        result.state = HttpParseState::NeedMore;
        if (headers_complete_) {
            result.state = HttpParseState::HeadersComplete;
        }
        if (message_complete_) {
            result.state = HttpParseState::MessageComplete;
        }
        return result;
    }

    const char *pos = llhttp_get_error_pos(&parser_);
    if (pos && pos >= data && pos <= data + len) {
        result.consumed = static_cast<size_t>(pos - data);
    } else {
        result.consumed = 0;
    }

    if (err == HPE_PAUSED) {
        if (pause_after_headers_) {
            result.state = HttpParseState::HeadersComplete;
        } else if (pause_after_message_) {
            result.state = HttpParseState::MessageComplete;
        } else {
            result.state = HttpParseState::NeedMore;
        }
        return result;
    }

    result.state = HttpParseState::Error;
    if (parse_error_ == HttpParseError::None) {
        result.error = HttpParseError::BadRequest;
    } else {
        result.error = parse_error_;
    }
    return result;
}

bool Http1Parser::should_keep_alive() const noexcept {
    return llhttp_should_keep_alive(&parser_) != 0;
}

int Http1Parser::on_message_begin(llhttp_t *parser) {
    auto *self = static_cast<Http1Parser *>(parser->data);
    return self ? self->handle_message_begin() : HPE_USER;
}

int Http1Parser::on_method(llhttp_t *parser, const char *at, size_t length) {
    auto *self = static_cast<Http1Parser *>(parser->data);
    return self ? self->handle_method(at, length) : HPE_USER;
}

int Http1Parser::on_url(llhttp_t *parser, const char *at, size_t length) {
    auto *self = static_cast<Http1Parser *>(parser->data);
    return self ? self->handle_url(at, length) : HPE_USER;
}

int Http1Parser::on_version(llhttp_t *parser, const char *at, size_t length) {
    auto *self = static_cast<Http1Parser *>(parser->data);
    return self ? self->handle_version(at, length) : HPE_USER;
}

int Http1Parser::on_header_field(llhttp_t *parser, const char *at, size_t length) {
    auto *self = static_cast<Http1Parser *>(parser->data);
    return self ? self->handle_header_field(at, length) : HPE_USER;
}

int Http1Parser::on_header_value(llhttp_t *parser, const char *at, size_t length) {
    auto *self = static_cast<Http1Parser *>(parser->data);
    return self ? self->handle_header_value(at, length) : HPE_USER;
}

int Http1Parser::on_header_field_complete(llhttp_t *parser) {
    auto *self = static_cast<Http1Parser *>(parser->data);
    return self ? self->handle_header_field_complete() : HPE_USER;
}

int Http1Parser::on_header_value_complete(llhttp_t *parser) {
    auto *self = static_cast<Http1Parser *>(parser->data);
    return self ? self->handle_header_value_complete() : HPE_USER;
}

int Http1Parser::on_headers_complete(llhttp_t *parser) {
    auto *self = static_cast<Http1Parser *>(parser->data);
    return self ? self->handle_headers_complete() : HPE_USER;
}

int Http1Parser::on_body(llhttp_t *parser, const char *at, size_t length) {
    auto *self = static_cast<Http1Parser *>(parser->data);
    return self ? self->handle_body(at, length) : HPE_USER;
}

int Http1Parser::on_chunk_header(llhttp_t *parser) {
    auto *self = static_cast<Http1Parser *>(parser->data);
    return self ? self->handle_chunk_header() : HPE_USER;
}

int Http1Parser::on_message_complete(llhttp_t *parser) {
    auto *self = static_cast<Http1Parser *>(parser->data);
    return self ? self->handle_message_complete() : HPE_USER;
}

int Http1Parser::handle_message_begin() {
    return 0;
}

int Http1Parser::handle_method(const char *at, size_t length) {
    header_bytes_ += length;
    if (options_ && header_bytes_ > options_->max_header_bytes) {
        return fail(HttpParseError::HeadersTooLarge, "headers too large");
    }
    if (exchange_) {
        exchange_->method_.append(at, length);
    }
    return 0;
}

int Http1Parser::handle_url(const char *at, size_t length) {
    header_bytes_ += length;
    if (options_ && header_bytes_ > options_->max_header_bytes) {
        return fail(HttpParseError::HeadersTooLarge, "headers too large");
    }
    if (exchange_) {
        exchange_->target_.append(at, length);
    }
    return 0;
}

int Http1Parser::handle_version(const char *at, size_t length) {
    header_bytes_ += length;
    if (options_ && header_bytes_ > options_->max_header_bytes) {
        return fail(HttpParseError::HeadersTooLarge, "headers too large");
    }
    (void) at;
    (void) length;
    return 0;
}

int Http1Parser::handle_header_field(const char *at, size_t length) {
    header_bytes_ += length;
    if (options_ && header_bytes_ > options_->max_header_bytes) {
        return fail(HttpParseError::HeadersTooLarge, "headers too large");
    }
    current_header_name_.append(at, length);
    return 0;
}

int Http1Parser::handle_header_value(const char *at, size_t length) {
    header_bytes_ += length;
    if (options_ && header_bytes_ > options_->max_header_bytes) {
        return fail(HttpParseError::HeadersTooLarge, "headers too large");
    }
    current_header_value_.append(at, length);
    return 0;
}

int Http1Parser::handle_header_field_complete() {
    return 0;
}

int Http1Parser::handle_header_value_complete() {
    if (!exchange_) {
        return 0;
    }
    std::string name = to_lower_ascii(current_header_name_);
    std::string value = current_header_value_;
    exchange_->request_headers_.push_back(HttpHeader{std::move(name), std::move(value)});

    const HttpHeader &last = exchange_->request_headers_.back();
    if (last.name == "content-length") {
        std::string_view val = last.value;
        size_t length = 0;
        auto result = std::from_chars(val.data(), val.data() + val.size(), length);
        if (result.ec != std::errc() || result.ptr != val.data() + val.size()) {
            return fail(HttpParseError::BadRequest, "invalid content-length");
        }
        if (exchange_->request_content_length_set_ &&
            exchange_->request_content_length_ != length) {
            return fail(HttpParseError::BadRequest, "conflicting content-length");
        }
        exchange_->request_content_length_ = length;
        exchange_->request_content_length_set_ = true;
    } else if (last.name == "transfer-encoding") {
        bool chunked = false;
        if (!parse_transfer_encoding(last.value, chunked)) {
            return fail(HttpParseError::UnsupportedTransferEncoding, "unsupported transfer-encoding");
        }
        exchange_->request_chunked_ = chunked;
    } else if (last.name == "expect") {
        if (has_token(last.value, "100-continue")) {
            exchange_->request_expect_continue_ = true;
        }
    }

    current_header_name_.clear();
    current_header_value_.clear();
    return 0;
}

int Http1Parser::handle_headers_complete() {
    if (!exchange_) {
        return 0;
    }
    exchange_->version_.clear();
    exchange_->version_.append("HTTP/");
    exchange_->version_.append(std::to_string(llhttp_get_http_major(&parser_)));
    exchange_->version_.append(".");
    exchange_->version_.append(std::to_string(llhttp_get_http_minor(&parser_)));
    exchange_->request_keep_alive_ = should_keep_alive();
    if ((parser_.flags & F_TRANSFER_ENCODING) && !(parser_.flags & F_CHUNKED)) {
        return fail(HttpParseError::UnsupportedTransferEncoding, "unsupported transfer-encoding");
    }
    exchange_->request_chunked_ = (parser_.flags & F_CHUNKED) != 0;
    if (exchange_->request_chunked_) {
        exchange_->request_content_length_ = 0;
        exchange_->request_content_length_set_ = false;
    } else if ((parser_.flags & F_CONTENT_LENGTH) != 0) {
        exchange_->request_content_length_ = static_cast<size_t>(parser_.content_length);
        exchange_->request_content_length_set_ = true;
    }
    if (options_ && exchange_->request_content_length_ > options_->max_body_bytes) {
        return fail(HttpParseError::BodyTooLarge, "body too large");
    }
    if (!exchange_->request_chunked_ && exchange_->request_content_length_ == 0) {
        exchange_->body_complete_ = true;
    }
    headers_complete_ = true;
    pause_after_headers_ = true;
    return HPE_PAUSED;
}

int Http1Parser::handle_body(const char *at, size_t length) {
    if (options_ && body_bytes_ + length > options_->max_body_bytes) {
        return fail(HttpParseError::BodyTooLarge, "body too large");
    }
    body_bytes_ += length;
    if (exchange_) {
        exchange_->body_buffer_.append(at, length);
    }
    return 0;
}

int Http1Parser::handle_chunk_header() {
    if (options_ && parser_.content_length > options_->max_chunk_bytes) {
        return fail(HttpParseError::ChunkTooLarge, "chunk too large");
    }
    return 0;
}

int Http1Parser::handle_message_complete() {
    if (exchange_) {
        exchange_->body_complete_ = true;
    }
    message_complete_ = true;
    pause_after_message_ = true;
    return HPE_PAUSED;
}

void Http1Parser::reset_state() {
    current_header_name_.clear();
    current_header_value_.clear();
    header_bytes_ = 0;
    body_bytes_ = 0;
    headers_complete_ = false;
    message_complete_ = false;
    parse_error_ = HttpParseError::None;
    pause_after_headers_ = false;
    pause_after_message_ = false;
}

int Http1Parser::fail(HttpParseError error, const char *reason) {
    parse_error_ = error;
    llhttp_set_error_reason(&parser_, reason);
    return HPE_USER;
}

} // namespace fiber::http
