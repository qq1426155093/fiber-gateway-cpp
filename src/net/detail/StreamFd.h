#ifndef FIBER_NET_DETAIL_STREAM_FD_H
#define FIBER_NET_DETAIL_STREAM_FD_H

#include <concepts>
#include <coroutine>
#include <cstddef>
#include <optional>
#include <sys/uio.h>
#include <type_traits>
#include <utility>

#include "../../common/IoError.h"
#include "../../common/NonCopyable.h"
#include "../../common/NonMovable.h"
#include "../../event/EventLoop.h"
#include "RWFd.h"

namespace fiber::net::detail {

template<fiber::event::IoEvent Event>
concept ReadWriteEvent = (Event == fiber::event::IoEvent::Read || Event == fiber::event::IoEvent::Write);

/**
 * forbidden read/read and write/write in multi-coroutine, but read and write can overlap.
 * no internal read/write lock is enforced for performance.
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
    [[nodiscard]] fiber::event::EventLoop &loop() const noexcept;
    void close();

    [[nodiscard]] ReadAwaiter read(void *buf, size_t len) noexcept;
    [[nodiscard]] WriteAwaiter write(const void *buf, size_t len) noexcept;
    [[nodiscard]] ReadvAwaiter readv(const struct iovec *iov, int iovcnt) noexcept;
    [[nodiscard]] WritevAwaiter writev(const struct iovec *iov, int iovcnt) noexcept;
    [[nodiscard]] fiber::common::IoResult<size_t> try_read(void *buf, size_t len) noexcept;
    [[nodiscard]] fiber::common::IoResult<size_t> try_write(const void *buf, size_t len) noexcept;
    [[nodiscard]] fiber::common::IoResult<size_t> try_readv(const struct iovec *iov, int iovcnt) noexcept;
    [[nodiscard]] fiber::common::IoResult<size_t> try_writev(const struct iovec *iov, int iovcnt) noexcept;

private:
    template<typename Op>
    friend class ReadWriteAwaiter;

    fiber::common::IoErr read_once(void *buf, size_t len, size_t &out);
    fiber::common::IoErr write_once(const void *buf, size_t len, size_t &out);
    fiber::common::IoErr readv_once(const struct iovec *iov, int iovcnt, size_t &out);
    fiber::common::IoErr writev_once(const struct iovec *iov, int iovcnt, size_t &out);

    RWFd rwfd_;
};

template<typename Op>
class StreamFd::ReadWriteAwaiter {
public:
    template<typename... Args>
        requires(std::is_constructible_v<Op, Args...>)
    ReadWriteAwaiter(StreamFd &stream, Args &&...args) noexcept(std::is_nothrow_constructible_v<Op, Args...>)
        : stream_(&stream), op_(std::forward<Args>(args)...) {
    }

    ReadWriteAwaiter(const ReadWriteAwaiter &) = delete;
    ReadWriteAwaiter &operator=(const ReadWriteAwaiter &) = delete;
    ReadWriteAwaiter(ReadWriteAwaiter &&) = delete;
    ReadWriteAwaiter &operator=(ReadWriteAwaiter &&) = delete;
    ~ReadWriteAwaiter();

    bool await_ready() noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> handle);
    fiber::common::IoResult<size_t> await_resume() noexcept;

private:
    static_assert(ReadWriteEvent<Op::kEvent>);
    using EventWaiter = typename RWFd::template WaitAwaiter<Op::kEvent>;

    StreamFd *stream_ = nullptr;
    Op op_;
    fiber::common::IoErr err_{fiber::common::IoErr::None};
    std::optional<EventWaiter> waiter_{};
    bool waiting_ = false;
    size_t result_ = 0;
    bool completed_ = false;
};

} // namespace fiber::net::detail

#endif // FIBER_NET_DETAIL_STREAM_FD_H
