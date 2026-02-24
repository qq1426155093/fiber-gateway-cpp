#ifndef FIBER_NET_DETAIL_ACCEPT_FD_H
#define FIBER_NET_DETAIL_ACCEPT_FD_H

#include <cerrno>
#include <coroutine>
#include <unistd.h>

#include "../../common/Assert.h"
#include "../../common/IoError.h"
#include "../../common/NonCopyable.h"
#include "../../common/NonMovable.h"
#include "../../event/EventLoop.h"
#include "Efd.h"

namespace fiber::net::detail {

template<typename Traits>
class AcceptFd : public common::NonCopyable, public common::NonMovable {
public:
    using Address = typename Traits::Address;
    using ListenOptions = typename Traits::ListenOptions;
    using AcceptResult = typename Traits::AcceptResult;

    class AcceptAwaiter;

    explicit AcceptFd(fiber::event::EventLoop &loop) : efd_(loop, this, &AcceptFd::on_efd_events) {}

    ~AcceptFd() {
        if (!efd_.valid()) {
            return;
        }
        if (efd_.loop().in_loop()) {
            close();
            return;
        }
        FIBER_ASSERT(false);
    }

    fiber::common::IoResult<void> bind(const Address &addr, const ListenOptions &options) {
        if (efd_.valid()) {
            return std::unexpected(fiber::common::IoErr::Already);
        }
        auto fd_result = Traits::bind(addr, options);
        if (!fd_result) {
            return std::unexpected(fd_result.error());
        }
        fiber::common::IoErr attach_err = efd_.attach(*fd_result);
        if (attach_err != fiber::common::IoErr::None) {
            ::close(*fd_result);
            return std::unexpected(attach_err);
        }
        return {};
    }

    [[nodiscard]] bool valid() const noexcept { return efd_.valid(); }

    [[nodiscard]] int fd() const noexcept { return efd_.fd(); }

    void close() {
        FIBER_ASSERT(efd_.loop().in_loop());
        if (!efd_.valid()) {
            return;
        }
        auto *waiter = waiter_;
        waiter_ = nullptr;
        std::coroutine_handle<> handle{};
        if (waiter) {
            waiter->result_ = std::unexpected(fiber::common::IoErr::Canceled);
            waiter->waiting_ = false;
            handle = waiter->handle_;
            waiter->handle_ = {};
        }
        efd_.close_fd();
        if (handle) {
            handle.resume();
        }
    }

    [[nodiscard]] AcceptAwaiter accept() noexcept { return AcceptAwaiter(*this); }

private:
    friend class AcceptAwaiter;

    bool begin_wait(AcceptAwaiter *awaiter) {
        FIBER_ASSERT(efd_.loop().in_loop());
        if (!awaiter) {
            return false;
        }
        awaiter->result_ = AcceptResult{};
        if (!efd_.valid()) {
            awaiter->result_ = std::unexpected(fiber::common::IoErr::BadFd);
            return false;
        }
        if (waiter_) {
            awaiter->result_ = std::unexpected(fiber::common::IoErr::Busy);
            return false;
        }
        AcceptResult out;
        fiber::common::IoErr err = Traits::accept_once(efd_.fd(), out);
        if (err == fiber::common::IoErr::None) {
            awaiter->result_ = out;
            return false;
        }
        if (err != fiber::common::IoErr::WouldBlock) {
            awaiter->result_ = std::unexpected(err);
            return false;
        }
        fiber::common::IoErr watch_err = watch_read();
        if (watch_err != fiber::common::IoErr::None) {
            awaiter->result_ = std::unexpected(watch_err);
            return false;
        }
        waiter_ = awaiter;
        awaiter->waiting_ = true;
        return true;
    }

    void cancel_wait(AcceptAwaiter *awaiter) {
        FIBER_ASSERT(efd_.loop().in_loop());
        FIBER_ASSERT(awaiter == waiter_);
        waiter_ = nullptr;
        awaiter->waiting_ = false;
        unwatch_read();
    }

    fiber::common::IoErr watch_read() {
        return efd_.watch_add(fiber::event::IoEvent::Read);
    }

    void unwatch_read() {
        (void) efd_.watch_del(fiber::event::IoEvent::Read);
    }

    void handle_acceptable() {
        if (!waiter_) {
            unwatch_read();
            return;
        }
        AcceptResult out;
        fiber::common::IoErr err = Traits::accept_once(efd_.fd(), out);
        if (err == fiber::common::IoErr::WouldBlock) {
            return;
        }
        AcceptAwaiter *waiter = waiter_;
        waiter_ = nullptr;
        waiter->waiting_ = false;
        unwatch_read();
        if (err == fiber::common::IoErr::None) {
            waiter->result_ = out;
        } else {
            waiter->result_ = std::unexpected(err);
        }
        waiter->handle_.resume();
    }

    static void on_efd_events(void *owner, fiber::event::IoEvent events) {
        if (!owner) {
            return;
        }
        if (!fiber::event::any(events & fiber::event::IoEvent::Read)) {
            return;
        }
        static_cast<AcceptFd *>(owner)->handle_acceptable();
    }

    Efd efd_;
    AcceptAwaiter *waiter_ = nullptr;
};

template<typename Traits>
class AcceptFd<Traits>::AcceptAwaiter {
public:
    explicit AcceptAwaiter(AcceptFd &acceptor) noexcept : acceptor_(&acceptor) {}

    AcceptAwaiter(const AcceptAwaiter &) = delete;
    AcceptAwaiter &operator=(const AcceptAwaiter &) = delete;
    AcceptAwaiter(AcceptAwaiter &&) = delete;
    AcceptAwaiter &operator=(AcceptAwaiter &&) = delete;

    ~AcceptAwaiter() {
        if (!waiting_) {
            return;
        }
        FIBER_ASSERT(acceptor_->efd_.loop().in_loop());
        acceptor_->cancel_wait(this);
    }

    bool await_ready() noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle) {
        handle_ = handle;
        return acceptor_->begin_wait(this);
    }

    fiber::common::IoResult<AcceptResult> await_resume() noexcept { return result_; }

private:
    friend class AcceptFd;

    AcceptFd *acceptor_ = nullptr;
    std::coroutine_handle<> handle_{};
    fiber::common::IoResult<AcceptResult> result_{};
    bool waiting_ = false;
};

} // namespace fiber::net::detail

#endif // FIBER_NET_DETAIL_ACCEPT_FD_H
