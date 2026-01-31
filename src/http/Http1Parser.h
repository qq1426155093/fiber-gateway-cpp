#ifndef FIBER_HTTP_HTTP1_PARSER_H
#define FIBER_HTTP_HTTP1_PARSER_H

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "HeadBuf.h"
#include "HttpCommon.h"

namespace fiber::http {

struct HttpServerOptions;
class HttpExchange;

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
    HeaderTooLarge = -16,
};

class RequestLineParser : public common::NonCopyable, public common::NonMovable {
public:
    struct RequestLineState {
        std::uint8_t *uri_start{};
        std::uint8_t *uri_end{};
        std::uint8_t *uri_ext{};
        std::uint8_t *args_start{};
        std::uint8_t *request_start{};
        std::uint8_t *request_end{};
        std::uint8_t *method_end{};
        std::uint8_t *schema_start{};
        std::uint8_t *schema_end{};
        std::uint8_t *host_start{};
        std::uint8_t *host_end{};
        std::uint8_t *http_protocol_start{};
        int http_major = 0;
        int http_minor = 0;
        int http_version = 0;
        HttpMethod method = HttpMethod::Unknown;
        bool complex_uri = false;
        bool quoted_uri = false;
        bool plus_in_uri = false;
        bool empty_path_in_uri = false;
    };

public:
    RequestLineParser(const HttpServerOptions &options);

    void reset();

    ParseCode execute(fiber::http::BufChain *buffer);
    // replace the pointers in RequestLineState.
    // the content was copied to the new_buf_start pointer because the limit of the old memory capacity.
    ParseCode replace_buf_ptr(BufChain *old_chain, BufChain *new_chain) noexcept;
    const RequestLineState &state() const noexcept { return line_; }

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


private:
    static constexpr size_t kInvalidPos = std::numeric_limits<size_t>::max();

    const HttpServerOptions *options_ = nullptr;
    State state_ = State::Start;
    RequestLineState line_{};
};

class HeaderLineParser : public common::NonCopyable, public common::NonMovable {
public:
    static constexpr size_t kLowcaseHeaderLen = 32;
    struct HeaderLineState {
        std::uint8_t lowcase_header[kLowcaseHeaderLen]{};
        std::uint8_t *header_name_start{};
        std::uint8_t *header_name_end{};
        std::uint8_t *header_start{};
        std::uint8_t *header_end{};
        bool invalid_header = false;
        uint32_t header_hash = 0;
        uint32_t lowcase_index = 0;
    };

    HeaderLineParser(const HttpServerOptions &options);

    void reset();

    ParseCode execute(BufChain *buffer);
    // replace the pointers in HeaderLineState.
    // the content was copied to the new_buf_start pointer because the limit of the old memory capacity.
    ParseCode replace_buf_ptr(BufChain *old_chain, BufChain *new_chain) noexcept;
    bool header_view(std::string_view &name, std::string_view &value) const noexcept;
    uint32_t header_hash() const noexcept { return line_.header_hash; }
    const HeaderLineState &state() const noexcept { return line_; }

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

private:
    static constexpr size_t kInvalidPos = std::numeric_limits<size_t>::max();

    const HttpServerOptions *options_ = nullptr;
    State state_ = State::Start;
    HeaderLineState line_{};
};

class BodyParser : public common::NonCopyable, public common::NonMovable {
public:
    BodyParser();
    void reset();
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP1_PARSER_H
