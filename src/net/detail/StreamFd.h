#ifndef FIBER_NET_DETAIL_STREAM_FD_H
#define FIBER_NET_DETAIL_STREAM_FD_H

#include <atomic>
#include <cerrno>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "../../common/IoError.h"
#include "../../common/NonCopyable.h"
#include "../../common/NonMovable.h"
#include "../../event/EventLoop.h"
#include "StreamFd.h"

namespace fiber::net::detail {

/**
 * forbidden read/write in multi-coroutine ,but read and write can overlap.
 * we do not add std::atomic_bool read_occupied_, write_occupied_ member for performance.
 * undefined behaver will occur if violate this constraint.
 */
class StreamFd : public common::NonCopyable, public common::NonMovable {
public:
    template<fiber::event::IoEvent RW>
    class ReadWriteAwaiter;

    StreamFd(fiber::event::EventLoop &loop, int fd);
    ~StreamFd();

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] int fd() const noexcept;
    void close();

    [[nodiscard]] ReadWriteAwaiter<fiber::event::IoEvent::Read> read(void *buf, size_t len) noexcept;
    [[nodiscard]] ReadWriteAwaiter<fiber::event::IoEvent::Write> write(const void *buf, size_t len) noexcept;


private:
    struct StreamItem : fiber::event::Poller::Item {
        StreamFd *stream = nullptr;
    };
    struct WaiterBase {
        StreamFd *stream_ = nullptr;
        fiber::event::IoEvent event_{}; // request or ready event.
        fiber::common::IoErr err_{fiber::common::IoErr::None}; //
        std::coroutine_handle<> coro_ = nullptr;
    };
    // ReadAwaiter WriteAwaiter can inherent this struct.
    struct LocalThreadWaiter : WaiterBase {};
    struct CrossThreadWaiter;

    template<fiber::event::IoEvent RW, typename Waiter>
        requires((std::is_same<LocalThreadWaiter, Waiter>::value || std::is_same<CrossThreadWaiter, Waiter>::value) &&
                 (RW == fiber::event::IoEvent::Read || RW == fiber::event::IoEvent::Write))
    fiber::common::IoErr begin_event(Waiter *waiter) noexcept {
        FIBER_ASSERT(loop_.in_loop());
        FIBER_ASSERT(waiter);
        if (fd_ < 0) {
            return fiber::common::IoErr::BadFd;
        }
        if constexpr (RW == fiber::event::IoEvent::Read) {
            FIBER_ASSERT(local_read_waiter_ == nullptr);
            if constexpr (std::is_same<LocalThreadWaiter, Waiter>::value) {
                local_read_waiter_ = waiter;
                local_read_waiting_ = true;
            } else {
                cross_read_waiter_ = waiter;
                local_read_waiting_ = false;
            }
        } else {
            FIBER_ASSERT(local_write_waiter_ == nullptr);
            if constexpr (std::is_same<LocalThreadWaiter, Waiter>::value) {
                local_write_waiter_ = waiter;
                local_write_waiting_ = true;
            } else {
                cross_write_waiter_ = waiter;
                local_write_waiting_ = false;
            }
        }
        fiber::event::IoEvent desired = watching_ | RW;
        if (desired == watching_) {
            return fiber::common::IoErr::None;
        }
        int rc = 0;
        if (watching_ == fiber::event::IoEvent::None) {
            rc = loop_.poller().add(fd_, desired, &item_);
        } else {
            rc = loop_.poller().mod(fd_, desired, &item_);
        }
        if (rc != 0) {
            if constexpr (RW == fiber::event::IoEvent::Read) {
                local_read_waiter_ = nullptr;
            } else {
                local_write_waiter_ = nullptr;
            }
            return fiber::common::io_err_from_errno(errno);
        }
        watching_ = desired;
        return fiber::common::IoErr::None;
    }
    template<fiber::event::IoEvent RW, typename Waiter>
        requires((std::is_same<LocalThreadWaiter, Waiter>::value || std::is_same<CrossThreadWaiter, Waiter>::value) &&
                 (RW == fiber::event::IoEvent::Read || RW == fiber::event::IoEvent::Write))
    fiber::common::IoErr cancel_event(Waiter *waiter) noexcept {
        FIBER_ASSERT(loop_.in_loop());
        if (!waiter) {
            return fiber::common::IoErr::Invalid;
        }
        void *current = nullptr;
        if constexpr (RW == fiber::event::IoEvent::Read) {
            current = static_cast<void *>(local_read_waiter_);
        } else {
            current = static_cast<void *>(local_write_waiter_);
        }
        if (current != static_cast<void *>(waiter)) {
            return fiber::common::IoErr::NotFound;
        }
        if constexpr (RW == fiber::event::IoEvent::Read) {
            local_read_waiter_ = nullptr;
        } else {
            local_write_waiter_ = nullptr;
        }
        fiber::event::IoEvent desired = watching_ & ~RW;
        if (desired == watching_) {
            return fiber::common::IoErr::None;
        }
        if (desired == fiber::event::IoEvent::None) {
            loop_.poller().del(fd_);
        } else {
            loop_.poller().mod(fd_, desired, &item_);
        }
        watching_ = desired;
        return fiber::common::IoErr::None;
    }


