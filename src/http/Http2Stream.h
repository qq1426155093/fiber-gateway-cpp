#ifndef FIBER_HTTP_HTTP2_STREAM_H
#define FIBER_HTTP_HTTP2_STREAM_H

#include <cstddef>
#include <cstdint>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "Http2Pending.h"

namespace fiber::http {

class Http2Connection;

class Http2Stream : public common::NonCopyable, public common::NonMovable {
public:
    enum class State : std::uint8_t {
        Idle,
        ReservedLocal,
        ReservedRemote,
        Open,
        HalfClosedLocal,
        HalfClosedRemote,
        Closed,
    };

    explicit Http2Stream(std::uint32_t stream_id) noexcept : stream_id_(stream_id) {}

    enum class ScheduleResult : std::uint8_t {
        NoPending,
        BlockedByStreamWindow,
        BlockedByConnWindow,
        Scheduled,
    };

    [[nodiscard]] std::uint32_t stream_id() const noexcept { return stream_id_; }
    [[nodiscard]] State state() const noexcept { return state_; }
    void set_state(State state) noexcept { state_ = state; }

    [[nodiscard]] bool active() const noexcept { return active_; }
    void set_active(bool active) noexcept { active_ = active; }

    common::IoErr enqueue_pending(Http2Connection &conn, Http2PendingKind kind, Http2SendPayload &&payload,
                                  std::uint8_t first_frame_flags = 0,
                                  std::uint8_t last_frame_flags = 0, Http2PendingEntry::ChangeFn on_change = nullptr,
                                  void *user_ctx = nullptr) noexcept;
    [[nodiscard]] bool has_pending() const noexcept { return pending_head_ != nullptr; }
    [[nodiscard]] Http2PendingKind pending_kind() const noexcept;
    [[nodiscard]] bool blocked_by_stream_window() const noexcept;
    [[nodiscard]] bool blocked_by_conn_window() const noexcept;
    [[nodiscard]] ScheduleResult schedule_pending() noexcept;
    void update_send_window(std::int32_t delta) noexcept;
    void drain_pending(common::IoErr result) noexcept;

private:
    void append_active_pending(Http2PendingEntry &entry) noexcept;
    void remove_active_pending(Http2PendingEntry &entry) noexcept;
    void pop_pending_head() noexcept;
    void maybe_finish_pending(Http2PendingEntry &entry) noexcept;
    void finish_pending(Http2PendingEntry &entry, common::IoErr result) noexcept;
    void refresh_connection_membership() noexcept;
    static void handle_send_done(void *user_data, std::size_t total_bytes, std::size_t written_bytes,
                                 std::size_t frame_header_size, std::size_t logical_bytes,
                                 common::IoErr result) noexcept;

    std::uint32_t stream_id_ = 0;
    State state_ = State::Idle;
    bool active_ = false;
    Http2Connection *conn_ = nullptr;
    std::int32_t send_window_ = 65535;
    Http2Stream *active_prev_ = nullptr;
    Http2Stream *active_next_ = nullptr;
    Http2PendingEntry *pending_head_ = nullptr;
    Http2PendingEntry *pending_tail_ = nullptr;
    Http2PendingEntry *active_pending_head_ = nullptr;
    Http2Stream *pending_prev_ = nullptr;
    Http2Stream *pending_next_ = nullptr;
    bool in_pending_registry_ = false;
    Http2Stream *ready_prev_ = nullptr;
    Http2Stream *ready_next_ = nullptr;
    bool in_ready_list_ = false;
    Http2Stream *conn_wait_prev_ = nullptr;
    Http2Stream *conn_wait_next_ = nullptr;
    bool in_conn_window_wait_list_ = false;

    friend class Http2Connection;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_STREAM_H
