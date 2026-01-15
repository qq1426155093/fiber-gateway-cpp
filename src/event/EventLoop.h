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
concept NotifyEntryMember = std::is_object_v<Handle> && !std::is_const_v<Handle> &&
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
concept NotifyCallback = std::same_as<decltype(Cb), void (*)(Handle *)>;

template<typename Handle, auto Cb>
concept DeferCallback = std::same_as<decltype(Cb), void (*)(Handle *)>;

struct Queue {
    struct Queue *next;
    struct Queue *prev;
};

static inline void queue_init(Queue *q) {
    q->next = q;
    q->prev = q;
}

static inline int queue_empty(const Queue *q) { return q == q->next; }

static inline Queue *queue_head(const Queue *q) { return q->next; }

static inline Queue *queue_next(const Queue *q) { return q->next; }

static inline void queue_add(Queue *h, Queue *n) {
    h->prev->next = n->next;
    n->next->prev = h->prev;
    h->prev = n->prev;
    h->prev->next = h;
}

static inline void queue_split(Queue *h, Queue *q, Queue *n) {
    n->prev = h->prev;
    n->prev->next = n;
    n->next = q;
    h->prev = q->prev;
    h->prev->next = h;
    q->prev = n;
}

static inline void queue_move(Queue *h, Queue *n) {
    if (queue_empty(h))
        queue_init(n);
    else
        queue_split(h, h->next, n);
}
static inline void queue_insert_head(Queue *h, Queue *q) {
    q->next = h->next;
    q->prev = h;
    q->next->prev = q;
    h->next = q;
}

static inline void queue_insert_tail(Queue *h, Queue *q) {
    q->next = h;
    q->prev = h->prev;
    q->prev->next = q;
    h->prev = q;
}

static inline void queue_remove(Queue *q) {
    q->prev->next = q->next;
    q->next->prev = q->prev;
}


} // namespace detail

#define queue_data(pointer, type, field) ((type *) ((char *) (pointer) - offsetof(type, field)))

class EventLoop {
public:
    struct NotifyEntry {
        friend class EventLoop;

    public:
        using Callback = void (*)(NotifyEntry *);

        NotifyEntry();
        NotifyEntry(const NotifyEntry &) = delete;
        NotifyEntry &operator=(const NotifyEntry &) = delete;
        NotifyEntry(NotifyEntry &&) = delete;
        NotifyEntry &operator=(NotifyEntry &&) = delete;

    private:
        Callback on_run = nullptr;
        MpscQueue<NotifyEntry *>::Node node;
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

    struct DeferEntry {
    public:
        friend class EventLoop;

        using Callback = void (*)(DeferEntry *);

    private:
        detail::Queue node_;
        Callback callback_ = nullptr;
        bool in_queue_ = false;
        std::ptrdiff_t handle_offset_ = 0;
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


    template<typename Handle, auto EntryMember, auto RunCb>
        requires detail::NotifyEntryMember<Handle, NotifyEntry, EntryMember> && detail::NotifyCallback<Handle, RunCb>
    void post(Handle &handle) noexcept {
        NotifyEntry &entry = handle.*EntryMember;
        entry.handle_offset = reinterpret_cast<char *>(&entry) - reinterpret_cast<char *>(&handle);
        entry.on_run = &EventLoop::notify_trampoline<Handle, EntryMember, RunCb>;
        enqueue_notify(&entry.node);
    }

    template<typename Handle, auto EntryMember, auto RunCb>
        requires detail::DeferEntryMember<Handle, DeferEntry, EntryMember> && detail::DeferCallback<Handle, RunCb>
    void post_local(Handle &handle) noexcept {
        DeferEntry &entry = handle.*EntryMember;
        entry.handle_offset_ = reinterpret_cast<char *>(&entry) - reinterpret_cast<char *>(&handle);
        entry.callback_ = &EventLoop::defer_trampoline<Handle, EntryMember, RunCb>;
        if (entry.in_queue_) {
            return;
        }
        entry.in_queue_ = true;
        detail::queue_insert_tail(&local_queue_, &entry.node_);
    }

    template<typename Handle, auto EntryMember>
        requires detail::DeferEntryMember<Handle, DeferEntry, EntryMember>
    void cancel(Handle &handle) {
        DeferEntry &entry = handle.*EntryMember;
        cancel(entry);
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

    struct WakeupEntry : Poller::Item {
        EventLoop *loop = nullptr;
    };
    using NotifyNode = MpscQueue<NotifyEntry *>::Node;

    static TimerEntry *timer_from_node(TimerQueue::Node *node) noexcept;
    friend bool operator<(const TimerQueue::Node &a, const TimerQueue::Node &b) noexcept;

    static void on_wakeup(Poller::Item *item, int fd, IoEvent events);

    template<typename Handle, auto EntryMember, auto Cb>
    static void notify_trampoline(NotifyEntry *entry) {
        auto *bytes = reinterpret_cast<char *>(entry);
        auto *handle = reinterpret_cast<Handle *>(bytes - entry->handle_offset);
        Cb(handle);
    }

    template<typename Handle, auto EntryMember, auto Cb>
    static void timer_trampoline(TimerEntry *entry) {
        auto *bytes = reinterpret_cast<char *>(entry);
        auto *handle = reinterpret_cast<Handle *>(bytes - entry->handle_offset);
        Cb(handle);
    }

    template<typename Handle, auto EntryMember, auto Cb>
    static void defer_trampoline(DeferEntry *entry) {
        auto *bytes = reinterpret_cast<char *>(entry);
        auto *handle = reinterpret_cast<Handle *>(bytes - entry->handle_offset_);
        Cb(handle);
    }

    void notify_wakeup();
    void enqueue_notify(NotifyNode *node);
    template<bool all>
    void drain_notify() {
        NotifyNode *node = notify_queue_.try_pop_all();

        while (node) {
            NotifyNode *next = MpscQueue<NotifyEntry *>::next(node);
            NotifyEntry *entry = MpscQueue<NotifyEntry *>::unwrap(node);
            MpscQueue<NotifyEntry *>::reset(node);
            entry->on_run(entry);
            node = next;
            if constexpr (all) {
                if (!node) {
                    node = notify_queue_.try_pop_all();
                }
            }
        }
    }

    template<bool all>
    void drain_defer() {
        for (;;) {
            if (detail::queue_empty(&local_queue_)) {
                return;
            }
            detail::Queue pending;
            detail::queue_move(&local_queue_, &pending);

            while (!detail::queue_empty(&pending)) {
                detail::Queue *node = detail::queue_head(&pending);
                detail::queue_remove(node);
                auto *entry = queue_data(node, DeferEntry, node_);
                entry->in_queue_ = false;
                entry->callback_(entry);
            }
            if constexpr (!all) {
                return;
            }
        }
    }

    void drain_wakeup();
    void run_due_timers(std::chrono::steady_clock::time_point now);
    int next_timeout_ms(std::chrono::steady_clock::time_point now) const;
    void post_at(std::chrono::steady_clock::time_point when, TimerEntry &entry);
    void cancel(TimerEntry &entry);
    void cancel(DeferEntry &entry);

    MpscQueue<NotifyEntry *> notify_queue_;
    // Loop-thread only: timer heap operations.
    detail::Queue local_queue_;
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
