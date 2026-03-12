#ifndef FIBER_HTTP_HTTP2_PROTOCOL_H
#define FIBER_HTTP_HTTP2_PROTOCOL_H

#include <cstdint>

namespace fiber::http {

enum class Http2FrameType : std::uint8_t {
    Data = 0x0,
    Headers = 0x1,
    Priority = 0x2,
    RstStream = 0x3,
    Settings = 0x4,
    PushPromise = 0x5,
    Ping = 0x6,
    Goaway = 0x7,
    WindowUpdate = 0x8,
    Continuation = 0x9,
};

enum class Http2ErrorCode : std::uint32_t {
    NoError = 0x0,
    ProtocolError = 0x1,
    InternalError = 0x2,
    FlowControlError = 0x3,
    SettingsTimeout = 0x4,
    StreamClosed = 0x5,
    FrameSizeError = 0x6,
    RefusedStream = 0x7,
    Cancel = 0x8,
    CompressionError = 0x9,
    ConnectError = 0xa,
    EnhanceYourCalm = 0xb,
    InadequateSecurity = 0xc,
    Http11Required = 0xd,
};

struct Http2FrameHeader {
    std::uint32_t length = 0;
    Http2FrameType type = Http2FrameType::Data;
    std::uint8_t flags = 0;
    std::uint32_t stream_id = 0;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_PROTOCOL_H
