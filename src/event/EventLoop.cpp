#include "EventLoop.h"

#include <cerrno>
#include <cstddef>
#include <limits>
#include <sys/eventfd.h>
#include <unistd.h>

#include "../common/Assert.h"

namespace fiber::event {

thread_local EventLoop *EventLoop::current_ = nullptr;

namespace {

constexpr std::uint32_t to_mask(IoEvent events) { return static_cast<std::uint32_t>(events); }

IoEvent to_io_event(std::uint32_t events, IoEvent interested) {
    IoEvent mask = Poller::Event::None;
    if (events & (EPOLLIN | EPOLLPRI)) {
        mask |= IoEvent::Read;
    }
    if (events & EPOLLOUT) {
        mask |= IoEvent::Write;
    }
    if (events & (EPOLLERR | EPOLLHUP)) {
        mask |= interested;
    }
    return mask;
}

} // namespace

EventLoop::NotifyEntry::NotifyEntry() : node(this) {}

EventLoop::EventLoop(EventLoopGroup *group) : group_(group) {
    timers_.init();
    wakeup_entry_.loop = this;
    wakeup_entry_.callback = &EventLoop::on_wakeup;
    event_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (event_fd_ < 0) {
        return;
    }
    if (!poller_.valid()) {
        ::close(event_fd_);
        event_fd_ = -1;
        return;
    }
    if (poller_.add(event_fd_, IoEvent::Read, &wakeup_entry_) != fiber::common::IoErr::None) {
        ::close(event_fd_);
        event_fd_ = -1;
    }
}

EventLoop::~EventLoop() {
    if (event_fd_ >= 0) {
        ::close(event_fd_);
    }
}

EventLoop::TimerEntry *EventLoop::timer_from_node(TimerQueue::Node *node) noexcept {
    if (!node) {
        return nullptr;
    }
    return reinterpret_cast<TimerEntry *>(reinterpret_cast<char *>(node) - offsetof(TimerEntry, node));
}

bool operator<(const TimerQueue::Node &a, const TimerQueue::Node &b) noexcept {
    const auto *left = EventLoop::timer_from_node(const_cast<TimerQueue::Node *>(&a));
    const auto *right = EventLoop::timer_from_node(const_cast<TimerQueue::Node *>(&b));
    return *left < *right;
}

void EventLoop::notify_wakeup() {
    if (event_fd_ < 0) {
        return;
    }
    if (!wakeup_pending_.exchange(true, std::memory_order_acq_rel)) {
        std::uint64_t one = 1;
        ssize_t written = ::write(event_fd_, &one, sizeof(one));
        (void) written;
    }
}

void EventLoop::enqueue_notify(NotifyNode *node) {
    notify_queue_.push(node);
    if (!in_loop()) {
        notify_wakeup();
    }
}

void EventLoop::on_wakeup(Poller::Item *item, int fd, IoEvent events) {
    (void) fd;
    (void) events;
    auto *entry = static_cast<WakeupEntry *>(item);
    if (!entry || !entry->loop) {
        return;
    }
    entry->loop->drain_wakeup();
}

void EventLoop::drain_wakeup() {
    if (event_fd_ < 0) {
        return;
    }
    std::uint64_t value = 0;
    for (;;) {
        ssize_t rc = ::read(event_fd_, &value, sizeof(value));
        if (rc == static_cast<ssize_t>(sizeof(value))) {
            continue;
        }
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    wakeup_pending_.store(false, std::memory_order_release);
}

void EventLoop::run_due_timers(std::chrono::steady_clock::time_point now) {
    for (;;) {
        TimerQueue::Node *node = timers_.min();
        if (!node) {
            break;
        }
        TimerEntry *entry = timer_from_node(node);
        if (!entry || entry->deadline > now) {
            break;
        }
        timers_.remove(&entry->node);
        entry->in_heap_ = false;
        if (entry->callback) {
            entry->callback(entry);
        }
    }
}

int EventLoop::next_timeout_ms(std::chrono::steady_clock::time_point now) const {
    TimerQueue::Node *node = timers_.min();
    if (!node) {
        return -1;
    }
    const TimerEntry *entry = timer_from_node(const_cast<TimerQueue::Node *>(node));
    if (!entry) {
        return -1;
    }
    if (entry->deadline <= now) {
        return 0;
    }
    auto delta = entry->deadline - now;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(delta).count();
    if (ms <= 0) {
        return 0;
    }
    if (ms > std::numeric_limits<int>::max()) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(ms);
}

void EventLoop::run() {
    if (event_fd_ < 0 || !poller_.valid()) {
        return;
    }
    EventLoop *prev = current_;
    current_ = this;
    stop_requested_.store(false, std::memory_order_release);
    drain_notifys<false>();
    do {
        run_once();
    } while (!stop_requested_.load(std::memory_order_acquire));
    current_ = prev;
}

void EventLoop::run_once() {
    if (event_fd_ < 0 || !poller_.valid()) {
        return;
    }
    now_ = std::chrono::steady_clock::now();
    run_due_timers(now_);

    drain_notifys<true>();
    int timeout_ms = next_timeout_ms(now_);
    constexpr int kMaxEvents = 64;
    epoll_event events[kMaxEvents];

    int count = poller_.wait(events, kMaxEvents, timeout_ms);
    now_ = std::chrono::steady_clock::now();
    if (count < 0) {
        if (errno == EINTR) {
            return;
        }
        return;
    }

    for (int i = 0; i < count; ++i) {
        auto *item = static_cast<Poller::Item *>(events[i].data.ptr);
        IoEvent io = to_io_event(events[i].events, item ? item->interested_ : IoEvent::None);
        if (to_mask(io) == 0) {
            continue;
        }
        item->callback(item, item->fd(), io);
    }
    drain_notifys<false>();
}

void EventLoop::stop() {
    stop_requested_.store(true, std::memory_order_release);
    notify_wakeup();
}

void EventLoop::post_at(std::chrono::steady_clock::time_point when, TimerEntry &entry) {
    FIBER_ASSERT(in_loop());
    FIBER_ASSERT(!entry.in_heap_);
    FIBER_ASSERT(entry.callback != nullptr);

    entry.deadline = when;
    timers_.insert(&entry.node);
    entry.in_heap_ = true;
}

void EventLoop::cancel(TimerEntry &entry) {
    FIBER_ASSERT(in_loop());
    if (!entry.in_heap_) {
        return;
    }
    timers_.remove(&entry.node);
    entry.in_heap_ = false;
}

} // namespace fiber::event
