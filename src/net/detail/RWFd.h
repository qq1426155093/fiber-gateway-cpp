#ifndef FIBER_NET_DETAIL_RW_FD_H
#define FIBER_NET_DETAIL_RW_FD_H

#include <atomic>
#include <concepts>
#include <coroutine>
#include <cstdint>
#include <new>
#include <type_traits>

#include "../../common/Assert.h"
#include "../../common/IoError.h"
#include "../../common/NonCopyable.h"
#include "../../common/NonMovable.h"
#include "../../event/EventLoop.h"
#include "Efd.h"

namespace fiber::net::detail {

class RWFd;

struct RWFdWaiterBase {
    RWFd *rwfd_ = nullptr;
    fiber::event::IoEvent event_{};
    fiber::common::IoErr err_{fiber::common::IoErr::None};
    std::coroutine_handle<> coro_ = nullptr;
};

struct RWFdLocalThreadWaiter : RWFdWaiterBase {};
struct RWFdCrossThreadWaiter;

template<typename T>
concept RWFdWaiter = std::same_as<std::remove_cvref_t<T>, RWFdLocalThreadWaiter> ||
                     std::same_as<std::remove_cvref_t<T>, RWFdCrossThreadWaiter>;

template<fiber::event::IoEvent Event>
concept RWFdWaitEvent = (Event == fiber::event::IoEvent::Read || Event == fiber::event::IoEvent::Write);

enum class RWFdWaiterState : std::uint8_t {
    Notify_Watch,
    Notify_Resume,
    Watching_Event,
    Request_Cancel,
    Waiting_Cancel,
    Canceled,
};

class RWFd : public common::NonCopyable, public common::NonMovable {
public:
    template<fiber::event::IoEvent Event>
        requires(RWFdWaitEvent<Event>)
    class WaitAwaiter;

    using WaitReadableAwaiter = WaitAwaiter<fiber::event::IoEvent::Read>;
    using WaitWritableAwaiter = WaitAwaiter<fiber::event::IoEvent::Write>;

    explicit RWFd(fiber::event::EventLoop &loop);
    RWFd(fiber::event::EventLoop &loop, int fd);
    ~RWFd();

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] fiber::event::EventLoop &loop() const noexcept;

    fiber::common::IoErr attach(int fd) noexcept;
    int release_fd() noexcept;
    void close();

    [[nodiscard]] WaitReadableAwaiter wait_readable() noexcept;
    [[nodiscard]] WaitWritableAwaiter wait_writable() noexcept;

private:
    friend struct RWFdCrossThreadWaiter;

    template<fiber::event::IoEvent Event>
        requires(RWFdWaitEvent<Event>)
    RWFdLocalThreadWaiter *&local_waiter_slot() noexcept {
        if constexpr (Event == fiber::event::IoEvent::Read) {
            return local_read_waiter_;
        }
        return local_write_waiter_;
    }

    template<fiber::event::IoEvent Event>
        requires(RWFdWaitEvent<Event>)
    RWFdCrossThreadWaiter *&cross_waiter_slot() noexcept {
        if constexpr (Event == fiber::event::IoEvent::Read) {
            return cross_read_waiter_;
        }
        return cross_write_waiter_;
    }

    template<fiber::event::IoEvent Event>
        requires(RWFdWaitEvent<Event>)
    bool &local_waiting_slot() noexcept {
        if constexpr (Event == fiber::event::IoEvent::Read) {
            return local_read_waiting_;
        }
        return local_write_waiting_;
    }

    template<fiber::event::IoEvent Event>
        requires(RWFdWaitEvent<Event>)
    [[nodiscard]] bool event_has_waiter() const noexcept {
        if constexpr (Event == fiber::event::IoEvent::Read) {
            return local_read_waiter_ != nullptr;
        }
        return local_write_waiter_ != nullptr;
    }

    template<fiber::event::IoEvent Event, typename Waiter>
        requires(RWFdWaitEvent<Event> && RWFdWaiter<Waiter>)
    fiber::common::IoErr begin_wait(Waiter *waiter) noexcept {
        FIBER_ASSERT(loop().in_loop());
        FIBER_ASSERT(waiter);
        if (!valid()) {
            return fiber::common::IoErr::BadFd;
        }
        if (event_has_waiter<Event>()) {
            return fiber::common::IoErr::Busy;
        }
        fiber::common::IoErr err = efd_.watch_add(Event);
        if (err != fiber::common::IoErr::None) {
            return err;
        }

        if constexpr (std::is_same_v<std::remove_cvref_t<Waiter>, RWFdLocalThreadWaiter>) {
            local_waiter_slot<Event>() = waiter;
            local_waiting_slot<Event>() = true;
        } else {
            cross_waiter_slot<Event>() = waiter;
            local_waiting_slot<Event>() = false;
        }
        return fiber::common::IoErr::None;
    }

