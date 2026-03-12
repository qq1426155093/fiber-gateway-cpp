#ifndef FIBER_HTTP_HTTP2_SEND_PAYLOAD_H
#define FIBER_HTTP_HTTP2_SEND_PAYLOAD_H

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/mem/IoBuf.h"

namespace fiber::http {

class HttpTransport;

struct Http2StableSpan {
    const std::uint8_t *data = nullptr;
    std::size_t length = 0;
    std::size_t offset = 0;
};

class Http2SendPayload {
public:
    enum class Kind : std::uint8_t {
        None,
        StableSpan,
        IoBuf,
        IoBufChain,
    };

    Http2SendPayload() noexcept = default;
    ~Http2SendPayload();

    Http2SendPayload(const Http2SendPayload &) = delete;
    Http2SendPayload &operator=(const Http2SendPayload &) = delete;

    Http2SendPayload(Http2SendPayload &&other) noexcept;
    Http2SendPayload &operator=(Http2SendPayload &&other) noexcept;

    [[nodiscard]] Kind kind() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t readable_bytes() const noexcept;

    void reset() noexcept;
    void set_stable_span(const std::uint8_t *data, std::size_t length) noexcept;
    void set_buf(mem::IoBuf &&buf) noexcept;
    void set_chain(mem::IoBufChain &&bufs) noexcept;
    [[nodiscard]] bool split_prefix_to(std::size_t bytes, Http2SendPayload &out) noexcept;
    fiber::async::Task<common::IoResult<size_t>> write_once(HttpTransport &transport,
                                                            std::chrono::milliseconds timeout) noexcept;

private:
    union Storage {
        Storage() {}
        ~Storage() {}

        Http2StableSpan span;
        mem::IoBuf buf;
        mem::IoBufChain chain;
    };

    void move_from(Http2SendPayload &&other) noexcept;
    [[nodiscard]] Http2StableSpan &span() noexcept;
    [[nodiscard]] const Http2StableSpan &span() const noexcept;
    [[nodiscard]] mem::IoBuf &buf() noexcept;
    [[nodiscard]] const mem::IoBuf &buf() const noexcept;
    [[nodiscard]] mem::IoBufChain &chain() noexcept;
    [[nodiscard]] const mem::IoBufChain &chain() const noexcept;

    Storage storage_{};
    Kind kind_ = Kind::None;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_SEND_PAYLOAD_H
