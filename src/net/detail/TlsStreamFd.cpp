#include "TlsStreamFd.h"

#include <cerrno>
#include <new>
#include <unistd.h>

#include "../../common/Assert.h"

#include <openssl/ssl.h>

namespace fiber::net::detail {

namespace {

constexpr int kInvalidFd = -1;

} // namespace

TlsStreamFd::TlsStreamFd(fiber::event::EventLoop &loop, int fd, bool owns_fd)
    : loop_(loop), fd_(fd), owns_fd_(owns_fd) {
    item_.stream = this;
    item_.callback = &TlsStreamFd::on_events;
}

TlsStreamFd::~TlsStreamFd() {
    if (fd_ < 0) {
        return;
    }
    if (loop_.in_loop()) {
        close();
        return;
    }
    FIBER_ASSERT(false);
}

common::IoResult<void> TlsStreamFd::init(SSL_CTX *ctx, bool is_server) {
    if (!ctx) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (fd_ < 0) {
        return std::unexpected(common::IoErr::BadFd);
    }
    ssl_ = SSL_new(ctx);
    if (!ssl_) {
        return std::unexpected(common::IoErr::NoMem);
    }
    if (SSL_set_fd(ssl_, fd_) != 1) {
        SSL_free(ssl_);
        ssl_ = nullptr;
        return std::unexpected(common::IoErr::Invalid);
    }
    if (is_server) {
        SSL_set_accept_state(ssl_);
    } else {
        SSL_set_connect_state(ssl_);
    }
    handshake_done_ = false;
    return {};
}

bool TlsStreamFd::valid() const noexcept {
    return fd_ >= 0 && ssl_ != nullptr;
}

int TlsStreamFd::fd() const noexcept {
    return fd_;
}

std::string TlsStreamFd::selected_alpn() const noexcept {
    if (!ssl_) {
        return {};
    }
    const unsigned char *proto = nullptr;
    unsigned int proto_len = 0;
    SSL_get0_alpn_selected(ssl_, &proto, &proto_len);
    if (!proto || proto_len == 0) {
        return {};
    }
    return std::string(reinterpret_cast<const char *>(proto), proto_len);
}

void TlsStreamFd::close() {
    FIBER_ASSERT(loop_.in_loop());
    if (fd_ < 0) {
        return;
    }
    if (registered_) {
        loop_.poller().del(fd_);
        registered_ = false;
        watching_ = fiber::event::IoEvent::None;
    }
    if (local_waiter_) {
        bool local = local_waiting_;
        TlsWaiterBase *waiter = local ? static_cast<TlsWaiterBase *>(local_waiter_)
                                   : static_cast<TlsWaiterBase *>(cross_waiter_);
        if (local) {
            local_waiter_ = nullptr;
        } else {
            cross_waiter_ = nullptr;
        }
        local_waiting_ = false;
        busy_ = false;
        waiter->err_ = fiber::common::IoErr::Canceled;
        if (local) {
            waiter->coro_.resume();
        } else {
            TlsCrossThreadWaiter::do_notify_resume(static_cast<TlsCrossThreadWaiter *>(waiter));
        }
    }
    if (ssl_) {
        SSL_shutdown(ssl_);
        SSL_free(ssl_);
        ssl_ = nullptr;
        handshake_done_ = false;
    }
    int fd = fd_;
    fd_ = kInvalidFd;
    if (owns_fd_) {
        ::close(fd);
    }
}

TlsStreamFd::ReadAwaiter TlsStreamFd::read(void *buf, size_t len) noexcept {
    return {*this, buf, len};
}

TlsStreamFd::WriteAwaiter TlsStreamFd::write(const void *buf, size_t len) noexcept {
    return {*this, buf, len};
}

fiber::common::IoResult<size_t> TlsStreamFd::try_read(void *buf, size_t len) noexcept {
    if (busy_) {
        return std::unexpected(fiber::common::IoErr::Busy);
    }
    busy_ = true;
    size_t out = 0;
    fiber::event::IoEvent wait_event = fiber::event::IoEvent::None;
    fiber::common::IoErr err = read_once(buf, len, out, wait_event);
    busy_ = false;
    if (err == fiber::common::IoErr::WouldBlock && wait_event == fiber::event::IoEvent::None) {
        return std::unexpected(fiber::common::IoErr::Invalid);
    }
    if (err == fiber::common::IoErr::None) {
        return out;
    }
    return std::unexpected(err);
}

fiber::common::IoResult<size_t> TlsStreamFd::try_write(const void *buf, size_t len) noexcept {
    if (busy_) {
        return std::unexpected(fiber::common::IoErr::Busy);
    }
    busy_ = true;
    size_t out = 0;
    fiber::event::IoEvent wait_event = fiber::event::IoEvent::None;
    fiber::common::IoErr err = write_once(buf, len, out, wait_event);
    busy_ = false;
    if (err == fiber::common::IoErr::WouldBlock && wait_event == fiber::event::IoEvent::None) {
        return std::unexpected(fiber::common::IoErr::Invalid);
    }
    if (err == fiber::common::IoErr::None) {
        return out;
    }
    return std::unexpected(err);
}

TlsStreamFd::HandshakeAwaiter TlsStreamFd::handshake() noexcept {
    return HandshakeAwaiter(*this);
}

TlsStreamFd::ShutdownAwaiter TlsStreamFd::shutdown() noexcept {
    return ShutdownAwaiter(*this);
}

TlsStreamFd::StartResult TlsStreamFd::start_op(TlsWaiterBase *waiter, bool local) noexcept {
    FIBER_ASSERT(loop_.in_loop());
    FIBER_ASSERT(waiter);
    if (busy_) {
        return {false, fiber::common::IoErr::Busy};
    }
    busy_ = true;
    fiber::event::IoEvent wait_event = fiber::event::IoEvent::None;
    fiber::common::IoErr err = waiter->op_(*this, waiter->op_ctx_, wait_event);
    if (err == fiber::common::IoErr::WouldBlock) {
        if (wait_event == fiber::event::IoEvent::None) {
            busy_ = false;
            return {false, fiber::common::IoErr::Invalid};
        }
        fiber::common::IoErr watch_err = begin_event(waiter, wait_event, local);
        if (watch_err != fiber::common::IoErr::None) {
            busy_ = false;
            return {false, watch_err};
        }
        return {true, fiber::common::IoErr::None};
    }
    busy_ = false;
    return {false, err};
}

void TlsStreamFd::complete_wait(TlsWaiterBase *waiter, fiber::common::IoErr err) noexcept {
    bool local = local_waiting_;
    if (local) {
        local_waiter_ = nullptr;
    } else {
        cross_waiter_ = nullptr;
    }
    local_waiting_ = false;
    busy_ = false;
    waiter->err_ = err;
    if (local) {
        waiter->coro_.resume();
    } else {
        TlsCrossThreadWaiter::do_notify_resume(static_cast<TlsCrossThreadWaiter *>(waiter));
    }
}

fiber::common::IoErr TlsStreamFd::begin_event(TlsWaiterBase *waiter,
                                              fiber::event::IoEvent event,
                                              bool local) noexcept {
    FIBER_ASSERT(loop_.in_loop());
    FIBER_ASSERT(waiter);
    if (fd_ < 0) {
        return fiber::common::IoErr::BadFd;
    }
    FIBER_ASSERT(local_waiter_ == nullptr);
    fiber::common::IoErr rc = fiber::common::IoErr::None;
    if (!registered_) {
        rc = loop_.poller().add(fd_, event, &item_, fiber::event::Poller::Mode::OneShot);
        if (rc == fiber::common::IoErr::None) {
            registered_ = true;
        }
    } else {
        rc = loop_.poller().mod(fd_, event, &item_, fiber::event::Poller::Mode::OneShot);
    }
    if (rc != fiber::common::IoErr::None) {
        return rc;
    }
    watching_ = event;
    waiter->event_ = event;
    if (local) {
        local_waiter_ = static_cast<TlsLocalThreadWaiter *>(waiter);
        local_waiting_ = true;
    } else {
        cross_waiter_ = static_cast<TlsCrossThreadWaiter *>(waiter);
        local_waiting_ = false;
    }
    return fiber::common::IoErr::None;
}

fiber::common::IoErr TlsStreamFd::arm_event(TlsWaiterBase *waiter, fiber::event::IoEvent event) noexcept {
    FIBER_ASSERT(loop_.in_loop());
    FIBER_ASSERT(waiter);
    if (fd_ < 0) {
        return fiber::common::IoErr::BadFd;
    }
    fiber::common::IoErr rc = fiber::common::IoErr::None;
    if (!registered_) {
        rc = loop_.poller().add(fd_, event, &item_, fiber::event::Poller::Mode::OneShot);
        if (rc == fiber::common::IoErr::None) {
            registered_ = true;
        }
    } else {
        rc = loop_.poller().mod(fd_, event, &item_, fiber::event::Poller::Mode::OneShot);
    }
    if (rc != fiber::common::IoErr::None) {
        return rc;
    }
    watching_ = event;
    waiter->event_ = event;
    return fiber::common::IoErr::None;
}

fiber::common::IoErr TlsStreamFd::cancel_event(TlsWaiterBase *waiter) noexcept {
    FIBER_ASSERT(loop_.in_loop());
    FIBER_ASSERT(waiter);
    if (local_waiting_) {
        FIBER_ASSERT(static_cast<void *>(local_waiter_) == static_cast<void *>(waiter));
        local_waiter_ = nullptr;
    } else {
        FIBER_ASSERT(static_cast<void *>(cross_waiter_) == static_cast<void *>(waiter));
        cross_waiter_ = nullptr;
    }
    local_waiting_ = false;
    busy_ = false;
    watching_ = fiber::event::IoEvent::None;
    return fiber::common::IoErr::None;
}

fiber::common::IoErr TlsStreamFd::handshake_once(fiber::event::IoEvent &event) noexcept {
    if (fd_ < 0 || !ssl_) {
        return fiber::common::IoErr::BadFd;
    }
    if (handshake_done_) {
        return fiber::common::IoErr::None;
    }
    for (;;) {
        int rc = SSL_do_handshake(ssl_);
        if (rc == 1) {
            handshake_done_ = true;
            return fiber::common::IoErr::None;
        }
        int err = SSL_get_error(ssl_, rc);
        if (err == SSL_ERROR_WANT_READ) {
            event = fiber::event::IoEvent::Read;
            return fiber::common::IoErr::WouldBlock;
        }
        if (err == SSL_ERROR_WANT_WRITE) {
            event = fiber::event::IoEvent::Write;
            return fiber::common::IoErr::WouldBlock;
        }
        if (err == SSL_ERROR_ZERO_RETURN) {
            return fiber::common::IoErr::ConnReset;
        }
        if (err == SSL_ERROR_SYSCALL) {
            int sys_err = errno;
            if (sys_err == EINTR) {
                continue;
            }
            if (sys_err != 0) {
                return fiber::common::io_err_from_errno(sys_err);
            }
            return fiber::common::IoErr::ConnReset;
        }
        return fiber::common::IoErr::Invalid;
    }
}

fiber::common::IoErr TlsStreamFd::shutdown_once(fiber::event::IoEvent &event) noexcept {
    if (fd_ < 0 || !ssl_) {
        return fiber::common::IoErr::BadFd;
    }
    if (!handshake_done_) {
        return fiber::common::IoErr::None;
    }
    for (;;) {
        int rc = SSL_shutdown(ssl_);
        if (rc == 1 || rc == 0) {
            return fiber::common::IoErr::None;
        }
        int err = SSL_get_error(ssl_, rc);
        if (err == SSL_ERROR_WANT_READ) {
            event = fiber::event::IoEvent::Read;
            return fiber::common::IoErr::WouldBlock;
        }
        if (err == SSL_ERROR_WANT_WRITE) {
            event = fiber::event::IoEvent::Write;
            return fiber::common::IoErr::WouldBlock;
        }
        if (err == SSL_ERROR_ZERO_RETURN) {
            return fiber::common::IoErr::None;
        }
        if (err == SSL_ERROR_SYSCALL) {
            int sys_err = errno;
            if (sys_err == EINTR) {
                continue;
            }
            if (sys_err != 0) {
                return fiber::common::io_err_from_errno(sys_err);
            }
            return fiber::common::IoErr::Invalid;
        }
        return fiber::common::IoErr::Invalid;
    }
}

fiber::common::IoErr TlsStreamFd::read_once(void *buf,
                                            size_t len,
                                            size_t &out,
                                            fiber::event::IoEvent &event) noexcept {
    out = 0;
    if (fd_ < 0 || !ssl_) {
        return fiber::common::IoErr::BadFd;
    }
    for (;;) {
        int rc = SSL_read(ssl_, buf, static_cast<int>(len));
        if (rc > 0) {
            out = static_cast<size_t>(rc);
            return fiber::common::IoErr::None;
        }
        int err = SSL_get_error(ssl_, rc);
        if (err == SSL_ERROR_WANT_READ) {
            event = fiber::event::IoEvent::Read;
            return fiber::common::IoErr::WouldBlock;
        }
        if (err == SSL_ERROR_WANT_WRITE) {
            event = fiber::event::IoEvent::Write;
            return fiber::common::IoErr::WouldBlock;
        }
        if (err == SSL_ERROR_ZERO_RETURN) {
            return fiber::common::IoErr::None;
        }
        if (err == SSL_ERROR_SYSCALL) {
            int sys_err = errno;
            if (sys_err == EINTR) {
                continue;
            }
            if (sys_err != 0) {
                return fiber::common::io_err_from_errno(sys_err);
            }
            return fiber::common::IoErr::ConnReset;
        }
        return fiber::common::IoErr::Invalid;
    }
}

fiber::common::IoErr TlsStreamFd::write_once(const void *buf,
                                             size_t len,
                                             size_t &out,
                                             fiber::event::IoEvent &event) noexcept {
    out = 0;
    if (fd_ < 0 || !ssl_) {
        return fiber::common::IoErr::BadFd;
    }
    for (;;) {
        int rc = SSL_write(ssl_, buf, static_cast<int>(len));
        if (rc > 0) {
            out = static_cast<size_t>(rc);
            return fiber::common::IoErr::None;
        }
        int err = SSL_get_error(ssl_, rc);
        if (err == SSL_ERROR_WANT_READ) {
            event = fiber::event::IoEvent::Read;
            return fiber::common::IoErr::WouldBlock;
        }
        if (err == SSL_ERROR_WANT_WRITE) {
            event = fiber::event::IoEvent::Write;
            return fiber::common::IoErr::WouldBlock;
        }
        if (err == SSL_ERROR_ZERO_RETURN) {
            return fiber::common::IoErr::BrokenPipe;
        }
        if (err == SSL_ERROR_SYSCALL) {
            int sys_err = errno;
            if (sys_err == EINTR) {
                continue;
            }
            if (sys_err != 0) {
                return fiber::common::io_err_from_errno(sys_err);
            }
            return fiber::common::IoErr::BrokenPipe;
        }
        return fiber::common::IoErr::Invalid;
    }
}

void TlsStreamFd::handle_events(fiber::event::IoEvent events) {
    FIBER_ASSERT(loop_.in_loop());
    if (!fiber::event::any(events)) {
        return;
    }
    fiber::event::IoEvent desired = watching_ & ~events;
    if (desired != fiber::event::IoEvent::None) {
        loop_.poller().mod(fd_, desired, &item_, fiber::event::Poller::Mode::OneShot);
    }
    watching_ = desired;

    if (!local_waiter_) {
        return;
    }

    TlsWaiterBase *waiter = local_waiting_ ? static_cast<TlsWaiterBase *>(local_waiter_)
                                        : static_cast<TlsWaiterBase *>(cross_waiter_);
    if (!waiter) {
        return;
    }

    fiber::event::IoEvent wait_event = fiber::event::IoEvent::None;
    fiber::common::IoErr err = waiter->op_(*this, waiter->op_ctx_, wait_event);
    if (err == fiber::common::IoErr::WouldBlock) {
        fiber::common::IoErr watch_err = arm_event(waiter, wait_event);
        if (watch_err == fiber::common::IoErr::None) {
            return;
        }
        err = watch_err;
    }
    complete_wait(waiter, err);
}

void TlsStreamFd::on_events(fiber::event::Poller::Item *item, int fd, fiber::event::IoEvent events) {
    (void) fd;
    auto *stream_item = static_cast<StreamItem *>(item);
    if (!stream_item || !stream_item->stream) {
        return;
    }
    stream_item->stream->handle_events(events);
}

void TlsCrossThreadWaiter::on_notify_cancel(TlsCrossThreadWaiter *waiter) {
    TlsWaiterState state = waiter->state_.load(std::memory_order_relaxed);
    TlsStreamFd *stream = waiter->stream_;
    FIBER_ASSERT(stream->loop_.in_loop());
    if (state == TlsWaiterState::Request_Cancel) {
        stream->cancel_event(waiter);
    } else {
        FIBER_ASSERT(state == TlsWaiterState::Waiting_Cancel);
    }
    delete waiter;
}

void TlsCrossThreadWaiter::cancel_wait() noexcept {
    TlsWaiterState state = state_.load(std::memory_order_acquire);
    TlsWaiterState expected;
    for (;;) {
        switch (state) {
        case TlsWaiterState::Notify_Watch:
        case TlsWaiterState::Notify_Resume:
            expected = TlsWaiterState::Canceled;
            break;
        case TlsWaiterState::Watching_Event:
            expected = TlsWaiterState::Request_Cancel;
            break;
        default:
            FIBER_ASSERT(false);
        }
        if (state_.compare_exchange_weak(state, expected, std::memory_order_acq_rel, std::memory_order_acquire)) {
            break;
        }
    }

    if (expected == TlsWaiterState::Request_Cancel) {
        stream_->loop_.post<TlsCrossThreadWaiter, &TlsCrossThreadWaiter::cancel_entry_, &TlsCrossThreadWaiter::on_notify_cancel>(
            *this);
    }
}

void TlsCrossThreadWaiter::do_notify_resume(TlsCrossThreadWaiter *waiter) noexcept {
    TlsWaiterState state = waiter->state_.load(std::memory_order_acquire);
    TlsWaiterState expected;

    for (;;) {
        switch (state) {
        case TlsWaiterState::Watching_Event:
            expected = TlsWaiterState::Notify_Resume;
            break;
        case TlsWaiterState::Request_Cancel:
            expected = TlsWaiterState::Waiting_Cancel;
            break;
        default:
            FIBER_ASSERT(false);
        }
        if (waiter->state_.compare_exchange_weak(state, expected, std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
            break;
        }
    }
    if (expected == TlsWaiterState::Notify_Resume) {
        waiter->loop_->post<TlsCrossThreadWaiter, &TlsCrossThreadWaiter::cancel_entry_, &TlsCrossThreadWaiter::on_notify_resume>(
            *waiter);
    }
}

void TlsCrossThreadWaiter::on_notify_watch(TlsCrossThreadWaiter *waiter) {
    FIBER_ASSERT(waiter);
    FIBER_ASSERT(waiter->stream_);

    TlsWaiterState old = waiter->state_.exchange(TlsWaiterState::Watching_Event, std::memory_order_acq_rel);
    if (old == TlsWaiterState::Canceled) {
        delete waiter;
        return;
    }
    FIBER_ASSERT(old == TlsWaiterState::Notify_Watch);

    TlsStreamFd *stream = waiter->stream_;
    auto result = stream->start_op(waiter, false);
    if (!result.waiting) {
        waiter->err_ = result.err;
        do_notify_resume(waiter);
    }
}

void TlsCrossThreadWaiter::on_notify_resume(TlsCrossThreadWaiter *waiter) {
    FIBER_ASSERT(waiter);
    FIBER_ASSERT(waiter->loop_->in_loop());

    if (waiter->state_.load(std::memory_order_relaxed) == TlsWaiterState::Canceled) {
        delete waiter;
        return;
    }
    waiter->coro_.resume();
}

TlsStreamFd::ReadAwaiter::ReadAwaiter(TlsStreamFd &stream, void *buf, size_t len) noexcept : buf_(buf), len_(len) {
    stream_ = &stream;
    op_ctx_ = this;
    op_ = &ReadAwaiter::do_op;
}

TlsStreamFd::ReadAwaiter::~ReadAwaiter() {
    if (!waiting_) {
        return;
    }
    if (waiter_) {
        FIBER_ASSERT(!stream_->loop_.in_loop());
        waiter_->cancel_wait();
        waiter_ = nullptr;
        return;
    }
    FIBER_ASSERT(stream_->loop_.in_loop());
    stream_->cancel_event(this);
}

bool TlsStreamFd::ReadAwaiter::await_suspend(std::coroutine_handle<> handle) {
    coro_ = handle;
    err_ = fiber::common::IoErr::None;
    completed_ = false;
    if (stream_->loop_.in_loop()) {
        auto result = stream_->start_op(this, true);
        if (result.waiting) {
            waiting_ = true;
            return true;
        }
        err_ = result.err;
        completed_ = true;
        return false;
    }
    err_ = fiber::common::IoErr::NotSupported;
    completed_ = true;
    return false;
}

fiber::common::IoResult<size_t> TlsStreamFd::ReadAwaiter::await_resume() noexcept {
    waiting_ = false;
    if (completed_) {
        completed_ = false;
        if (err_ == fiber::common::IoErr::None) {
            return result_;
        }
        return std::unexpected(err_);
    }

    fiber::common::IoErr err = err_;
    if (waiter_) {
        err = waiter_->err_;
        delete waiter_;
        waiter_ = nullptr;
    }
    if (err == fiber::common::IoErr::None) {
        return result_;
    }
    return std::unexpected(err);
}

fiber::common::IoErr TlsStreamFd::ReadAwaiter::do_op(TlsStreamFd &stream,
                                                     void *ctx,
                                                     fiber::event::IoEvent &event) {
    auto *self = static_cast<ReadAwaiter *>(ctx);
    size_t out = 0;
    fiber::common::IoErr err = stream.read_once(self->buf_, self->len_, out, event);
    self->result_ = out;
    return err;
}

TlsStreamFd::WriteAwaiter::WriteAwaiter(TlsStreamFd &stream, const void *buf, size_t len) noexcept
    : buf_(buf), len_(len) {
    stream_ = &stream;
    op_ctx_ = this;
    op_ = &WriteAwaiter::do_op;
}

TlsStreamFd::WriteAwaiter::~WriteAwaiter() {
    if (!waiting_) {
        return;
    }
    if (waiter_) {
        FIBER_ASSERT(!stream_->loop_.in_loop());
        waiter_->cancel_wait();
        waiter_ = nullptr;
        return;
    }
    FIBER_ASSERT(stream_->loop_.in_loop());
    stream_->cancel_event(this);
}

bool TlsStreamFd::WriteAwaiter::await_suspend(std::coroutine_handle<> handle) {
    coro_ = handle;
    err_ = fiber::common::IoErr::None;
    completed_ = false;
    if (stream_->loop_.in_loop()) {
        auto result = stream_->start_op(this, true);
        if (result.waiting) {
            waiting_ = true;
            return true;
        }
        err_ = result.err;
        completed_ = true;
        return false;
    }
    err_ = fiber::common::IoErr::NotSupported;
    completed_ = true;
    return false;
}

fiber::common::IoResult<size_t> TlsStreamFd::WriteAwaiter::await_resume() noexcept {
    waiting_ = false;
    if (completed_) {
        completed_ = false;
        if (err_ == fiber::common::IoErr::None) {
            return result_;
        }
        return std::unexpected(err_);
    }

    fiber::common::IoErr err = err_;
    if (waiter_) {
        err = waiter_->err_;
        delete waiter_;
        waiter_ = nullptr;
    }
    if (err == fiber::common::IoErr::None) {
        return result_;
    }
    return std::unexpected(err);
}

fiber::common::IoErr TlsStreamFd::WriteAwaiter::do_op(TlsStreamFd &stream,
                                                      void *ctx,
                                                      fiber::event::IoEvent &event) {
    auto *self = static_cast<WriteAwaiter *>(ctx);
    size_t out = 0;
    fiber::common::IoErr err = stream.write_once(self->buf_, self->len_, out, event);
    self->result_ = out;
    return err;
}

TlsStreamFd::HandshakeAwaiter::HandshakeAwaiter(TlsStreamFd &stream) noexcept {
    stream_ = &stream;
    op_ctx_ = this;
    op_ = &HandshakeAwaiter::do_op;
}

TlsStreamFd::HandshakeAwaiter::~HandshakeAwaiter() {
    if (!waiting_) {
        return;
    }
    if (waiter_) {
        FIBER_ASSERT(!stream_->loop_.in_loop());
        waiter_->cancel_wait();
        waiter_ = nullptr;
        return;
    }
    FIBER_ASSERT(stream_->loop_.in_loop());
    stream_->cancel_event(this);
}

bool TlsStreamFd::HandshakeAwaiter::await_suspend(std::coroutine_handle<> handle) {
    coro_ = handle;
    err_ = fiber::common::IoErr::None;
    completed_ = false;
    if (stream_->loop_.in_loop()) {
        auto result = stream_->start_op(this, true);
        if (result.waiting) {
            waiting_ = true;
            return true;
        }
        err_ = result.err;
        completed_ = true;
        return false;
    }
    err_ = fiber::common::IoErr::NotSupported;
    completed_ = true;
    return false;
}

fiber::common::IoResult<void> TlsStreamFd::HandshakeAwaiter::await_resume() noexcept {
    waiting_ = false;
    if (completed_) {
        completed_ = false;
        if (err_ == fiber::common::IoErr::None) {
            return {};
        }
        return std::unexpected(err_);
    }

    fiber::common::IoErr err = err_;
    if (waiter_) {
        err = waiter_->err_;
        delete waiter_;
        waiter_ = nullptr;
    }
    if (err == fiber::common::IoErr::None) {
        return {};
    }
    return std::unexpected(err);
}

fiber::common::IoErr TlsStreamFd::HandshakeAwaiter::do_op(TlsStreamFd &stream,
                                                          void *ctx,
                                                          fiber::event::IoEvent &event) {
    (void) ctx;
    return stream.handshake_once(event);
}

TlsStreamFd::ShutdownAwaiter::ShutdownAwaiter(TlsStreamFd &stream) noexcept {
    stream_ = &stream;
    op_ctx_ = this;
    op_ = &ShutdownAwaiter::do_op;
}

TlsStreamFd::ShutdownAwaiter::~ShutdownAwaiter() {
    if (!waiting_) {
        return;
    }
    if (waiter_) {
        FIBER_ASSERT(!stream_->loop_.in_loop());
        waiter_->cancel_wait();
        waiter_ = nullptr;
        return;
    }
    FIBER_ASSERT(stream_->loop_.in_loop());
    stream_->cancel_event(this);
}

bool TlsStreamFd::ShutdownAwaiter::await_suspend(std::coroutine_handle<> handle) {
    coro_ = handle;
    err_ = fiber::common::IoErr::None;
    completed_ = false;
    if (stream_->loop_.in_loop()) {
        auto result = stream_->start_op(this, true);
        if (result.waiting) {
            waiting_ = true;
            return true;
        }
        err_ = result.err;
        completed_ = true;
        return false;
    }
    err_ = fiber::common::IoErr::NotSupported;
    completed_ = true;
    return false;
}

fiber::common::IoResult<void> TlsStreamFd::ShutdownAwaiter::await_resume() noexcept {
    waiting_ = false;
    if (completed_) {
        completed_ = false;
        if (err_ == fiber::common::IoErr::None) {
            return {};
        }
        return std::unexpected(err_);
    }

    fiber::common::IoErr err = err_;
    if (waiter_) {
        err = waiter_->err_;
        delete waiter_;
        waiter_ = nullptr;
    }
    if (err == fiber::common::IoErr::None) {
        return {};
    }
    return std::unexpected(err);
}

fiber::common::IoErr TlsStreamFd::ShutdownAwaiter::do_op(TlsStreamFd &stream,
                                                         void *ctx,
                                                         fiber::event::IoEvent &event) {
    (void) ctx;
    return stream.shutdown_once(event);
}

} // namespace fiber::net::detail
