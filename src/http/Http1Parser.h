#ifndef FIBER_HTTP_HTTP1_PARSER_H
#define FIBER_HTTP_HTTP1_PARSER_H

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "HttpExchange.h"

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

    static constexpr size_t kInvalidPos = std::numeric_limits<size_t>::max();
    static constexpr size_t kLowcaseHeaderLen = 32;

    enum class ParseStage {
        RequestLine,
        Headers,
        Body,
    };

    struct RequestState {
        size_t request_start = kInvalidPos;
        size_t method_end = kInvalidPos;
        size_t uri_start = kInvalidPos;
        size_t uri_end = kInvalidPos;
        size_t schema_start = kInvalidPos;
        size_t schema_end = kInvalidPos;
        size_t host_start = kInvalidPos;
        size_t host_end = kInvalidPos;
        size_t port_start = kInvalidPos;
        size_t port_end = kInvalidPos;
        size_t args_start = kInvalidPos;
        size_t request_end = kInvalidPos;
        size_t header_name_start = kInvalidPos;
        size_t header_name_end = kInvalidPos;
        size_t header_start = kInvalidPos;
        size_t header_end = kInvalidPos;
        size_t uri_ext = kInvalidPos;
        size_t http_protocol_start = kInvalidPos;
        int state = 0;
        int http_major = 0;
        int http_minor = 0;
        int http_version = 0;
        bool invalid_header = false;
        bool method_is_get = false;
        bool complex_uri = false;
        bool quoted_uri = false;
        bool plus_in_uri = false;
        bool empty_path_in_uri = false;
        bool upstream = false;
        uint32_t header_hash = 0;
        uint32_t lowcase_index = 0;
        char lowcase_header[kLowcaseHeaderLen]{};
    };

    struct ChunkedState {
        size_t size = 0;
        size_t length = 0;
        int state = 0;
    };

    struct ParseBuffer {
        const char *start = nullptr;
        const char *pos = nullptr;
        const char *last = nullptr;
    };

    struct StatusParseState {
        int state = 0;
        int http_major = 0;
        int http_minor = 0;
    };

    struct StatusLine {
        int code = 0;
        int count = 0;
        size_t start = kInvalidPos;
        size_t end = kInvalidPos;
        int http_version = 0;
    };

    static size_t buffer_offset(const ParseBuffer &buffer, const char *p) noexcept;
    static int parse_request_line(RequestState &request, ParseBuffer &buffer);
    static int parse_header_line(RequestState &request, ParseBuffer &buffer, bool allow_underscores);
    static int parse_status_line(StatusParseState &state, ParseBuffer &buffer, StatusLine &status);
    static int parse_chunked(ChunkedState &state, ParseBuffer &buffer);

    void reset_state();
    void reset_request_state();
    void reset_chunked_state();

    HttpParseError parse_error_ = HttpParseError::None;
    HttpExchange *exchange_ = nullptr;
    const HttpServerOptions *options_ = nullptr;
    std::string parse_buffer_;
    size_t parse_offset_ = 0;
    ParseStage stage_ = ParseStage::RequestLine;
    RequestState request_state_{};
    ChunkedState chunked_state_{};
    size_t header_bytes_ = 0;
    size_t body_bytes_ = 0;
    bool headers_complete_ = false;
    bool message_complete_ = false;
    bool paused_ = false;
    bool connection_close_ = false;
    bool connection_keep_alive_ = false;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP1_PARSER_H
