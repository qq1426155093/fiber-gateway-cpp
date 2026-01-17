#ifndef FIBER_ASYNC_SLEEP_H
#define FIBER_ASYNC_SLEEP_H

#include <chrono>
#include <coroutine>

#include "../common/Assert.h"
#include "../event/EventLoop.h"

namespace fiber::async {

    class SleepAwaiter {
public:
    explicit SleepAwaiter(std::chrono::steady_clock::duration delay);
    SleepAwaiter(const SleepAwaiter &) = delete;
    SleepAwaiter &operator=(const SleepAwaiter &) = delete;
    SleepAwaiter(SleepAwaiter &&other) noexcept : delay_(other.delay_) {
        FIBER_ASSERT(!other.armed_);
        timer_.owner = this;
        other.timer_.owner = nullptr;
        other.loop_ = nullptr;
        other.handle_ = {};
        other.armed_ = false;
    }
    SleepAwaiter &operator=(SleepAwaiter &&) = delete;
    ~SleepAwaiter();

    bool await_ready() const noexcept;
    void await_suspend(std::coroutine_handle<> handle);
    void await_resume() const noexcept {
    }

private:
    struct SleepTimer final {
        fiber::event::EventLoop::TimerEntry entry{};
        SleepAwaiter *owner = nullptr;

        static void on_timer(SleepTimer *timer);
    };

    void fire();

    std::chrono::steady_clock::duration delay_{};
    fiber::event::EventLoop *loop_ = nullptr;
    SleepTimer timer_{};
    std::coroutine_handle<> handle_{};
    bool armed_ = false;
};

SleepAwaiter sleep(std::chrono::steady_clock::duration delay);

} // namespace fiber::async

#endif // FIBER_ASYNC_SLEEP_H
