#ifndef FIBER_HTTP_HTTP2_SENDING_ENTRY_QUEUE_H
#define FIBER_HTTP_HTTP2_SENDING_ENTRY_QUEUE_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <coroutine>

#include "../common/IoError.h"
#include "../event/EventLoop.h"
#include "Http2SendPayload.h"

namespace fiber::http {

struct Http2SendingEntry {
    using DoneFn = void (*)(void *user_data, std::size_t total_bytes, std::size_t written_bytes,
                            std::size_t frame_header_size, std::size_t logical_bytes, common::IoErr result) noexcept;

    Http2SendingEntry *next = nullptr;
    Http2SendPayload *payload_ptr() noexcept;
    const Http2SendPayload *payload_ptr() const noexcept;

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
    alignas(Http2SendPayload) std::byte payload_storage_[sizeof(Http2SendPayload)]{};

    friend class Http2Connection;
    friend class Http2SendingEntryQueue;
    friend class Http2Stream;
};

class Http2SendingEntryQueue {
public:
    struct PollResult {
        enum class Kind : std::uint8_t {
            Entry,
            TimedOut,
            Closed,
        };

        Kind kind = Kind::TimedOut;
        Http2SendingEntry *entry = nullptr;
    };

    class PollAwaiter {
    public:
        PollAwaiter(Http2SendingEntryQueue &queue, std::chrono::milliseconds timeout) noexcept;
        PollAwaiter(const PollAwaiter &) = delete;
        PollAwaiter &operator=(const PollAwaiter &) = delete;
        PollAwaiter(PollAwaiter &&) = delete;
        PollAwaiter &operator=(PollAwaiter &&) = delete;
        ~PollAwaiter();

        bool await_ready() const noexcept;
        bool await_suspend(std::coroutine_handle<> handle);
        PollResult await_resume() noexcept;

    private:
        static void on_notify(PollAwaiter *awaiter);
        static void on_timeout(PollAwaiter *awaiter);
        void resume() noexcept;
        bool has_timer() const noexcept;

        Http2SendingEntryQueue *queue_ = nullptr;
        std::chrono::milliseconds timeout_{};
        fiber::event::EventLoop *loop_ = nullptr;
        std::coroutine_handle<> handle_{};
        fiber::event::EventLoop::NotifyEntry notify_entry_{};
        fiber::event::EventLoop::TimerEntry timer_entry_{};
        bool timed_out_ = false;
        bool resume_posted_ = false;

        friend class Http2SendingEntryQueue;
    };

    explicit Http2SendingEntryQueue(std::size_t max_free_entries) noexcept : max_free_entries_(max_free_entries) {}
    Http2SendingEntryQueue(const Http2SendingEntryQueue &) = delete;
    Http2SendingEntryQueue &operator=(const Http2SendingEntryQueue &) = delete;
    ~Http2SendingEntryQueue();

    [[nodiscard]] Http2SendingEntry *acquire() noexcept;
    void release(Http2SendingEntry *entry) noexcept;
    [[nodiscard]] common::IoErr enqueue(Http2SendingEntry *entry) noexcept;
    [[nodiscard]] PollAwaiter poll_to_send(std::chrono::milliseconds timeout) noexcept;
    [[nodiscard]] Http2SendingEntry *pop_ready() noexcept;
    void close() noexcept;
    [[nodiscard]] bool closed() const noexcept { return closed_; }
    [[nodiscard]] bool idle() const noexcept { return ready_head_ == nullptr; }

private:
    friend class PollAwaiter;

    void destroy_entry(Http2SendingEntry *entry) noexcept;
    void reset_released_entry(Http2SendingEntry *entry) noexcept;
    [[nodiscard]] bool arm_waiter(PollAwaiter *awaiter) noexcept;
    void cancel_waiter(PollAwaiter *awaiter) noexcept;
    void notify_waiter() noexcept;

    Http2SendingEntry *ready_head_ = nullptr;
    Http2SendingEntry *ready_tail_ = nullptr;
    Http2SendingEntry *free_head_ = nullptr;
    std::size_t free_count_ = 0;
    std::size_t max_free_entries_ = 0;
    PollAwaiter *waiter_ = nullptr;
    bool closed_ = false;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_SENDING_ENTRY_QUEUE_H
