#ifndef FIBER_NET_DETAIL_STREAM_FD_H
#define FIBER_NET_DETAIL_STREAM_FD_H

#include <atomic>
#include <cerrno>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <sys/uio.h>
#include <type_traits>
#include <utility>

#include "../../common/IoError.h"
#include "../../common/NonCopyable.h"
#include "../../common/NonMovable.h"
#include "../../event/EventLoop.h"
#include "StreamFd.h"

namespace fiber::net::detail {

class StreamFd;

struct WaiterBase {
    StreamFd *stream_ = nullptr;
    fiber::event::IoEvent event_{}; // request or ready event.
    fiber::common::IoErr err_{fiber::common::IoErr::None}; //
    std::coroutine_handle<> coro_ = nullptr;
};

struct LocalThreadWaiter : WaiterBase {};
struct CrossThreadWaiter;

template<typename T>
concept StreamWaiter = std::same_as<std::remove_cvref_t<T>, LocalThreadWaiter> ||
                       std::same_as<std::remove_cvref_t<T>, CrossThreadWaiter>;

template<fiber::event::IoEvent Event>
concept ReadWriteEvent = (Event == fiber::event::IoEvent::Read || Event == fiber::event::IoEvent::Write);

/**
 * forbidden read/write in multi-coroutine ,but read and write can overlap.
 * we do not add std::atomic_bool read_occupied_, write_occupied_ member for performance.
 * cannot close StreamFd if some coroutine watching events. which cause crash.
 * undefined behaver will occur if deviate this constraint.
 */
class StreamFd : public common::NonCopyable, public common::NonMovable {
public:
    struct ReadOp {
        static constexpr fiber::event::IoEvent kEvent = fiber::event::IoEvent::Read;
        void *buf_ = nullptr;
        size_t len_ = 0;

        ReadOp(void *buf, size_t len) : buf_(buf), len_(len) {}

        fiber::common::IoErr once(StreamFd &stream, size_t &out) const { return stream.read_once(buf_, len_, out); }
    };
    struct WriteOp {
        static constexpr fiber::event::IoEvent kEvent = fiber::event::IoEvent::Write;
        const void *buf_ = nullptr;
        size_t len_ = 0;

        WriteOp(const void *buf, size_t len) : buf_(buf), len_(len) {}

        fiber::common::IoErr once(StreamFd &stream, size_t &out) const { return stream.write_once(buf_, len_, out); }
    };
    struct ReadvOp {
        static constexpr fiber::event::IoEvent kEvent = fiber::event::IoEvent::Read;
        const struct iovec *iov_ = nullptr;
        int iovcnt_ = 0;

        ReadvOp(const struct iovec *iov, int iovcnt) : iov_(iov), iovcnt_(iovcnt) {}

        fiber::common::IoErr once(StreamFd &stream, size_t &out) const { return stream.readv_once(iov_, iovcnt_, out); }
    };
    struct WritevOp {
        static constexpr fiber::event::IoEvent kEvent = fiber::event::IoEvent::Write;
        const struct iovec *iov_ = nullptr;
        int iovcnt_ = 0;

        WritevOp(const struct iovec *iov, int iovcnt) : iov_(iov), iovcnt_(iovcnt) {}

        fiber::common::IoErr once(StreamFd &stream, size_t &out) const {
            return stream.writev_once(iov_, iovcnt_, out);
        }
    };

    template<typename Op>
    class ReadWriteAwaiter;

    using ReadAwaiter = ReadWriteAwaiter<ReadOp>;
    using WriteAwaiter = ReadWriteAwaiter<WriteOp>;
    using ReadvAwaiter = ReadWriteAwaiter<ReadvOp>;
    using WritevAwaiter = ReadWriteAwaiter<WritevOp>;

    StreamFd(fiber::event::EventLoop &loop, int fd);
    ~StreamFd();

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] int fd() const noexcept;
    void close();

    [[nodiscard]] ReadAwaiter read(void *buf, size_t len) noexcept;
    [[nodiscard]] WriteAwaiter write(const void *buf, size_t len) noexcept;
    [[nodiscard]] ReadvAwaiter readv(const struct iovec *iov, int iovcnt) noexcept;
    [[nodiscard]] WritevAwaiter writev(const struct iovec *iov, int iovcnt) noexcept;


private:
    friend struct CrossThreadWaiter;

    struct StreamItem : fiber::event::Poller::Item {
        StreamFd *stream = nullptr;
    };

    template<fiber::event::IoEvent Event>
        requires(ReadWriteEvent<Event>)
    LocalThreadWaiter *&local_waiter_slot() noexcept {
        if constexpr (Event == fiber::event::IoEvent::Read) {
            return local_read_waiter_;
        }
        return local_write_waiter_;
    }

    template<fiber::event::IoEvent Event>
        requires(ReadWriteEvent<Event>)
    CrossThreadWaiter *&cross_waiter_slot() noexcept {
        if constexpr (Event == fiber::event::IoEvent::Read) {
            return cross_read_waiter_;
        }
        return cross_write_waiter_;
    }

    template<fiber::event::IoEvent Event>
        requires(ReadWriteEvent<Event>)
    bool &local_waiting_slot() noexcept {
        if constexpr (Event == fiber::event::IoEvent::Read) {
            return local_read_waiting_;
        }
        return local_write_waiting_;
    }

