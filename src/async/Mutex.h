#ifndef FIBER_ASYNC_MUTEX_H
#define FIBER_ASYNC_MUTEX_H

#include <atomic>
#include <cstdint>
#include <coroutine>
#include <mutex>
#include <thread>

#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../event/EventLoop.h"

namespace fiber::async {

class Mutex : public common::NonCopyable, public common::NonMovable {
private:
    struct Waiter;
    using WaiterPtr = Waiter *;

public:
    class LockGuard {
    public:
        LockGuard() = default;
        explicit LockGuard(Mutex *mutex) : mutex_(mutex) {
        }

        LockGuard(const LockGuard &) = delete;
        LockGuard &operator=(const LockGuard &) = delete;
        LockGuard(LockGuard &&other) noexcept;
        LockGuard &operator=(LockGuard &&other) noexcept;
        ~LockGuard();

        void unlock();
        [[nodiscard]] bool owns_lock() const noexcept;

    private:
        Mutex *mutex_ = nullptr;
    };

    class LockAwaiter {
    public:
        explicit LockAwaiter(Mutex &mutex) noexcept : mutex_(&mutex) {
        }

        LockAwaiter(const LockAwaiter &) = delete;
        LockAwaiter &operator=(const LockAwaiter &) = delete;
        LockAwaiter(LockAwaiter &&) = delete;
        LockAwaiter &operator=(LockAwaiter &&) = delete;
        ~LockAwaiter();

        bool await_ready() noexcept;
        bool await_suspend(std::coroutine_handle<> handle);
        LockGuard await_resume() noexcept;

    private:
        friend class Mutex;

        Mutex *mutex_ = nullptr;
        WaiterPtr waiter_ = nullptr;
        bool acquired_ = false;
    };

    Mutex() = default;
    ~Mutex() = default;

    [[nodiscard]] LockAwaiter lock() noexcept;
    bool try_lock() noexcept;
    void unlock();
    bool locked() const noexcept;

private:
    enum class WaiterState : std::uint8_t {
        Waiting,
        Notified,
        Resumed,
        Canceled
    };

    struct Waiter {
        explicit Waiter(Mutex *owner,
                        std::coroutine_handle<> handle,
                        fiber::event::EventLoop *loop,
                        std::thread::id thread_id);

        void resume();
        static void on_run(Waiter *waiter);

        Mutex *mutex = nullptr;
        std::coroutine_handle<> handle{};
        fiber::event::EventLoop *loop = nullptr;
        std::thread::id thread{};
        std::atomic<WaiterState> state{WaiterState::Waiting};
        Waiter *prev = nullptr;
        Waiter *next = nullptr;
        bool queued = false;
        fiber::event::EventLoop::NotifyEntry notify_entry{};
    };

    bool enqueue_waiter(WaiterPtr waiter);
    void cancel_waiter(WaiterPtr waiter);
    WaiterPtr select_next_waiter_locked();
    static void post_resume(WaiterPtr waiter);

    mutable std::mutex state_mu_{};
    WaiterPtr waiters_head_ = nullptr;
    WaiterPtr waiters_tail_ = nullptr;
    bool locked_ = false;
    std::thread::id owner_thread_{};
};

} // namespace fiber::async

#endif // FIBER_ASYNC_MUTEX_H
