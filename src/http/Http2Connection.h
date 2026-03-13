#ifndef FIBER_HTTP_HTTP2_CONNECTION_H
#define FIBER_HTTP_HTTP2_CONNECTION_H

#include <chrono>
#include <array>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <coroutine>
#include <memory>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../common/mem/IoBuf.h"
#include "Http2Pending.h"
#include "Http2PendingPool.h"
#include "Http2Protocol.h"
#include "Http2SendingEntryQueue.h"
#include "Http2Stream.h"
#include "Http2StreamTable.h"
#include "HttpTransport.h"

namespace fiber::http {

class Http2Connection : public common::NonCopyable, public common::NonMovable {
public:
    enum class ConnectionRole : std::uint8_t {
        Client,
        Server,
    };

    using FrameHeader = Http2FrameHeader;
    using RunResult = common::IoResult<void>;
    using SendResult = common::IoResult<void>;
    using StableSpan = Http2StableSpan;
    using SendPayload = Http2SendPayload;
    using SendEntry = Http2SendingEntry;
    using PendingEntry = Http2PendingEntry;
    using PendingChange = Http2PendingChange;
    using PendingKind = Http2PendingKind;

    struct Options {
        ConnectionRole role = ConnectionRole::Server;
        std::size_t read_buffer_size = 64 * 1024;
        std::chrono::milliseconds read_timeout = std::chrono::seconds(30);
        std::chrono::milliseconds write_timeout = std::chrono::seconds(30);
        std::chrono::milliseconds keepalive_ping_interval = std::chrono::milliseconds::zero();
        std::uint32_t max_frame_size = 16384;
        std::size_t max_free_send_entries = 64;
        std::size_t max_free_pending_entries = 64;
        std::uint32_t max_peer_concurrent_streams = 100;
        std::uint32_t local_max_concurrent_streams = 128;
        std::uint32_t max_local_push_streams = 0;
        std::uint32_t initial_connection_recv_window = 0x7fffffffU;
        std::int32_t initial_connection_send_window = 65535;
        std::int32_t initial_stream_send_window = 65535;
        bool expect_peer_preface = true;
        bool auto_start_connection_preface = true;
    };

    virtual ~Http2Connection();

    Http2Connection(std::unique_ptr<HttpTransport> transport);
    Http2Connection(std::unique_ptr<HttpTransport> transport, Options options);

    fiber::async::Task<RunResult> run() noexcept;
    Http2Stream *create_local_stream(std::uint32_t stream_id) noexcept;

protected:
    // `offset` is the number of payload bytes already delivered for the current
    // frame. Only the first `length` bytes starting at `buf.readable_data()`
    // are part of this callback's payload chunk.
    virtual common::IoErr on_frame_payload(const FrameHeader &fhr, const mem::IoBuf &buf, std::size_t offset,
                                           std::size_t length) noexcept;