    template<fiber::event::IoEvent Event, typename Waiter>
        requires(ReadWriteEvent<Event> && StreamWaiter<Waiter>)
    fiber::common::IoErr begin_event(Waiter *waiter) noexcept {
        FIBER_ASSERT(loop_.in_loop());
        FIBER_ASSERT(waiter);
        if (fd_ < 0) {
            return fiber::common::IoErr::BadFd;
        }

        FIBER_ASSERT((watching_ & Event) == fiber::event::IoEvent::None);
        fiber::event::IoEvent desired = watching_ | Event;

        fiber::common::IoErr rc = fiber::common::IoErr::None;
        if (!registered_) {
            rc = loop_.poller().add(fd_, desired, &item_, fiber::event::Poller::Mode::OneShot);
            if (rc == fiber::common::IoErr::None) {
                registered_ = true;
            }
        } else {
            rc = loop_.poller().mod(fd_, desired, &item_, fiber::event::Poller::Mode::OneShot);
        }
        if (rc != fiber::common::IoErr::None) {
            return rc;
        }
        watching_ = desired;
        auto &local_waiter = local_waiter_slot<Event>();
        auto &local_waiting = local_waiting_slot<Event>();
        FIBER_ASSERT(local_waiter == nullptr);
        if constexpr (std::is_same<LocalThreadWaiter, Waiter>::value) {
            local_waiter = waiter;
            local_waiting = true;
        } else {
            auto &cross_waiter = cross_waiter_slot<Event>();
            cross_waiter = waiter;
            local_waiting = false;
        }
        return fiber::common::IoErr::None;
    }

    template<fiber::event::IoEvent Event, typename Waiter>
        requires(ReadWriteEvent<Event> && StreamWaiter<Waiter>)
    fiber::common::IoErr cancel_event(Waiter *waiter) noexcept {
        FIBER_ASSERT(loop_.in_loop());
        FIBER_ASSERT(waiter);
        auto &local_waiter = local_waiter_slot<Event>();
        FIBER_ASSERT(static_cast<void *>(local_waiter) == static_cast<void *>(waiter));
        local_waiter = nullptr;
        local_waiting_slot<Event>() = false;
        fiber::event::IoEvent desired = watching_ & ~Event;
        if (desired == watching_) {
            return fiber::common::IoErr::None;
        }
        fiber::common::IoErr io_err = common::IoErr::None;
        if (registered_ && desired != fiber::event::IoEvent::None) {
            io_err = loop_.poller().mod(fd_, desired, &item_, fiber::event::Poller::Mode::OneShot);
        }
        watching_ = desired;
        return io_err;
    }


    fiber::common::IoErr read_once(void *buf, size_t len, size_t &out);
    fiber::common::IoErr write_once(const void *buf, size_t len, size_t &out);
    fiber::common::IoErr readv_once(const struct iovec *iov, int iovcnt, size_t &out);
    fiber::common::IoErr writev_once(const struct iovec *iov, int iovcnt, size_t &out);

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
    bool registered_ = false;
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
struct CrossThreadWaiter : WaiterBase {
    fiber::event::EventLoop *loop_ = nullptr;
    // used to notify io-loop watch event and caller-loop resume
    fiber::event::EventLoop::NotifyEntry notify_entry_{};
    // if the state_ is Notify_Watch. but the caller cancel waiting, notify the io-loop unwatch event.
    fiber::event::EventLoop::NotifyEntry cancel_entry_{};
    std::atomic<WaiterState> state_{WaiterState::Notify_Watch};

    void cancel_wait() noexcept;

    static void do_notify_resume(CrossThreadWaiter *waiter) noexcept;
    static void on_notify_watch(CrossThreadWaiter *waiter);
    static void on_notify_cancel(CrossThreadWaiter *waiter);
    static void on_notify_resume(CrossThreadWaiter *waiter);
};

template<typename Op>
class StreamFd::ReadWriteAwaiter : public LocalThreadWaiter {
public:
    template<typename... Args>
        requires(std::is_constructible_v<Op, Args...>)
    ReadWriteAwaiter(StreamFd &stream, Args &&...args) noexcept(std::is_nothrow_constructible_v<Op, Args...>) :
        op_(std::forward<Args>(args)...) {
        stream_ = &stream;
        event_ = Op::kEvent;
    }

    ReadWriteAwaiter(const ReadWriteAwaiter &) = delete;
    ReadWriteAwaiter &operator=(const ReadWriteAwaiter &) = delete;
    ReadWriteAwaiter(ReadWriteAwaiter &&) = delete;
    ReadWriteAwaiter &operator=(ReadWriteAwaiter &&) = delete;
    ~ReadWriteAwaiter();

    bool await_ready() noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> handle);
    // invoke io operation in this function according to Op, maybe EAGAIN.
    fiber::common::IoResult<size_t> await_resume() noexcept;

private:
    friend class StreamFd;
    static_assert(Op::kEvent == fiber::event::IoEvent::Read || Op::kEvent == fiber::event::IoEvent::Write);
    Op op_;
    bool waiting_ = false;
    CrossThreadWaiter *waiter_ = nullptr;
    size_t result_ = 0;
    bool completed_ = false;
};


} // namespace fiber::net::detail

#endif // FIBER_NET_DETAIL_STREAM_FD_H