    fiber::common::IoErr read_once(void *buf, size_t len, size_t &out);
    fiber::common::IoErr write_once(const void *buf, size_t len, size_t &out);

    static void on_events(fiber::event::Poller::Item *item, int fd, fiber::event::IoEvent events);
    void handle_events(fiber::event::IoEvent events);

    // acquire the read/write lock. None if success, Busy if another coroutine acquired.  BadFd if closed.
    //[[nodiscard]] fiber::common::IoErr try_acquire_read() noexcept;
    //[[nodiscard]] fiber::common::IoErr try_acquire_write() noexcept;
    // release the read/write lock.
    // void release_read() noexcept;
    // void release_write() noexcept;

    fiber::event::EventLoop &loop_;
    StreamItem item_{};
    int fd_ = -1;
    fiber::event::IoEvent watching_ = fiber::event::IoEvent::None;
    // std::atomic_bool read_occupied_{false}; // reading lock, forbidden concurrent
    // std::atomic_bool write_occupied_{false}; // writing lock, forbidden concurrent

    bool local_read_waiting_ = false;
    // read waiter, if read-caller got EAGAIN, request watch read event and wait it ready
    union {
        LocalThreadWaiter *local_read_waiter_ = nullptr;
        CrossThreadWaiter *cross_read_waiter_;
    };

    bool local_write_waiting_ = false;
    // write waiter, if write-caller got EAGAIN, request watch write event and wait it ready
    union {
        LocalThreadWaiter *local_write_waiter_ = nullptr;
        CrossThreadWaiter *cross_write_waiter_;
    };
};

enum class WaiterState : std::uint8_t {
    Notify_Watch, // caller-thread notify fd-loop-thread of stream watch event.
    Notify_Resume, // notify the caller-thread resume the coroutine
    Watching_Event, // watching in fd-loop poller.
    Request_Cancel, // request cancel watch event if fd-loop is watching.
    Waiting_Cancel, // wake by event before Request_Cancel request arrive.
    Canceled, // Notify_Watch and Notify_Resume state can turn to canceled
};
struct StreamFd::CrossThreadWaiter : WaiterBase {
    fiber::event::EventLoop *loop_ = nullptr;
    // used to notify io-loop watch event and caller-loop resume
    fiber::event::EventLoop::DeferEntry notify_entry_{};
    // if the state_ is Notify_Watch. but the caller cancel waiting, notify the io-loop unwatch event.
    fiber::event::EventLoop::DeferEntry cancel_entry_{};
    std::atomic<WaiterState> state_{WaiterState::Notify_Watch};

    void cancel_wait() noexcept;

    static void do_notify_resume(CrossThreadWaiter *waiter) noexcept;
    static void on_notify_watch(CrossThreadWaiter *waiter);
    static void on_notify_cancel(CrossThreadWaiter *waiter);
    static void on_notify_resume(CrossThreadWaiter *waiter);
    static void on_cancel_wait(CrossThreadWaiter *waiter);
};

template<fiber::event::IoEvent RW>
class StreamFd::ReadWriteAwaiter : public LocalThreadWaiter {
public:
    ReadWriteAwaiter(StreamFd &stream, void *buf, size_t len) noexcept : buf_(buf), len_(len) {
        stream_ = &stream;
        event_ = RW;
    }

    ReadWriteAwaiter(const ReadWriteAwaiter &) = delete;
    ReadWriteAwaiter &operator=(const ReadWriteAwaiter &) = delete;
    ReadWriteAwaiter(ReadWriteAwaiter &&) = delete;
    ReadWriteAwaiter &operator=(ReadWriteAwaiter &&) = delete;
    ~ReadWriteAwaiter();

    bool await_ready() noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> handle);
    // invoke ::recv()/::send in this function arccording to RW, maybe EAGAIN.
    fiber::common::IoResult<size_t> await_resume() noexcept;

private:
    friend class StreamFd;
    void *buf_ = nullptr;
    size_t len_ = 0;
    bool waiting_ = false;
    CrossThreadWaiter *waiter_ = nullptr;
    size_t result_ = 0;
    bool completed_ = false;
};


} // namespace fiber::net::detail

#endif // FIBER_NET_DETAIL_STREAM_FD_H
