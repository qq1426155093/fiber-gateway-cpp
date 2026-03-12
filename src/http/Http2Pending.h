#ifndef FIBER_HTTP_HTTP2_PENDING_H
#define FIBER_HTTP_HTTP2_PENDING_H

#include <cstddef>
#include <cstdint>

#include "../common/IoError.h"
#include "Http2SendPayload.h"

namespace fiber::http {

class Http2Stream;

enum class Http2PendingKind : std::uint8_t {
    Header,
    Data,
};

struct Http2PendingChange {
    enum class Kind : std::uint8_t {
        Scheduled,
        Written,
        Completed,
        Failed,
        Canceled,
    };

    Kind kind = Kind::Scheduled;
    std::size_t delta_bytes = 0;
    std::size_t total_bytes = 0;
    std::size_t written_bytes = 0;
    common::IoErr result = common::IoErr::None;
};

struct Http2PendingEntry {
    using ChangeFn = void (*)(Http2PendingEntry *entry, const Http2PendingChange &change) noexcept;

    Http2PendingEntry *next = nullptr;
    Http2PendingEntry *active_prev = nullptr;
    Http2PendingEntry *active_next = nullptr;
    Http2Stream *stream = nullptr;
    std::uint32_t stream_id = 0;
    Http2PendingKind kind = Http2PendingKind::Data;
    Http2SendPayload payload;
    std::uint8_t first_frame_flags = 0;
    std::uint8_t last_frame_flags = 0;
    std::size_t total_bytes = 0;
    std::size_t written_bytes = 0;
    std::size_t inflight_sends = 0;
    common::IoErr result = common::IoErr::None;
    bool terminal_notified = false;
    ChangeFn on_change = nullptr;
    void *user_ctx_ = nullptr;

    void init(Http2Stream &owner, std::uint32_t next_stream_id, Http2PendingKind next_kind,
              Http2SendPayload &&next_payload, std::uint8_t next_first_frame_flags,
              std::uint8_t next_last_frame_flags, ChangeFn next_on_change, void *next_user_ctx) noexcept {
        reset();
        stream = &owner;
        stream_id = next_stream_id;
        kind = next_kind;
        payload = std::move(next_payload);
        first_frame_flags = next_first_frame_flags;
        last_frame_flags = next_last_frame_flags;
        total_bytes = payload.readable_bytes();
        on_change = next_on_change;
        user_ctx_ = next_user_ctx;
    }

    void reset() noexcept {
        next = nullptr;
        active_prev = nullptr;
        active_next = nullptr;
        stream = nullptr;
        stream_id = 0;
        kind = Http2PendingKind::Data;
        payload.reset();
        first_frame_flags = 0;
        last_frame_flags = 0;
        total_bytes = 0;
        written_bytes = 0;
        inflight_sends = 0;
        result = common::IoErr::None;
        terminal_notified = false;
        on_change = nullptr;
        user_ctx_ = nullptr;
    }

    [[nodiscard]] std::size_t remaining_bytes() const noexcept { return payload.readable_bytes(); }

    void notify_change(Http2PendingChange::Kind next_kind, std::size_t delta_bytes, common::IoErr next_result) noexcept {
        if (!on_change) {
            return;
        }
        Http2PendingChange change;
        change.kind = next_kind;
        change.delta_bytes = delta_bytes;
        change.total_bytes = total_bytes;
        change.written_bytes = written_bytes;
        change.result = next_result;
        on_change(this, change);
    }
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_PENDING_H
