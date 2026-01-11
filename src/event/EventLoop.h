#ifndef FIBER_EVENT_EVENT_LOOP_H
#define FIBER_EVENT_EVENT_LOOP_H

#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "../async/CoroutineFramePool.h"
#include "MpscQueue.h"
#include "Poller.h"
#include "TimerQueue.h"

namespace fiber::event {

using IoEvent = Poller::Event;

class EventLoopGroup;

namespace detail {

template<typename Handle, typename Entry, auto EntryMember>
concept TimerEntryMember = std::is_object_v<Handle> && !std::is_const_v<Handle> &&
                           std::same_as<decltype(EntryMember), Entry Handle::*> && requires(Handle &handle) {
                               { handle.*EntryMember } -> std::same_as<Entry &>;
                           };

template<typename Handle, typename Entry, auto EntryMember>
concept DeferEntryMember = std::is_object_v<Handle> && !std::is_const_v<Handle> &&
                           std::same_as<decltype(EntryMember), Entry Handle::*> && requires(Handle &handle) {
                               { handle.*EntryMember } -> std::same_as<Entry &>;
                           };

template<typename Handle, auto Cb>
concept TimerCallback = std::same_as<decltype(Cb), void (*)(Handle *)>;

template<typename Handle, auto Cb>
concept DeferCallback = std::same_as<decltype(Cb), void (*)(Handle *)>;

} // namespace detail

class EventLoop {
public:
    struct DeferEntry {
        friend class EventLoop;

    public:
        using Callback = void (*)(DeferEntry *);

        DeferEntry();
        DeferEntry(const DeferEntry &) = delete;
        DeferEntry &operator=(const DeferEntry &) = delete;
        DeferEntry(DeferEntry &&) = delete;
        DeferEntry &operator=(DeferEntry &&) = delete;

    private:
        Callback on_run = nullptr;
        Callback on_cancel = nullptr;
        std::atomic<std::uint8_t> state{0};
        MpscQueue<DeferEntry *>::Node node;
        std::ptrdiff_t handle_offset = 0;
    };

    struct TimerEntry {
        friend class EventLoop;

    public:
        using Callback = void (*)(TimerEntry *);

        TimerEntry() = default;
        TimerEntry(const TimerEntry &) = delete;
        TimerEntry &operator=(const TimerEntry &) = delete;
        TimerEntry(TimerEntry &&) = delete;
        TimerEntry &operator=(TimerEntry &&) = delete;

        bool operator<(const TimerEntry &other) const noexcept { return deadline < other.deadline; }

    private:
        Callback callback = nullptr;
        std::chrono::steady_clock::time_point deadline{};
        TimerQueue::Node node{};
        bool in_heap_ = false;
        std::ptrdiff_t handle_offset = 0;
    };

    explicit EventLoop(EventLoopGroup *group = nullptr);
    ~EventLoop();

    void run();
    void run_once();
    void stop();

    [[nodiscard]] static EventLoop &current() noexcept {
        FIBER_ASSERT(current_ != nullptr);
        return *current_;
    }
    [[nodiscard]] static EventLoop *current_or_null() noexcept { return current_; }

    [[nodiscard]] bool in_loop() const noexcept { return current_or_null() == this; }
    [[nodiscard]] std::chrono::steady_clock::time_point now() const noexcept { return now_; }

    void post(DeferEntry &entry) noexcept;
    bool cancel(DeferEntry &entry) noexcept;

    void post_at(std::chrono::steady_clock::time_point when, TimerEntry &entry);
    void cancel(TimerEntry &entry);

    template<typename Handle, auto EntryMember, auto RunCb, auto CancelCb>
        requires detail::DeferEntryMember<Handle, DeferEntry, EntryMember> && detail::DeferCallback<Handle, RunCb> &&
                 detail::DeferCallback<Handle, CancelCb>
    void post(Handle &handle) noexcept {
        DeferEntry &entry = handle.*EntryMember;
        entry.handle_offset = reinterpret_cast<char *>(&entry) - reinterpret_cast<char *>(&handle);
        entry.on_run = &EventLoop::defer_trampoline<Handle, EntryMember, RunCb>;
        entry.on_cancel = &EventLoop::defer_trampoline<Handle, EntryMember, CancelCb>;
        post(entry);
    }

