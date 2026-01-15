#include "Mutex.h"

#include <iterator>
#include <utility>

#include "../common/Assert.h"
#include "../event/EventLoop.h"

namespace fiber::async {

Mutex::Waiter::Waiter(Mutex *owner,
                      std::coroutine_handle<> handle,
                      fiber::event::EventLoop *loop,
                      std::thread::id thread_id)
    : mutex(owner),
      handle(handle),
      loop(loop),
      thread(thread_id) {
}

void Mutex::Waiter::resume() {
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

void Mutex::Waiter::on_run(Waiter *waiter) {
    if (!waiter) {
        return;
    }
    waiter->resume();
    delete waiter;
}

Mutex::LockGuard::LockGuard(LockGuard &&other) noexcept : mutex_(other.mutex_) {
    other.mutex_ = nullptr;
}

Mutex::LockGuard &Mutex::LockGuard::operator=(LockGuard &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    if (mutex_) {
        mutex_->unlock();
    }
    mutex_ = other.mutex_;
    other.mutex_ = nullptr;
    return *this;
}

Mutex::LockGuard::~LockGuard() {
    if (mutex_) {
        mutex_->unlock();
    }
}

void Mutex::LockGuard::unlock() {
    if (!mutex_) {
        return;
    }
    mutex_->unlock();
    mutex_ = nullptr;
}

bool Mutex::LockGuard::owns_lock() const noexcept {
    return mutex_ != nullptr;
}

Mutex::LockAwaiter::~LockAwaiter() {
    if (mutex_ && waiter_) {
        mutex_->cancel_waiter(waiter_);
    }
}

bool Mutex::LockAwaiter::await_ready() noexcept {
    if (!mutex_) {
        return true;
    }
    if (mutex_->try_lock()) {
        acquired_ = true;
        return true;
    }
    return false;
}

bool Mutex::LockAwaiter::await_suspend(std::coroutine_handle<> handle) {
    if (!mutex_ || acquired_) {
        return false;
    }
    auto *loop = fiber::event::EventLoop::current_or_null();
    FIBER_ASSERT(loop != nullptr);
    waiter_ = new Waiter(mutex_, handle, loop, std::this_thread::get_id());
    if (!mutex_->enqueue_waiter(waiter_)) {
        acquired_ = true;
        delete waiter_;
        waiter_ = nullptr;
        return false;
    }
    return true;
}

Mutex::LockGuard Mutex::LockAwaiter::await_resume() noexcept {
    waiter_ = nullptr;
    return mutex_ ? LockGuard(mutex_) : LockGuard();
}

Mutex::LockAwaiter Mutex::lock() noexcept {
    return LockAwaiter(*this);
}

bool Mutex::try_lock() noexcept {
    std::lock_guard guard(state_mu_);
    if (locked_) {
        return false;
    }
    FIBER_ASSERT(waiters_head_ == nullptr);
    FIBER_ASSERT(waiters_tail_ == nullptr);
    locked_ = true;
    owner_thread_ = std::this_thread::get_id();
    return true;
}

void Mutex::unlock() {
    WaiterPtr next = nullptr;
    {
        std::lock_guard guard(state_mu_);
        FIBER_ASSERT(locked_);
        FIBER_ASSERT(owner_thread_ == std::this_thread::get_id());
        next = select_next_waiter_locked();
    }
    if (next) {
        post_resume(next);
    }
}

bool Mutex::locked() const noexcept {
    std::lock_guard guard(state_mu_);
    return locked_;
}

bool Mutex::enqueue_waiter(WaiterPtr waiter) {
    std::lock_guard guard(state_mu_);
    if (!locked_) {
        locked_ = true;
        owner_thread_ = waiter->thread;
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

void Mutex::cancel_waiter(WaiterPtr waiter) {
    if (!waiter) {
        return;
    }
    WaiterPtr next = nullptr;
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
        }
        if (state == WaiterState::Notified) {
            FIBER_ASSERT(owner_thread_ == std::this_thread::get_id());
            waiter->state.store(WaiterState::Canceled, std::memory_order_release);
            waiter->handle = {};
            next = select_next_waiter_locked();
        }
    }
    if (next) {
        post_resume(next);
    }
    if (should_delete) {
        delete waiter;
    }
}

Mutex::WaiterPtr Mutex::select_next_waiter_locked() {
    while (waiters_head_) {
        WaiterPtr next = waiters_head_;
        waiters_head_ = next->next;
        if (waiters_head_) {
            waiters_head_->prev = nullptr;
        } else {
            waiters_tail_ = nullptr;
        }
        next->prev = nullptr;
        next->next = nullptr;
        next->queued = false;
        WaiterState state = next->state.load(std::memory_order_acquire);
        FIBER_ASSERT(state != WaiterState::Resumed);
        if (state != WaiterState::Waiting) {
            continue;
        }
        next->state.store(WaiterState::Notified, std::memory_order_release);
        owner_thread_ = next->thread;
        return next;
    }
    locked_ = false;
    owner_thread_ = {};
    return {};
}

void Mutex::post_resume(WaiterPtr waiter) {
    FIBER_ASSERT(waiter && waiter->loop);
    waiter->loop->post<Waiter, &Waiter::notify_entry, &Waiter::on_run>(*waiter);
}

} // namespace fiber::async
