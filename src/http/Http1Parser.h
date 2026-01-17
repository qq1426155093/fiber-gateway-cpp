#ifndef FIBER_HTTP_HTTP1_PARSER_H
#define FIBER_HTTP_HTTP1_PARSER_H

#include <cstddef>
#include <string>

#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "HttpExchange.h"

extern "C" {
#include "llhttp.h"
}

namespace fiber::http {

enum class HttpParseState {
    NeedMore,
    HeadersComplete,
    MessageComplete,
    Error,
};

enum class HttpParseError {
    None,
    BadRequest,
    HeadersTooLarge,
    BodyTooLarge,
    ChunkTooLarge,
    UnsupportedTransferEncoding,
};

struct HttpParseResult {
    HttpParseState state = HttpParseState::NeedMore;
    HttpParseError error = HttpParseError::None;
    llhttp_errno_t llhttp_error = HPE_OK;
    size_t consumed = 0;
};

class Http1Parser : public common::NonCopyable, public common::NonMovable {
public:
    Http1Parser();

    void reset(HttpExchange &exchange, const HttpServerOptions &options);
    void resume();

    HttpParseResult execute(const char *data, size_t len);

    [[nodiscard]] bool headers_complete() const noexcept { return headers_complete_; }
    [[nodiscard]] bool message_complete() const noexcept { return message_complete_; }
    [[nodiscard]] bool should_keep_alive() const noexcept;
    [[nodiscard]] HttpParseError last_error() const noexcept { return parse_error_; }

private:
    friend class Http1Connection;

    static int on_message_begin(llhttp_t *parser);
    static int on_method(llhttp_t *parser, const char *at, size_t length);
    static int on_url(llhttp_t *parser, const char *at, size_t length);
    static int on_version(llhttp_t *parser, const char *at, size_t length);
    static int on_header_field(llhttp_t *parser, const char *at, size_t length);
    static int on_header_value(llhttp_t *parser, const char *at, size_t length);
    static int on_header_field_complete(llhttp_t *parser);
    static int on_header_value_complete(llhttp_t *parser);
    static int on_headers_complete(llhttp_t *parser);
    static int on_body(llhttp_t *parser, const char *at, size_t length);
    static int on_chunk_header(llhttp_t *parser);
    static int on_message_complete(llhttp_t *parser);

    int handle_message_begin();
    int handle_method(const char *at, size_t length);
    int handle_url(const char *at, size_t length);
    int handle_version(const char *at, size_t length);
    int handle_header_field(const char *at, size_t length);
    int handle_header_value(const char *at, size_t length);
    int handle_header_field_complete();
    int handle_header_value_complete();
    int handle_headers_complete();
    int handle_body(const char *at, size_t length);
    int handle_chunk_header();
    int handle_message_complete();

    void reset_state();
    int fail(HttpParseError error, const char *reason);

    llhttp_t parser_{};
    llhttp_settings_t settings_{};
    HttpExchange *exchange_ = nullptr;
    const HttpServerOptions *options_ = nullptr;
    std::string current_header_name_;
    std::string current_header_value_;
    size_t header_bytes_ = 0;
    size_t body_bytes_ = 0;
    bool headers_complete_ = false;
    bool message_complete_ = false;
    HttpParseError parse_error_ = HttpParseError::None;
    bool pause_after_headers_ = false;
    bool pause_after_message_ = false;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP1_PARSER_H