    template<typename Handle, auto EntryMember>
        requires detail::DeferEntryMember<Handle, DeferEntry, EntryMember>
    bool cancel(Handle &handle) {
        DeferEntry &entry = handle.*EntryMember;
        return cancel(entry);
    }

    template<typename Handle, auto EntryMember, auto Cb>
        requires detail::TimerEntryMember<Handle, TimerEntry, EntryMember> && detail::TimerCallback<Handle, Cb>
    void post_at(std::chrono::steady_clock::time_point when, Handle &handle) {
        TimerEntry &entry = handle.*EntryMember;
        entry.handle_offset = reinterpret_cast<char *>(&entry) - reinterpret_cast<char *>(&handle);
        entry.callback = &EventLoop::timer_trampoline<Handle, EntryMember, Cb>;
        post_at(when, entry);
    }

    template<typename Handle, auto EntryMember>
        requires detail::TimerEntryMember<Handle, TimerEntry, EntryMember>
    void cancel(Handle &handle) {
        TimerEntry &entry = handle.*EntryMember;
        cancel(entry);
    }


    fiber::async::CoroutineFramePool &frame_pool() noexcept { return frame_pool_; }

    const fiber::async::CoroutineFramePool &frame_pool() const noexcept { return frame_pool_; }

    Poller &poller() noexcept { return poller_; }
    const Poller &poller() const noexcept { return poller_; }

    EventLoopGroup *group() noexcept { return group_; }

    const EventLoopGroup *group() const noexcept { return group_; }

private:
    static thread_local EventLoop *current_;
    static constexpr std::uint8_t kDeferQueued = 0x1;
    static constexpr std::uint8_t kDeferCanceled = 0x2;

    struct WakeupEntry : Poller::Item {
        EventLoop *loop = nullptr;
    };
    using DeferNode = MpscQueue<DeferEntry *>::Node;

    static TimerEntry *timer_from_node(TimerQueue::Node *node) noexcept;
    friend bool operator<(const TimerQueue::Node &a, const TimerQueue::Node &b) noexcept;

    static void on_wakeup(Poller::Item *item, int fd, IoEvent events);

    template<typename Handle, auto EntryMember, auto Cb>
    static void defer_trampoline(DeferEntry *entry) {
        if (!entry) {
            return;
        }
        auto *bytes = reinterpret_cast<char *>(entry);
        auto *handle = reinterpret_cast<Handle *>(bytes - entry->handle_offset);
        Cb(handle);
    }

    template<typename Handle, auto EntryMember, auto Cb>
    static void timer_trampoline(TimerEntry *entry) {
        if (!entry) {
            return;
        }
        auto *bytes = reinterpret_cast<char *>(entry);
        auto *handle = reinterpret_cast<Handle *>(bytes - entry->handle_offset);
        Cb(handle);
    }

    void notify_wakeup();
    void enqueue_defer(DeferNode *node);
    template<bool all>
    void drain_defers() {
        DeferNode *node = defer_queue_.try_pop_all();

        while (node) {
            DeferNode *next = MpscQueue<DeferEntry *>::next(node);
            DeferEntry *entry = MpscQueue<DeferEntry *>::unwrap(node);
            MpscQueue<DeferEntry *>::reset(node);
            std::uint8_t state = entry->state.exchange(0, std::memory_order_acq_rel);
            if (state & kDeferCanceled) {
                if (entry->on_cancel) {
                    entry->on_cancel(entry);
                }
            } else {
                if (entry->on_run) {
                    entry->on_run(entry);
                }
            }
            node = next;
            if constexpr (all) {
                if (!node) {
                    node = defer_queue_.try_pop_all();
                }
            }
        }
    }
    void drain_wakeup();
    void run_due_timers(std::chrono::steady_clock::time_point now);
    int next_timeout_ms(std::chrono::steady_clock::time_point now) const;

    MpscQueue<DeferEntry *> defer_queue_;
    // Loop-thread only: timer heap operations.
    TimerQueue timers_;
    Poller poller_;
    int event_fd_ = -1;
    WakeupEntry wakeup_entry_{};
    std::atomic<bool> wakeup_pending_{false};
    std::atomic<bool> stop_requested_{false};
    std::chrono::steady_clock::time_point now_{};
    fiber::async::CoroutineFramePool frame_pool_{};
    EventLoopGroup *group_ = nullptr;
};

} // namespace fiber::event

#endif // FIBER_EVENT_EVENT_LOOP_H
