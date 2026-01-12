#include "StreamFd.h"

#include <cerrno>
#include <new>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include "../../common/Assert.h"

namespace fiber::net::detail {

namespace {

constexpr int kInvalidFd = -1;


}
// namespace


StreamFd::StreamFd(fiber::event::EventLoop &loop, int fd) : loop_(loop), fd_(fd) {
    item_.stream = this;
    item_.callback = &StreamFd::on_events;
}

StreamFd::~StreamFd() {
    if (fd_ < 0) {
        return;
    }
    if (loop_.in_loop()) {
        close();
        return;
    }
    FIBER_ASSERT(false);
}

bool StreamFd::valid() const noexcept { return fd_ >= 0; }

int StreamFd::fd() const noexcept { return fd_; }

void StreamFd::close() {
    FIBER_ASSERT(loop_.in_loop());
    if (fd_ < 0) {
        return;
    }
    if (watching_ != fiber::event::IoEvent::None) {
        loop_.poller().del(fd_);
        watching_ = fiber::event::IoEvent::None;
    }
    if (local_read_waiter_) {
        if (local_read_waiting_) {
            LocalThreadWaiter *waiter = local_read_waiter_;
            local_read_waiter_ = nullptr;
            waiter->err_ = fiber::common::IoErr::Canceled;
            waiter->coro_.resume();
        } else {
            CrossThreadWaiter *waiter = cross_read_waiter_;
            cross_read_waiter_ = nullptr;
            waiter->err_ = fiber::common::IoErr::Canceled;
            CrossThreadWaiter::do_notify_resume(waiter);
        }
    }

    if (local_write_waiter_) {
        if (local_write_waiting_) {
            LocalThreadWaiter *waiter = local_write_waiter_;
            local_write_waiter_ = nullptr;
            waiter->err_ = fiber::common::IoErr::Canceled;
            waiter->coro_.resume();
        } else {
            CrossThreadWaiter *waiter = cross_write_waiter_;
            cross_write_waiter_ = nullptr;
            waiter->err_ = fiber::common::IoErr::Canceled;
            CrossThreadWaiter::do_notify_resume(waiter);
        }
    }
    int fd = fd_;
    fd_ = kInvalidFd;
    ::close(fd);
}

StreamFd::ReadAwaiter StreamFd::read(void *buf, size_t len) noexcept { return {*this, buf, len}; }

StreamFd::WriteAwaiter StreamFd::write(const void *buf, size_t len) noexcept { return {*this, buf, len}; }

StreamFd::ReadvAwaiter StreamFd::readv(const struct iovec *iov, int iovcnt) noexcept { return {*this, iov, iovcnt}; }

StreamFd::WritevAwaiter StreamFd::writev(const struct iovec *iov, int iovcnt) noexcept { return {*this, iov, iovcnt}; }

fiber::common::IoErr StreamFd::read_once(void *buf, size_t len, size_t &out) {
    out = 0;
    if (fd_ < 0) {
        return fiber::common::IoErr::BadFd;
    }
    for (;;) {
        ssize_t rc = ::recv(fd_, buf, len, 0);
        if (rc >= 0) {
            out = static_cast<size_t>(rc);
            return fiber::common::IoErr::None;
        }
        int err = errno;
        if (err == EINTR) {
            continue;
        }
        if (err == EAGAIN || err == EWOULDBLOCK) {
            return fiber::common::IoErr::WouldBlock;
        }
        return fiber::common::io_err_from_errno(err);
    }
}

fiber::common::IoErr StreamFd::write_once(const void *buf, size_t len, size_t &out) {
    out = 0;
    if (fd_ < 0) {
        return fiber::common::IoErr::BadFd;
    }
    for (;;) {
        ssize_t rc = ::send(fd_, buf, len, 0);
        if (rc >= 0) {
            out = static_cast<size_t>(rc);
            return fiber::common::IoErr::None;
        }
        int err = errno;
        if (err == EINTR) {
            continue;
        }
        if (err == EAGAIN || err == EWOULDBLOCK) {
            return fiber::common::IoErr::WouldBlock;
        }
        return fiber::common::io_err_from_errno(err);
    }
}

fiber::common::IoErr StreamFd::readv_once(const struct iovec *iov, int iovcnt, size_t &out) {
    out = 0;
    if (fd_ < 0) {
        return fiber::common::IoErr::BadFd;
    }
    for (;;) {
        ssize_t rc = ::readv(fd_, iov, iovcnt);
        if (rc >= 0) {
            out = static_cast<size_t>(rc);
            return fiber::common::IoErr::None;
        }
        int err = errno;
        if (err == EINTR) {
            continue;
        }
        if (err == EAGAIN || err == EWOULDBLOCK) {
            return fiber::common::IoErr::WouldBlock;
        }
        return fiber::common::io_err_from_errno(err);
    }
}

fiber::common::IoErr StreamFd::writev_once(const struct iovec *iov, int iovcnt, size_t &out) {
    out = 0;
    if (fd_ < 0) {
        return fiber::common::IoErr::BadFd;
    }
    for (;;) {
        ssize_t rc = ::writev(fd_, iov, iovcnt);
        if (rc >= 0) {
            out = static_cast<size_t>(rc);
            return fiber::common::IoErr::None;
        }
        int err = errno;
        if (err == EINTR) {
            continue;
        }
        if (err == EAGAIN || err == EWOULDBLOCK) {
            return fiber::common::IoErr::WouldBlock;
        }
        return fiber::common::io_err_from_errno(err);
    }
}

void StreamFd::handle_events(fiber::event::IoEvent events) {
    FIBER_ASSERT(loop_.in_loop());
    if (!fiber::event::any(events)) {
        return;
    }
    fiber::event::IoEvent desired = watching_ & ~events;
    if (desired == fiber::event::IoEvent::None) {
        loop_.poller().del(fd_);
    } else {
        loop_.poller().mod(fd_, desired, &item_);
    }
    watching_ = desired;

    if (fiber::event::any(events & fiber::event::IoEvent::Read)) {
        if (local_read_waiting_) {
            LocalThreadWaiter *waiter = local_read_waiter_;
            FIBER_ASSERT(waiter);
            local_read_waiter_ = nullptr;
            waiter->coro_.resume();
        } else {
            CrossThreadWaiter *waiter = cross_read_waiter_;
            FIBER_ASSERT(waiter);
            cross_read_waiter_ = nullptr;
            CrossThreadWaiter::do_notify_resume(waiter);
        }
    }
    if (fiber::event::any(events & fiber::event::IoEvent::Write)) {
        if (local_write_waiting_) {
            LocalThreadWaiter *waiter = local_write_waiter_;
            FIBER_ASSERT(waiter);
            local_write_waiter_ = nullptr;
            waiter->coro_.resume();
        } else {
            CrossThreadWaiter *waiter = cross_write_waiter_;
            FIBER_ASSERT(waiter);
            cross_write_waiter_ = nullptr;
            CrossThreadWaiter::do_notify_resume(waiter);
        }
    }
}

void StreamFd::on_events(fiber::event::Poller::Item *item, int fd, fiber::event::IoEvent events) {
    (void) fd;
    if (!item) {
        return;
    }
    auto *stream_item = static_cast<StreamItem *>(item);
    if (!stream_item->stream) {
        return;
    }
    stream_item->stream->handle_events(events);
}

void CrossThreadWaiter::on_notify_cancel(CrossThreadWaiter *waiter) {
    WaiterState state = waiter->state_.load(std::memory_order_relaxed);
    StreamFd *stream = waiter->stream_;
    FIBER_ASSERT(stream->loop_.in_loop());
    if (state == WaiterState::Request_Cancel) {
        // waiting
        if (waiter->event_ == fiber::event::IoEvent::Read) {
            stream->cancel_event<fiber::event::IoEvent::Read, CrossThreadWaiter>(waiter);
        } else {
            stream->cancel_event<fiber::event::IoEvent::Write, CrossThreadWaiter>(waiter);
        }
    } else {
        FIBER_ASSERT(state == WaiterState::Waiting_Cancel);
    }

    delete waiter;
}
void CrossThreadWaiter::cancel_wait() noexcept {
    WaiterState state = state_.load(std::memory_order_acquire);
    WaiterState expected;
    for (;;) {
        switch (state) {
            case WaiterState::Notify_Watch:
            case WaiterState::Notify_Resume:
                expected = WaiterState::Canceled;
                break;
            case WaiterState::Watching_Event:
                expected = WaiterState::Request_Cancel;
                break;
            default:
                FIBER_ASSERT(false);
        }
        if (state_.compare_exchange_weak(state, expected, std::memory_order_acq_rel, std::memory_order_acquire)) {
            break;
        }
    }

    if (expected == WaiterState::Request_Cancel) {
        stream_->loop_.post<CrossThreadWaiter, &CrossThreadWaiter::cancel_entry_, &CrossThreadWaiter::on_notify_cancel,
                            &CrossThreadWaiter::on_cancel_wait>(*this);
    }
}

void CrossThreadWaiter::do_notify_resume(CrossThreadWaiter *waiter) noexcept {
    WaiterState state = waiter->state_.load(std::memory_order_acquire);
    WaiterState expected;

    for (;;) {
        switch (state) {
            case WaiterState::Watching_Event:
                expected = WaiterState::Notify_Resume;
                break;
            case WaiterState::Request_Cancel:
                expected = WaiterState::Waiting_Cancel;
                break;
            default:
                FIBER_ASSERT(false);
        }
        if (waiter->state_.compare_exchange_weak(state, expected, std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
            break;
        }
    }
    if (expected == WaiterState::Notify_Resume) {
        waiter->loop_->post<CrossThreadWaiter, &CrossThreadWaiter::cancel_entry_, &CrossThreadWaiter::on_notify_resume,
                            &CrossThreadWaiter::on_cancel_wait>(*waiter);
    }
}

void CrossThreadWaiter::on_notify_watch(CrossThreadWaiter *waiter) {
    FIBER_ASSERT(waiter);
    FIBER_ASSERT(waiter->stream_);

    WaiterState old = waiter->state_.exchange(WaiterState::Watching_Event, std::memory_order_acq_rel);
    if (old == WaiterState::Canceled) {
        delete waiter;
        return;
    }
    FIBER_ASSERT(old == WaiterState::Notify_Watch);

    StreamFd *stream = waiter->stream_;
    fiber::common::IoErr err = fiber::common::IoErr::None;
    if (waiter->event_ == fiber::event::IoEvent::Read) {
        err = stream->begin_event<fiber::event::IoEvent::Read, CrossThreadWaiter>(waiter);
    } else {
        err = stream->begin_event<fiber::event::IoEvent::Write, CrossThreadWaiter>(waiter);
    }
    if (err != fiber::common::IoErr::None) {
        waiter->err_ = err;
        // after this on_notify_cancel absolutely not be invoked.
        do_notify_resume(waiter);
    }
    // wait for io-event notify
}

void CrossThreadWaiter::on_cancel_wait(CrossThreadWaiter *waiter) {
    FIBER_ASSERT(waiter);
    delete waiter;
}

void CrossThreadWaiter::on_notify_resume(CrossThreadWaiter *waiter) {
    FIBER_ASSERT(waiter);
    FIBER_ASSERT(waiter->loop_->in_loop());

    if (waiter->state_.load(std::memory_order_relaxed) == WaiterState::Canceled) {
        delete waiter;
        return;
    }

    // Awaiter will delete this on the await_ready
    waiter->coro_.resume();
}


template<typename Op>
StreamFd::ReadWriteAwaiter<Op>::~ReadWriteAwaiter() {
    if (!waiting_) {
        FIBER_ASSERT(waiter_ == nullptr);
        return;
    }
    if (waiter_) {
        FIBER_ASSERT(!stream_->loop_.in_loop());
        auto *waiter = waiter_;
        waiter->cancel_wait();
        waiter_ = nullptr;
        return;
    }
    FIBER_ASSERT(stream_->loop_.in_loop());
    stream_->cancel_event<Op::kEvent, LocalThreadWaiter>(this);
}

template<typename Op>
bool StreamFd::ReadWriteAwaiter<Op>::await_suspend(std::coroutine_handle<> handle) {
    coro_ = handle;
    err_ = fiber::common::IoErr::None;
    completed_ = false;

    size_t out = 0;
    fiber::common::IoErr err = op_.once(*stream_, out);
    if (err == fiber::common::IoErr::None) {
        result_ = out;
        completed_ = true;
        return false;
    }
    if (err != fiber::common::IoErr::WouldBlock) {
        err_ = err;
        completed_ = true;
        return false;
    }

    waiting_ = true;
    if (stream_->loop_.in_loop()) {
        fiber::common::IoErr watch_err = stream_->begin_event<Op::kEvent, LocalThreadWaiter>(this);
        if (watch_err != fiber::common::IoErr::None) {
            err_ = watch_err;
            completed_ = true;
            waiting_ = false;
            return false;
        }
        return true;
    }


    auto *current = fiber::event::EventLoop::current_or_null();
    FIBER_ASSERT(current != nullptr);
    auto *waiter = new (std::nothrow) CrossThreadWaiter();
    if (!waiter) {
        err_ = fiber::common::IoErr::NoMem;
        completed_ = true;
        return false;
    }
    waiter->stream_ = stream_;
    waiter->event_ = Op::kEvent;
    waiter->coro_ = handle;
    waiter->loop_ = current;
    waiter_ = waiter;
    stream_->loop_.post<CrossThreadWaiter, &CrossThreadWaiter::notify_entry_, &CrossThreadWaiter::on_notify_watch,
                        &CrossThreadWaiter::on_cancel_wait>(*waiter);
    return true;
}

template<typename Op>
fiber::common::IoResult<size_t> StreamFd::ReadWriteAwaiter<Op>::await_resume() noexcept {
    waiting_ = false;
    if (completed_) {
        completed_ = false;
        if (err_ == fiber::common::IoErr::None) {
            return result_;
        }
        return std::unexpected(err_);
    }

    fiber::common::IoErr err = err_;
    size_t out = 0;
    CrossThreadWaiter *waiter = waiter_;
    if (waiter) {
        err = waiter->err_;
        waiter_ = nullptr;
        delete waiter;
    }
    if (err == fiber::common::IoErr::None) {
        err = op_.once(*stream_, out);
    }

    if (err == fiber::common::IoErr::None) {
        return out;
    }
    return std::unexpected(err);
}

template class StreamFd::ReadWriteAwaiter<StreamFd::ReadOp>;
template class StreamFd::ReadWriteAwaiter<StreamFd::WriteOp>;
template class StreamFd::ReadWriteAwaiter<StreamFd::ReadvOp>;
template class StreamFd::ReadWriteAwaiter<StreamFd::WritevOp>;

} // namespace fiber::net::detail
