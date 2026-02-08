#ifndef FIBER_ASYNC_WAIT_GROUP_H
#define FIBER_ASYNC_WAIT_GROUP_H

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <mutex>

#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../event/EventLoop.h"

namespace fiber::async {

class WaitGroup : public common::NonCopyable, public common::NonMovable {
private:
    enum class WaiterState : std::uint8_t {
        Waiting,
        Notified,
        Resumed,
        Canceled
    };

    struct Waiter {
        Waiter(WaitGroup *group, fiber::event::EventLoop *loop, std::coroutine_handle<> handle);

        WaitGroup *group = nullptr;
        fiber::event::EventLoop *loop = nullptr;
        std::coroutine_handle<> handle{};
        std::atomic<WaiterState> state{WaiterState::Waiting};
        Waiter *prev = nullptr;
        Waiter *next = nullptr;
        bool queued = false;
        fiber::event::EventLoop::NotifyEntry notify_entry{};

        void resume();
        static void on_run(Waiter *waiter);
    };

public:
    class JoinAwaiter {
    public:
        explicit JoinAwaiter(WaitGroup &group) noexcept : group_(&group) {
        }

        JoinAwaiter(const JoinAwaiter &) = delete;
        JoinAwaiter &operator=(const JoinAwaiter &) = delete;
        JoinAwaiter(JoinAwaiter &&) = delete;
        JoinAwaiter &operator=(JoinAwaiter &&) = delete;
        ~JoinAwaiter();

        bool await_ready() const noexcept;
        bool await_suspend(std::coroutine_handle<> handle);
        void await_resume() noexcept;

    private:
        WaitGroup *group_ = nullptr;
        Waiter *waiter_ = nullptr;
    };

    WaitGroup() = default;
    ~WaitGroup();

    void add(std::uint64_t n = 1);
    void done();
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] JoinAwaiter join() noexcept;

private:
    bool enqueue_waiter(Waiter *waiter);
    void cancel_waiter(Waiter *waiter);
    static void post_resume(Waiter *waiter);

    mutable std::mutex state_mu_{};
    std::uint64_t count_ = 0;
    Waiter *waiters_head_ = nullptr;
    Waiter *waiters_tail_ = nullptr;
};

} // namespace fiber::async

#endif // FIBER_ASYNC_WAIT_GROUP_H