    template<fiber::event::IoEvent Event, typename Waiter>
        requires(RWFdWaitEvent<Event> && RWFdWaiter<Waiter>)
    fiber::common::IoErr cancel_wait(Waiter *waiter) noexcept {
        FIBER_ASSERT(loop().in_loop());
        FIBER_ASSERT(waiter);
        if constexpr (std::is_same_v<std::remove_cvref_t<Waiter>, RWFdLocalThreadWaiter>) {
            auto *&slot = local_waiter_slot<Event>();
            FIBER_ASSERT(slot == waiter);
            slot = nullptr;
        } else {
            auto *&slot = cross_waiter_slot<Event>();
            FIBER_ASSERT(slot == waiter);
            slot = nullptr;
        }
        local_waiting_slot<Event>() = false;
        return efd_.watch_del(Event);
    }

    static void on_efd_events(void *owner, fiber::event::IoEvent events);
    void handle_events(fiber::event::IoEvent events);

    [[nodiscard]] bool has_waiters() const noexcept;

    Efd efd_;

    bool local_read_waiting_ = false;
    union {
        RWFdLocalThreadWaiter *local_read_waiter_ = nullptr;
        RWFdCrossThreadWaiter *cross_read_waiter_;
    };

    bool local_write_waiting_ = false;
    union {
        RWFdLocalThreadWaiter *local_write_waiter_ = nullptr;
        RWFdCrossThreadWaiter *cross_write_waiter_;
    };
};

struct RWFdCrossThreadWaiter : RWFdWaiterBase {
    fiber::event::EventLoop *loop_ = nullptr;
    fiber::event::EventLoop::NotifyEntry notify_entry_{};
    fiber::event::EventLoop::NotifyEntry cancel_entry_{};
    std::atomic<RWFdWaiterState> state_{RWFdWaiterState::Notify_Watch};

    void cancel_wait() noexcept;

    static void do_notify_resume(RWFdCrossThreadWaiter *waiter) noexcept;
    static void on_notify_watch(RWFdCrossThreadWaiter *waiter);
    static void on_notify_cancel(RWFdCrossThreadWaiter *waiter);
    static void on_notify_resume(RWFdCrossThreadWaiter *waiter);
};

template<fiber::event::IoEvent Event>
    requires(RWFdWaitEvent<Event>)
class RWFd::WaitAwaiter : public RWFdLocalThreadWaiter {
public:
    explicit WaitAwaiter(RWFd &rwfd) noexcept {
        rwfd_ = &rwfd;
        event_ = Event;
    }

    WaitAwaiter(const WaitAwaiter &) = delete;
    WaitAwaiter &operator=(const WaitAwaiter &) = delete;
    WaitAwaiter(WaitAwaiter &&) = delete;
    WaitAwaiter &operator=(WaitAwaiter &&) = delete;
    ~WaitAwaiter() {
        if (!waiting_) {
            FIBER_ASSERT(waiter_ == nullptr);
            return;
        }
        if (waiter_) {
            FIBER_ASSERT(!rwfd_->loop().in_loop());
            auto *waiter = waiter_;
            waiter->cancel_wait();
            waiter_ = nullptr;
            return;
        }
        FIBER_ASSERT(rwfd_->loop().in_loop());
        rwfd_->cancel_wait<Event, RWFdLocalThreadWaiter>(this);
    }

    bool await_ready() noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle) {
        coro_ = handle;
        err_ = fiber::common::IoErr::None;
        completed_ = false;
        waiting_ = true;

        if (rwfd_->loop().in_loop()) {
            fiber::common::IoErr err = rwfd_->begin_wait<Event, RWFdLocalThreadWaiter>(this);
            if (err != fiber::common::IoErr::None) {
                err_ = err;
                completed_ = true;
                waiting_ = false;
                return false;
            }
            return true;
        }

        auto *current = fiber::event::EventLoop::current_or_null();
        FIBER_ASSERT(current != nullptr);
        auto *waiter = new (std::nothrow) RWFdCrossThreadWaiter();
        if (!waiter) {
            err_ = fiber::common::IoErr::NoMem;
            completed_ = true;
            waiting_ = false;
            return false;
        }
        waiter->rwfd_ = rwfd_;
        waiter->event_ = Event;
        waiter->coro_ = handle;
        waiter->loop_ = current;
        waiter_ = waiter;
        rwfd_->loop().post<RWFdCrossThreadWaiter,
                           &RWFdCrossThreadWaiter::notify_entry_,
                           &RWFdCrossThreadWaiter::on_notify_watch>(*waiter);
        return true;
    }

    fiber::common::IoResult<void> await_resume() noexcept {
        waiting_ = false;
        if (completed_) {
            completed_ = false;
            if (err_ == fiber::common::IoErr::None) {
                return {};
            }
            return std::unexpected(err_);
        }

        fiber::common::IoErr err = err_;
        RWFdCrossThreadWaiter *waiter = waiter_;
        if (waiter) {
            err = waiter->err_;
            waiter_ = nullptr;
            delete waiter;
        }
        if (err == fiber::common::IoErr::None) {
            return {};
        }
        return std::unexpected(err);
    }

private:
    bool waiting_ = false;
    bool completed_ = false;
    RWFdCrossThreadWaiter *waiter_ = nullptr;
};

} // namespace fiber::net::detail

#endif // FIBER_NET_DETAIL_RW_FD_H
