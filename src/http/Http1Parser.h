#ifndef FIBER_HTTP_HTTP1_PARSER_H
#define FIBER_HTTP_HTTP1_PARSER_H

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"

namespace fiber::http {

struct HttpServerOptions;
class HttpExchange;

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

enum class ParseCode : int {
    Ok = 0,
    Error = -1,
    Again = -2,
    Done = -3,
    InvalidMethod = -10,
    InvalidRequest = -11,
    InvalidVersion = -12,
    Invalid09Method = -13,
    InvalidHeader = -14,
    HeaderDone = -15,
};

class RequestLineParser : public common::NonCopyable, public common::NonMovable {
public:
    RequestLineParser();

    void reset();

    ParseCode execute(HttpExchange &exchange,
                      const HttpServerOptions &options,
                      const char *data,
                      size_t len,
                      size_t &offset);
    bool carry_over(const char *data,
                    size_t size,
                    size_t pos,
                    char *dst,
                    size_t dst_cap,
                    size_t &dst_size,
                    size_t &dst_pos);

    enum class State {
        Start,
        Method,
        SpacesBeforeUri,
        Schema,
        SchemaSlash,
        SchemaSlashSlash,
        HostStart,
        Host,
        HostEnd,
        HostIpLiteral,
        Port,
        AfterSlashInUri,
        CheckUri,
        Uri,
        Http09,
        HttpH,
        HttpHT,
        HttpHTT,
        HttpHTTP,
        FirstMajorDigit,
        MajorDigit,
        FirstMinorDigit,
        MinorDigit,
        SpacesAfterDigit,
        AlmostDone
    };

    struct RequestLineState {
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
        size_t uri_ext = kInvalidPos;
        size_t http_protocol_start = kInvalidPos;
        int http_major = 0;
        int http_minor = 0;
        int http_version = 0;
        bool method_is_get = false;
        bool complex_uri = false;
        bool quoted_uri = false;
        bool plus_in_uri = false;
        bool empty_path_in_uri = false;
        bool upstream = false;
    };

private:
    static constexpr size_t kInvalidPos = std::numeric_limits<size_t>::max();

    State state_ = State::Start;
    RequestLineState line_{};
};

class HeaderLineParser : public common::NonCopyable, public common::NonMovable {
public:
    HeaderLineParser();

    void reset();

    ParseCode execute(HttpExchange &exchange,
                      const HttpServerOptions &options,
                      const char *data,
                      size_t len,
                      size_t &offset);
    bool carry_over(const char *data,
                    size_t size,
                    size_t pos,
                    char *dst,
                    size_t dst_cap,
                    size_t &dst_size,
                    size_t &dst_pos);

    [[nodiscard]] bool connection_close() const noexcept { return connection_close_; }
    [[nodiscard]] bool connection_keep_alive() const noexcept { return connection_keep_alive_; }
    [[nodiscard]] HttpParseError last_error() const noexcept { return last_error_; }

    enum class State {
        Start,
        Name,
        SpaceBeforeValue,
        Value,
        SpaceAfterValue,
        IgnoreLine,
        AlmostDone,
        HeaderAlmostDone
    };

    static constexpr size_t kLowcaseHeaderLen = 32;

    struct HeaderLineState {
        size_t header_name_start = kInvalidPos;
        size_t header_name_end = kInvalidPos;
        size_t header_start = kInvalidPos;
        size_t header_end = kInvalidPos;
        bool invalid_header = false;
        uint32_t header_hash = 0;
        uint32_t lowcase_index = 0;
        char lowcase_header[kLowcaseHeaderLen]{};
    };

private:
    static constexpr size_t kInvalidPos = std::numeric_limits<size_t>::max();

    State state_ = State::Start;
    HeaderLineState line_{};
    bool connection_close_ = false;
    bool connection_keep_alive_ = false;
    HttpParseError last_error_ = HttpParseError::None;
};

class BodyParser : public common::NonCopyable, public common::NonMovable {
public:
    BodyParser();

    void reset();

    HttpParseResult execute(HttpExchange &exchange,
                            const HttpServerOptions &options,
                            const char *data,
                            size_t len);

    enum class State {
        Init,
        ContentLength,
        Chunked,
        Done
    };

    struct ChunkedState {
        size_t size = 0;
        size_t length = 0;
        int state = 0;
    };

private:
    static constexpr size_t kInvalidPos = std::numeric_limits<size_t>::max();

    State state_ = State::Init;
    std::string parse_buffer_;
    size_t parse_offset_ = 0;
    size_t body_bytes_ = 0;
    ChunkedState chunked_state_{};
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP1_PARSER_H
