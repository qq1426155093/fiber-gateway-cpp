#include "WaitGroup.h"

#include <limits>

#include "../common/Assert.h"

namespace fiber::async {

WaitGroup::Waiter::Waiter(WaitGroup *group, fiber::event::EventLoop *loop, std::coroutine_handle<> handle)
    : group(group),
      loop(loop),
      handle(handle) {
}

void WaitGroup::Waiter::resume() {
    WaiterState expected = WaiterState::Notified;
    if (!state.compare_exchange_strong(expected, WaiterState::Resumed, std::memory_order_acq_rel)) {
        return;
    }
    auto resume_handle = handle;
    handle = {};
    if (resume_handle) {
        resume_handle.resume();
    }
}

void WaitGroup::Waiter::on_run(Waiter *waiter) {
    if (!waiter) {
        return;
    }
    waiter->resume();
    delete waiter;
}

WaitGroup::JoinAwaiter::~JoinAwaiter() {
    if (group_ && waiter_) {
        group_->cancel_waiter(waiter_);
    }
}

bool WaitGroup::JoinAwaiter::await_ready() const noexcept {
    return !group_ || group_->empty();
}

bool WaitGroup::JoinAwaiter::await_suspend(std::coroutine_handle<> handle) {
    if (!group_) {
        return false;
    }
    auto *loop = fiber::event::EventLoop::current_or_null();
    FIBER_ASSERT(loop != nullptr);
    waiter_ = new Waiter(group_, loop, handle);
    if (!group_->enqueue_waiter(waiter_)) {
        delete waiter_;
        waiter_ = nullptr;
        return false;
    }
    return true;
}

void WaitGroup::JoinAwaiter::await_resume() noexcept {
    waiter_ = nullptr;
}

WaitGroup::~WaitGroup() {
    std::lock_guard guard(state_mu_);
    FIBER_ASSERT(count_ == 0);
    FIBER_ASSERT(waiters_head_ == nullptr);
    FIBER_ASSERT(waiters_tail_ == nullptr);
}

void WaitGroup::add(std::uint64_t n) {
    if (n == 0) {
        return;
    }
    std::lock_guard guard(state_mu_);
    FIBER_ASSERT(count_ <= std::numeric_limits<std::uint64_t>::max() - n);
    count_ += n;
}

void WaitGroup::done() {
    Waiter *notify_head = nullptr;
    {
        std::lock_guard guard(state_mu_);
        FIBER_ASSERT(count_ > 0);
        --count_;
        if (count_ != 0) {
            return;
        }
        notify_head = waiters_head_;
        waiters_head_ = nullptr;
        waiters_tail_ = nullptr;

        for (Waiter *waiter = notify_head; waiter; waiter = waiter->next) {
            waiter->queued = false;
            waiter->prev = nullptr;
            waiter->state.store(WaiterState::Notified, std::memory_order_release);
        }
    }

    while (notify_head) {
        Waiter *waiter = notify_head;
        notify_head = waiter->next;
        waiter->next = nullptr;
        post_resume(waiter);
    }
}

bool WaitGroup::empty() const noexcept {
    std::lock_guard guard(state_mu_);
    return count_ == 0;
}

WaitGroup::JoinAwaiter WaitGroup::join() noexcept {
    return JoinAwaiter(*this);
}

bool WaitGroup::enqueue_waiter(Waiter *waiter) {
    FIBER_ASSERT(waiter != nullptr);
    std::lock_guard guard(state_mu_);
    if (count_ == 0) {
        return false;
    }

    waiter->prev = waiters_tail_;
    waiter->next = nullptr;
    if (waiters_tail_) {
        waiters_tail_->next = waiter;
    } else {
        waiters_head_ = waiter;
    }
    waiters_tail_ = waiter;
    waiter->queued = true;
    return true;
}

void WaitGroup::cancel_waiter(Waiter *waiter) {
    if (!waiter) {
        return;
    }

    bool should_delete = false;
    {
        std::lock_guard guard(state_mu_);
        WaiterState state = waiter->state.load(std::memory_order_acquire);
        if (state == WaiterState::Waiting) {
            if (waiter->queued) {
                if (waiter->prev) {
                    waiter->prev->next = waiter->next;
                } else {
                    waiters_head_ = waiter->next;
                }
                if (waiter->next) {
                    waiter->next->prev = waiter->prev;
                } else {
                    waiters_tail_ = waiter->prev;
                }
                waiter->prev = nullptr;
                waiter->next = nullptr;
                waiter->queued = false;
            }
            waiter->state.store(WaiterState::Canceled, std::memory_order_release);
            waiter->handle = {};
            should_delete = true;
        } else if (state == WaiterState::Notified) {
            waiter->state.store(WaiterState::Canceled, std::memory_order_release);
            waiter->handle = {};
        }
    }

    if (should_delete) {
        delete waiter;
    }
}

void WaitGroup::post_resume(Waiter *waiter) {
    FIBER_ASSERT(waiter != nullptr);
    FIBER_ASSERT(waiter->loop != nullptr);
    waiter->loop->post<Waiter, &Waiter::notify_entry, &Waiter::on_run>(*waiter);
}

} // namespace fiber::async
