#include "Signal.h"

#include "../common/Assert.h"
#include "../event/SignalService.h"

namespace fiber::async {

SignalSet::SignalSet() {
    sigemptyset(&set_);
}

SignalSet &SignalSet::add(int signum) {
    sigaddset(&set_, signum);
    return *this;
}

SignalSet &SignalSet::remove(int signum) {
    sigdelset(&set_, signum);
    return *this;
}

bool SignalSet::contains(int signum) const noexcept {
    return sigismember(&set_, signum) == 1;
}

detail::SignalWaiter::SignalWaiter(SignalAwaiter *owner,
                                   int signum,
                                   fiber::event::EventLoop *loop,
                                   std::coroutine_handle<> handle)
    : loop(loop),
      handle(handle),
      signum(signum),
      owner(owner) {
}

void detail::SignalWaiter::resume() {
    SignalWaiterState expected = SignalWaiterState::Notified;
    if (!state.compare_exchange_strong(expected, SignalWaiterState::Resumed, std::memory_order_acq_rel)) {
        return;
    }
    if (owner) {
        owner->info_ = info;
        owner->waiting_ = false;
        owner->waiter_ = nullptr;
    }
    auto resume_handle = handle;
    handle = {};
    if (resume_handle) {
        resume_handle.resume();
    }
}

void detail::SignalWaiter::on_run(SignalWaiter *waiter) {
    if (!waiter) {
        return;
    }
    waiter->resume();
    delete waiter;
}

SignalAwaiter::SignalAwaiter(int signum) noexcept : signum_(signum) {
}

SignalAwaiter::~SignalAwaiter() {
    if (waiting_ && service_) {
        service_->cancel_waiter(waiter_);
    }
}

bool SignalAwaiter::await_ready() noexcept {
    auto *loop = fiber::event::EventLoop::current_or_null();
    FIBER_ASSERT(loop != nullptr);
    service_ = fiber::event::SignalService::current_or_null();
    FIBER_ASSERT(service_ != nullptr);
    if (service_->try_pop_pending(signum_, info_)) {
        return true;
    }
    return false;
}

bool SignalAwaiter::await_suspend(std::coroutine_handle<> handle) {
    if (!service_) {
        service_ = fiber::event::SignalService::current_or_null();
    }
    FIBER_ASSERT(service_ != nullptr);
    auto *loop = fiber::event::EventLoop::current_or_null();
    FIBER_ASSERT(loop != nullptr);

    if (service_->try_pop_pending(signum_, info_)) {
        return false;
    }

    waiting_ = true;
    waiter_ = new detail::SignalWaiter(this, signum_, loop, handle);
    service_->enqueue_waiter(signum_, waiter_);
    return true;
}

SignalInfo SignalAwaiter::await_resume() noexcept {
    waiting_ = false;
    return info_;
}

SignalAwaiter wait_signal(int signum) {
    return SignalAwaiter(signum);
}

} // namespace fiber::async
