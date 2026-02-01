//
// Created by dear on 2026/2/1.
//

#ifndef FIBER_HTTP1BODY_H
#define FIBER_HTTP1BODY_H
#include "HeadBuf.h"

namespace fiber::http {


class ChunkParser {
public:
    using off_t = long int;
    enum class ParseCode {
        Ok = 0,
        Error = -1,
        Again = -2,
        Done = -3,
    };

    void reset() noexcept;
    [[nodiscard]] off_t size() const noexcept { return size_; }
    [[nodiscard]] off_t length() const noexcept { return length_; }
    void consume(off_t n) noexcept;
    ParseCode execute(BufChain *chain) noexcept;

private:
    enum class State {
        ChunkStart = 0,
        ChunkSize,
        ChunkExtension,
        ChunkExtensionAlmostDone,
        ChunkData,
        AfterData,
        AfterDataAlmostDone,
        LastChunkExtension,
        LastChunkExtensionAlmostDone,
        Trailer,
        TrailerAlmostDone,
        TrailerHeader,
        TrailerHeaderAlmostDone
    };

    State state_;
    off_t size_;
    off_t length_;
};

class Http1Body {
public:
    enum class Type {
        H09_EOF,
        H1x_Body_Length,
        H11_CHUNK,
        H20, // unimplemented
        H30 // unimplemented
    };

private:
    Type type_;
    union {
        ChunkParser chunk_parser_;
        // content_length
        // http_v2
        // http_v3
    };
};

} // namespace fiber::http


#endif // FIBER_HTTP1BODY_H
