#ifndef FIBER_HTTP_HTTP2_STREAM_H
#define FIBER_HTTP_HTTP2_STREAM_H

#include <cstddef>
#include <cstdint>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../common/mem/IoBuf.h"
#include "Http2Pending.h"
#include "Http2Protocol.h"

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

    enum class StreamOp : std::uint8_t {
        RecvHeaders = 0,
        RecvHeadersEndStream,
        RecvData,
        RecvDataEndStream,
        SendHeaders,
        SendHeadersEndStream,
        SendData,
        SendDataEndStream,
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
    [[nodiscard]] std::int32_t send_window() const noexcept { return send_window_; }

    [[nodiscard]] bool active() const noexcept { return active_; }
    void set_active(bool active) noexcept { active_ = active; }

    common::IoErr enqueue_pending(Http2PendingKind kind, Http2SendPayload &&payload, std::uint8_t first_frame_flags = 0,
                                  std::uint8_t last_frame_flags = 0, Http2PendingEntry::ChangeFn on_change = nullptr,
                                  void *user_ctx = nullptr) noexcept;
    [[nodiscard]] bool has_pending() const noexcept { return pending_head_ != nullptr; }
    [[nodiscard]] Http2PendingKind pending_kind() const noexcept;
    [[nodiscard]] bool blocked_by_stream_window() const noexcept;
    [[nodiscard]] bool blocked_by_conn_window() const noexcept;
    [[nodiscard]] ScheduleResult schedule_pending() noexcept;
    common::IoErr on_header_recv(const mem::IoBuf &payload, std::size_t block_offset, std::size_t length,
                                 bool end_headers, bool end_stream) noexcept;
    common::IoErr on_data_recv(const mem::IoBuf &payload, std::size_t data_offset, std::size_t length,
                               bool end_stream) noexcept;
    void on_remote_rst(Http2ErrorCode code, common::IoErr result = common::IoErr::Canceled) noexcept;
    common::IoErr send_header(Http2SendPayload &&payload, bool end_stream) noexcept;
    common::IoErr send_data(Http2SendPayload &&payload, bool end_stream) noexcept;
    common::IoErr close_rst(Http2ErrorCode code, common::IoErr result = common::IoErr::Canceled) noexcept;
    // Peer SETTINGS_INITIAL_WINDOW_SIZE can shrink after we have already
    // reserved/sent DATA on this stream, so the per-stream send window is
    // allowed to become negative until future WINDOW_UPDATE frames restore it.
    void update_send_window(std::int32_t delta) noexcept;
    void drain_pending(common::IoErr result) noexcept;
    void close(common::IoErr result = common::IoErr::Canceled) noexcept;

private:
    common::IoErr transition_on_recv_headers(bool end_stream) noexcept;
    common::IoErr transition_on_recv_data(bool end_stream) noexcept;
    common::IoErr transition_on_send_headers(bool end_stream) noexcept;
    common::IoErr transition_on_send_data(bool end_stream) noexcept;
    [[nodiscard]] static bool is_valid_transition(State state, StreamOp op) noexcept;
    void transition_on_remote_end_stream() noexcept;
    void transition_on_local_end_stream() noexcept;
    void append_active_pending(Http2PendingEntry &entry) noexcept;
    void remove_active_pending(Http2PendingEntry &entry) noexcept;
    void pop_pending_head() noexcept;
    void maybe_finish_pending(Http2PendingEntry &entry) noexcept;
    void finish_pending(Http2PendingEntry &entry, common::IoErr result) noexcept;
    void try_schedule_pending() noexcept;
    static void handle_send_done(void *user_data, std::size_t total_bytes, std::size_t written_bytes,
                                 std::size_t frame_header_size, std::size_t logical_bytes,
                                 common::IoErr result) noexcept;

    std::uint32_t stream_id_ = 0;
    State state_ = State::Idle;
    bool active_ = false;
    Http2Connection *conn_ = nullptr;
    // RFC 7540 allows the stream-level send window to become negative after a
    // smaller SETTINGS_INITIAL_WINDOW_SIZE is applied to in-flight streams.
    std::int32_t send_window_ = 65535;
    Http2Stream *active_prev_ = nullptr;
    Http2Stream *active_next_ = nullptr;
    Http2PendingEntry *pending_head_ = nullptr;
    Http2PendingEntry *pending_tail_ = nullptr;
    Http2PendingEntry *active_pending_head_ = nullptr;
    Http2Stream *conn_wait_prev_ = nullptr;
    Http2Stream *conn_wait_next_ = nullptr;
    bool in_conn_window_wait_list_ = false;
    bool connection_owned_ = false;
    Http2Stream *owned_next_ = nullptr;

    friend class Http2Connection;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_STREAM_H