    Http2Stream *find_stream(std::uint32_t stream_id) noexcept;
    const Http2Stream *find_stream(std::uint32_t stream_id) const noexcept;
    [[nodiscard]] SendEntry *acquire_send_entry() noexcept;
    void release_send_entry(SendEntry *entry) noexcept;
    [[nodiscard]] common::IoErr enqueue_send_entry(SendEntry *entry) noexcept;
    void update_connection_send_window(std::int32_t delta) noexcept;
    void stop_sending(common::IoErr reason = common::IoErr::Canceled) noexcept;
    [[nodiscard]] std::int32_t connection_send_window() const noexcept { return conn_send_window_; }
    [[nodiscard]] std::uint32_t peer_max_outbound_frame_size() const noexcept { return peer_max_outbound_frame_size_; }
    [[nodiscard]] std::uint32_t peer_max_concurrent_streams() const noexcept {
        return peer_advertised_max_concurrent_streams_;
    }
    [[nodiscard]] ConnectionRole role() const noexcept { return options_.role; }
    [[nodiscard]] bool peer_enable_push() const noexcept { return peer_enable_push_; }
    [[nodiscard]] bool has_stream(std::uint32_t stream_id) const noexcept { return streams_.find(stream_id) != nullptr; }
    [[nodiscard]] bool send_loop_exited() const noexcept { return !send_loop_running_; }
    fiber::async::Task<void> stop_and_join_send_loop(common::IoErr reason = common::IoErr::Canceled) noexcept;

private:
    common::IoErr consume_incoming_frame_payload(const FrameHeader &fhr, const mem::IoBuf &buf, std::size_t offset,
                                                 std::size_t length) noexcept;
    common::IoErr handle_data_payload(const FrameHeader &fhr, const mem::IoBuf &buf, std::size_t offset,
                                      std::size_t length) noexcept;
    common::IoErr handle_headers_payload(const FrameHeader &fhr, const mem::IoBuf &buf, std::size_t offset,
                                         std::size_t length) noexcept;
    common::IoErr handle_continuation_payload(const FrameHeader &fhr, const mem::IoBuf &buf, std::size_t offset,
                                              std::size_t length) noexcept;
    common::IoErr handle_settings_payload(const FrameHeader &fhr, const mem::IoBuf &buf, std::size_t offset,
                                          std::size_t length) noexcept;
    common::IoErr handle_ping_payload(const FrameHeader &fhr, const mem::IoBuf &buf, std::size_t offset,
                                      std::size_t length) noexcept;
    common::IoErr handle_window_update_payload(const FrameHeader &fhr, const mem::IoBuf &buf, std::size_t offset,
                                               std::size_t length) noexcept;
    common::IoErr handle_rst_stream_payload(const FrameHeader &fhr, const mem::IoBuf &buf, std::size_t offset,
                                            std::size_t length) noexcept;
    common::IoErr handle_goaway_payload(const FrameHeader &fhr, const mem::IoBuf &buf, std::size_t offset,
                                        std::size_t length) noexcept;
    common::IoErr apply_settings_parameter(std::uint16_t id, std::uint32_t value) noexcept;
    common::IoErr apply_peer_initial_stream_window(std::uint32_t value) noexcept;
    common::IoErr send_control_frame(Http2FrameType type, std::uint8_t flags, std::uint32_t stream_id,
                                     const std::uint8_t *payload, std::size_t length) noexcept;
    common::IoErr send_connection_preface() noexcept;
    common::IoErr send_settings_ack() noexcept;
    common::IoErr send_ping_ack(const std::uint8_t *opaque_data) noexcept;
    common::IoErr send_window_update(std::uint32_t stream_id, std::uint32_t increment) noexcept;
    common::IoErr send_rst_stream(std::uint32_t stream_id, Http2ErrorCode error_code) noexcept;
    void handle_stream_error(std::uint32_t stream_id, Http2ErrorCode error_code,
                             common::IoErr pending_result = common::IoErr::Canceled) noexcept;
    Http2Stream *create_peer_stream(std::uint32_t stream_id) noexcept;
    void erase_stream(Http2Stream &stream) noexcept;
    void maybe_destroy_stream(Http2Stream &stream) noexcept;
    bool can_accept_peer_stream(std::uint32_t stream_id) const noexcept;
    bool can_create_local_stream(std::uint32_t stream_id) const noexcept;
    bool is_next_peer_stream_id(std::uint32_t stream_id) const noexcept;
    bool is_next_local_stream_id(std::uint32_t stream_id) const noexcept;
    void on_peer_goaway(std::uint32_t last_stream_id, Http2ErrorCode error_code) noexcept;
    void close_streams_after_goaway(std::uint32_t last_stream_id) noexcept;
    [[nodiscard]] bool is_idle_stream(std::uint32_t stream_id) const noexcept;
    [[nodiscard]] bool is_local_stream_id(std::uint32_t stream_id) const noexcept;
    [[nodiscard]] bool is_peer_stream_id(std::uint32_t stream_id) const noexcept;
    [[nodiscard]] std::size_t configured_max_active_streams() const noexcept;
    fiber::async::Task<RunResult> finish_run(RunResult result) noexcept;
    fiber::async::Task<void> run_send_loop() noexcept;
    void start_send_loop() noexcept;
    [[nodiscard]] std::chrono::milliseconds send_loop_poll_timeout() const noexcept;
    void handle_send_loop_timeout() noexcept;
    void drain_conn_blocked_streams() noexcept;
    void finish_send_entry(SendEntry *entry, common::IoErr result) noexcept;
    void drain_send_queue(common::IoErr result) noexcept;
    void notify_send_done(SendEntry *entry) noexcept;
    void close_all_streams(common::IoErr result) noexcept;
    void append_conn_wait_stream(Http2Stream &stream) noexcept;
    void remove_conn_wait_stream(Http2Stream &stream) noexcept;

    std::unique_ptr<HttpTransport> transport_;
    Options options_;
    Http2StreamTable streams_;
    std::uint32_t peer_advertised_max_concurrent_streams_ = 100;
    std::uint32_t last_peer_stream_id_ = 0;
    std::uint32_t last_local_stream_id_ = 0;
    std::size_t peer_active_stream_count_ = 0;
    std::size_t local_push_stream_count_ = 0;
    std::int32_t conn_send_window_ = 0;
    std::int32_t peer_initial_stream_send_window_ = 65535;
    std::uint32_t peer_header_table_size_ = 4096;
    std::uint32_t peer_max_outbound_frame_size_ = 16384;
    std::uint32_t peer_max_header_list_size_ = 0xffffffffU;
    bool peer_enable_push_ = true;
    bool local_connection_preface_sent_ = false;
    bool local_settings_acknowledged_ = false;
    bool peer_sent_goaway_ = false;
    std::uint32_t peer_last_stream_id_ = 0;
    Http2ErrorCode peer_goaway_error_code_ = Http2ErrorCode::NoError;
    bool expecting_continuation_ = false;
    std::uint32_t inbound_header_stream_id_ = 0;
    std::size_t inbound_header_block_bytes_ = 0;
    bool inbound_header_end_stream_ = false;
    std::uint8_t incoming_pad_length_ = 0;
    std::array<std::uint8_t, 8> control_payload_scratch_{};
    std::size_t control_payload_used_ = 0;
    std::array<std::uint8_t, 6> settings_scratch_{};
    std::size_t settings_scratch_used_ = 0;
    Http2PendingPool pending_pool_;
    Http2SendingEntryQueue send_queue_;
    Http2Stream *owned_stream_head_ = nullptr;
    Http2Stream *conn_wait_head_ = nullptr;
    Http2Stream *conn_wait_tail_ = nullptr;
    bool run_started_ = false;
    bool send_loop_running_ = false;
    bool stop_sending_requested_ = false;
    common::IoErr stop_sending_reason_ = common::IoErr::Canceled;

    friend class Http2Stream;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_CONNECTION_H
