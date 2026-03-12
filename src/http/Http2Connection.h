#ifndef FIBER_HTTP_HTTP2_CONNECTION_H
#define FIBER_HTTP_HTTP2_CONNECTION_H

#include <chrono>
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
#include "Http2SendPayload.h"
#include "Http2Stream.h"
#include "Http2StreamTable.h"
#include "HttpTransport.h"

namespace fiber::http {

class Http2Connection : public common::NonCopyable, public common::NonMovable {
public:
    using FrameHeader = Http2FrameHeader;
    using RunResult = common::IoResult<void>;
    using SendResult = common::IoResult<void>;
    using StableSpan = Http2StableSpan;
    using SendPayload = Http2SendPayload;
    using PendingEntry = Http2PendingEntry;
    using PendingChange = Http2PendingChange;
    using PendingKind = Http2PendingKind;

    struct Options {
        std::size_t read_buffer_size = 64 * 1024;
        std::chrono::milliseconds read_timeout = std::chrono::seconds(30);
        std::chrono::milliseconds write_timeout = std::chrono::seconds(30);
        std::uint32_t max_frame_size = 16384;
        std::size_t max_free_send_entries = 64;
        std::size_t max_free_pending_entries = 64;
        std::uint32_t max_peer_concurrent_streams = 100;
        std::uint32_t max_local_push_streams = 0;
        std::int32_t initial_connection_send_window = 65535;
        std::int32_t initial_stream_send_window = 65535;
        bool expect_peer_preface = true;
    };

    enum class WriterState : std::uint8_t {
        WaitingForData,
        Writing,
        Stopping,
    };

    enum class DispatcherState : std::uint8_t {
        WaitingForWork,
        Dispatching,
        Stopping,
    };

    struct SendEntry {
        using DoneFn = void (*)(void *user_data, std::size_t total_bytes, std::size_t written_bytes,
                                std::size_t frame_header_size, std::size_t logical_bytes,
                                common::IoErr result) noexcept;

        SendEntry *next = nullptr;
        SendPayload *payload_ptr() noexcept;
        const SendPayload *payload_ptr() const noexcept;

        std::size_t total_bytes = 0;
        std::size_t written_bytes = 0;
        std::size_t frame_header_size = 0;
        std::size_t logical_bytes = 0;
        common::IoErr result = common::IoErr::None;
        bool done_notified = false;
        DoneFn on_done = nullptr;
        void *user_data = nullptr;

    private:
        std::uint8_t frame_header_[9]{};
        alignas(SendPayload) std::byte payload_storage_[sizeof(SendPayload)]{};

        friend class Http2Connection;
        friend class Http2Stream;
    };

    virtual ~Http2Connection();

    Http2Connection(std::unique_ptr<HttpTransport> transport);
    Http2Connection(std::unique_ptr<HttpTransport> transport, Options options);

    fiber::async::Task<RunResult> run() noexcept;

protected:
    // `offset` is the number of payload bytes already delivered for the current
    // frame. Only the first `length` bytes starting at `buf.readable_data()`
    // are part of this callback's payload chunk.
    virtual common::IoErr on_frame_payload(const FrameHeader &fhr, const mem::IoBuf &buf, std::size_t offset,
                                           std::size_t length) noexcept;

    common::IoErr enqueue_send_stable_span(const std::uint8_t *data, std::size_t length, SendEntry::DoneFn on_done,
                                           void *user_data = nullptr) noexcept;
    common::IoErr enqueue_send_buf(mem::IoBuf &&buf, SendEntry::DoneFn on_done, void *user_data = nullptr) noexcept;
    common::IoErr enqueue_send_chain(mem::IoBufChain &&bufs, SendEntry::DoneFn on_done,
                                     void *user_data = nullptr) noexcept;
    void update_connection_send_window(std::int32_t delta) noexcept;
    void update_stream_send_window(Http2Stream &stream, std::int32_t delta) noexcept;
    void stop_sending(common::IoErr reason = common::IoErr::Canceled) noexcept;
    [[nodiscard]] WriterState writer_state() const noexcept;
    [[nodiscard]] DispatcherState dispatcher_state() const noexcept;

private:
    [[nodiscard]] std::size_t configured_max_active_streams() const noexcept;
    fiber::async::Task<void> run_send_loop() noexcept;
    fiber::async::Task<void> run_dispatch_loop() noexcept;
    void start_send_loop() noexcept;
    void start_dispatch_loop() noexcept;
    [[nodiscard]] SendEntry *acquire_send_entry() noexcept;
    void release_send_entry(SendEntry *entry) noexcept;
    [[nodiscard]] common::IoErr enqueue_send_entry(SendEntry *entry) noexcept;
    void finish_send_entry(SendEntry *entry, common::IoErr result) noexcept;
    void drain_send_queue(common::IoErr result) noexcept;
    void notify_send_done(SendEntry *entry) noexcept;
    void drain_pending_entries(common::IoErr result) noexcept;
    void reevaluate_stream(Http2Stream &stream) noexcept;
    void refresh_conn_window_wait_list() noexcept;
    void append_ready_stream(Http2Stream &stream) noexcept;
    void remove_ready_stream(Http2Stream &stream) noexcept;
    void append_conn_wait_stream(Http2Stream &stream) noexcept;
    void remove_conn_wait_stream(Http2Stream &stream) noexcept;
    void register_pending_stream(Http2Stream &stream) noexcept;
    void unregister_pending_stream(Http2Stream &stream) noexcept;
    static void encode_frame_header(std::uint8_t *out, std::uint32_t length, Http2FrameType type, std::uint8_t flags,
                                    std::uint32_t stream_id) noexcept;

    std::unique_ptr<HttpTransport> transport_;
    Options options_;
    Http2StreamTable streams_;
    std::uint32_t peer_advertised_max_concurrent_streams_ = 100;
    std::uint32_t last_peer_stream_id_ = 0;
    std::uint32_t last_local_stream_id_ = 0;
    std::size_t peer_active_stream_count_ = 0;
    std::size_t local_push_stream_count_ = 0;
    std::int32_t conn_send_window_ = 0;
    Http2PendingPool pending_pool_;
    Http2Stream *ready_head_ = nullptr;
    Http2Stream *ready_tail_ = nullptr;
    Http2Stream *conn_wait_head_ = nullptr;
    Http2Stream *conn_wait_tail_ = nullptr;
    Http2Stream *pending_stream_head_ = nullptr;
    SendEntry *send_head_ = nullptr;
    SendEntry *send_tail_ = nullptr;
    SendEntry *sending_ = nullptr;
    SendEntry *free_send_entries_ = nullptr;
    std::size_t free_send_entry_count_ = 0;
    WriterState writer_state_ = WriterState::WaitingForData;
    DispatcherState dispatcher_state_ = DispatcherState::WaitingForWork;
    bool writer_running_ = false;
    bool dispatcher_running_ = false;
    bool stop_sending_requested_ = false;
    common::IoErr stop_sending_reason_ = common::IoErr::Canceled;

    friend class Http2Stream;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_CONNECTION_H
