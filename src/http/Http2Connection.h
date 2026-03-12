#ifndef FIBER_HTTP_HTTP2_CONNECTION_H
#define FIBER_HTTP_HTTP2_CONNECTION_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <coroutine>
#include <memory>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../common/mem/IoBuf.h"
#include "Http2Protocol.h"
#include "HttpTransport.h"

namespace fiber::http {

class Http2Connection : public common::NonCopyable, public common::NonMovable {
public:
    using FrameHeader = Http2FrameHeader;
    using RunResult = common::IoResult<void>;
    using SendResult = common::IoResult<void>;

    struct Options {
        std::size_t read_buffer_size = 64 * 1024;
        std::chrono::milliseconds read_timeout = std::chrono::seconds(30);
        std::chrono::milliseconds write_timeout = std::chrono::seconds(30);
        std::uint32_t max_frame_size = 16384;
        std::size_t max_free_send_entries = 64;
        bool expect_peer_preface = true;
    };

    struct StableSpan {
        const std::uint8_t *data = nullptr;
        std::size_t length = 0;
        std::size_t offset = 0;
    };

    enum class WriterState : std::uint8_t {
        WaitingForData,
        Writing,
        Stopping,
    };

    class SendPayload {
    public:
        enum class Kind : std::uint8_t {
            None,
            StableSpan,
            IoBuf,
            IoBufChain,
        };

        SendPayload() noexcept = default;
        ~SendPayload();

        SendPayload(const SendPayload &) = delete;
        SendPayload &operator=(const SendPayload &) = delete;

        SendPayload(SendPayload &&other) noexcept;
        SendPayload &operator=(SendPayload &&other) noexcept;

        [[nodiscard]] Kind kind() const noexcept;
        [[nodiscard]] bool empty() const noexcept;
        [[nodiscard]] std::size_t readable_bytes() const noexcept;

        void reset() noexcept;
        void set_stable_span(const std::uint8_t *data, std::size_t length) noexcept;
        void set_buf(mem::IoBuf &&buf) noexcept;
        void set_chain(mem::IoBufChain &&bufs) noexcept;
        fiber::async::Task<common::IoResult<size_t>> write_once(HttpTransport &transport,
                                                                std::chrono::milliseconds timeout) noexcept;

    private:
        union Storage {
            Storage() {}
            ~Storage() {}

            StableSpan span;
            mem::IoBuf buf;
            mem::IoBufChain chain;
        };

        void move_from(SendPayload &&other) noexcept;
        [[nodiscard]] StableSpan &span() noexcept;
        [[nodiscard]] const StableSpan &span() const noexcept;
        [[nodiscard]] mem::IoBuf &buf() noexcept;
        [[nodiscard]] const mem::IoBuf &buf() const noexcept;
        [[nodiscard]] mem::IoBufChain &chain() noexcept;
        [[nodiscard]] const mem::IoBufChain &chain() const noexcept;

        Storage storage_{};
        Kind kind_ = Kind::None;
    };

    struct SendEntry {
        using DoneFn = void (*)(SendEntry *entry) noexcept;

        SendEntry *next = nullptr;
        SendPayload *payload_ptr() noexcept;
        const SendPayload *payload_ptr() const noexcept;

        std::size_t total_bytes = 0;
        std::size_t written_bytes = 0;
        common::IoErr result = common::IoErr::None;
        bool done_notified = false;
        DoneFn on_done = nullptr;
        void *user_data = nullptr;

    private:
        alignas(SendPayload) std::byte payload_storage_[sizeof(SendPayload)]{};

        friend class Http2Connection;
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
    void stop_sending(common::IoErr reason = common::IoErr::Canceled) noexcept;
    [[nodiscard]] WriterState writer_state() const noexcept;

private:
    fiber::async::Task<void> run_send_loop() noexcept;
    void start_send_loop() noexcept;
    [[nodiscard]] SendEntry *acquire_send_entry() noexcept;
    void release_send_entry(SendEntry *entry) noexcept;
    [[nodiscard]] common::IoErr enqueue_send_entry(SendEntry *entry) noexcept;
    void finish_send_entry(SendEntry *entry, common::IoErr result) noexcept;
    void drain_send_queue(common::IoErr result) noexcept;
    void notify_send_done(SendEntry *entry) noexcept;

    std::unique_ptr<HttpTransport> transport_;
    Options options_;
    SendEntry *send_head_ = nullptr;
    SendEntry *send_tail_ = nullptr;
    SendEntry *sending_ = nullptr;
    SendEntry *free_send_entries_ = nullptr;
    std::size_t free_send_entry_count_ = 0;
    WriterState writer_state_ = WriterState::WaitingForData;
    bool writer_running_ = false;
    bool stop_sending_requested_ = false;
    common::IoErr stop_sending_reason_ = common::IoErr::Canceled;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_CONNECTION_H
