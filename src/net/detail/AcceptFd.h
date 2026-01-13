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

namespace fiber::net::detail {

template<typename Traits>
class AcceptFd : public common::NonCopyable, public common::NonMovable {
public:
    using Address = typename Traits::Address;
    using ListenOptions = typename Traits::ListenOptions;
    using AcceptResult = typename Traits::AcceptResult;

    class AcceptAwaiter;

    explicit AcceptFd(fiber::event::EventLoop &loop) : loop_(loop) {
        item_.acceptor = this;
        item_.callback = &AcceptFd::on_acceptable;
    }

    ~AcceptFd() {
        if (fd_ < 0) {
            return;
        }
        if (loop_.in_loop()) {
            close();
            return;
        }
        FIBER_ASSERT(false);
    }

    fiber::common::IoResult<void> bind(const Address &addr, const ListenOptions &options) {
        if (fd_ >= 0) {
            return std::unexpected(fiber::common::IoErr::Already);
        }
        auto fd_result = Traits::bind(addr, options);
        if (!fd_result) {
            return std::unexpected(fd_result.error());
        }
        fd_ = *fd_result;
        return {};
    }

    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

    [[nodiscard]] int fd() const noexcept { return fd_; }

    void close() {
        FIBER_ASSERT(loop_.in_loop());
        if (fd_ < 0) {
            return;
        }
        int fd = fd_;
        if (watching_) {
            unwatch_read();
        }
        fd_ = kInvalidFd;
        auto *waiter = waiter_;
        waiter_ = nullptr;
        std::coroutine_handle<> handle{};
        if (waiter) {
            waiter->result_ = std::unexpected(fiber::common::IoErr::Canceled);
            waiter->waiting_ = false;
            handle = waiter->handle_;
            waiter->handle_ = {};
        }
        ::close(fd);
        if (handle) {
            handle.resume();
        }
    }

    [[nodiscard]] AcceptAwaiter accept() noexcept { return AcceptAwaiter(*this); }

private:
    friend class AcceptAwaiter;

    struct AcceptItem : fiber::event::Poller::Item {
        AcceptFd *acceptor = nullptr;
    };

    bool begin_wait(AcceptAwaiter *awaiter) {
        FIBER_ASSERT(loop_.in_loop());
        if (!awaiter) {
            return false;
        }
        awaiter->result_ = AcceptResult{};
        if (fd_ < 0) {
            awaiter->result_ = std::unexpected(fiber::common::IoErr::BadFd);
            return false;
        }
        if (waiter_) {
            awaiter->result_ = std::unexpected(fiber::common::IoErr::Busy);
            return false;
        }
        AcceptResult out;
        fiber::common::IoErr err = Traits::accept_once(fd_, out);
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
        FIBER_ASSERT(loop_.in_loop());
        FIBER_ASSERT(awaiter == waiter_);
        waiter_ = nullptr;
        awaiter->waiting_ = false;
        unwatch_read();
    }

    fiber::common::IoErr watch_read() {
        if (watching_) {
            return fiber::common::IoErr::None;
        }
        fiber::common::IoErr err = loop_.poller().add(fd_, fiber::event::IoEvent::Read, &item_);
        if (err != fiber::common::IoErr::None) {
            return err;
        }
        watching_ = true;
        return fiber::common::IoErr::None;
    }

    void unwatch_read() {
        if (!watching_) {
            return;
        }
        loop_.poller().del(fd_);
        watching_ = false;
    }

    void handle_acceptable() {
        if (!waiter_) {
            unwatch_read();
            return;
        }
        AcceptResult out;
        fiber::common::IoErr err = Traits::accept_once(fd_, out);
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

    static void on_acceptable(fiber::event::Poller::Item *item, int fd, fiber::event::IoEvent events) {
        (void) fd;
        (void) events;
        auto *entry = static_cast<AcceptItem *>(item);
        if (!entry || !entry->acceptor) {
            return;
        }
        entry->acceptor->handle_acceptable();
    }

    static constexpr int kInvalidFd = -1;

    fiber::event::EventLoop &loop_;
    AcceptItem item_{};
    int fd_ = kInvalidFd;
    bool watching_ = false;
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
        FIBER_ASSERT(acceptor_->loop_.in_loop());
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
