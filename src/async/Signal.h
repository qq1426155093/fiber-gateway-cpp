#ifndef FIBER_ASYNC_SIGNAL_H
#define FIBER_ASYNC_SIGNAL_H

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <signal.h>

#include "../event/EventLoop.h"

namespace fiber::event {

class SignalService;

} // namespace fiber::event

namespace fiber::async {

struct SignalInfo {
    int signum{};
    int code{};
    pid_t pid{};
    uid_t uid{};
    int status{};
    int errno_{};
    std::intptr_t value{};
};

class SignalSet {
public:
    SignalSet();

    SignalSet &add(int signum);
    SignalSet &remove(int signum);
    bool contains(int signum) const noexcept;
    const sigset_t &native() const noexcept {
        return set_;
    }

private:
    sigset_t set_{};
};

class SignalAwaiter;

namespace detail {

enum class SignalWaiterState : std::uint8_t {
    Waiting,
    Notified,
    Resumed,
    Canceled
};

struct SignalWaiter {
    explicit SignalWaiter(SignalAwaiter *owner,
                          int signum,
                          fiber::event::EventLoop *loop,
                          std::coroutine_handle<> handle);

    SignalInfo info{};
    fiber::event::EventLoop *loop = nullptr;
    std::coroutine_handle<> handle{};
    std::atomic<SignalWaiterState> state{SignalWaiterState::Waiting};
    SignalWaiter *prev = nullptr;
    SignalWaiter *next = nullptr;
    bool queued = false;
    int signum = 0;
    fiber::event::EventLoop::NotifyEntry notify_entry{};
    SignalAwaiter *owner = nullptr;

    void resume();
    static void on_run(SignalWaiter *waiter);
};

} // namespace detail

class SignalAwaiter {
public:
    explicit SignalAwaiter(int signum) noexcept;
    SignalAwaiter(const SignalAwaiter &) = delete;
    SignalAwaiter &operator=(const SignalAwaiter &) = delete;
    SignalAwaiter(SignalAwaiter &&) = delete;
    SignalAwaiter &operator=(SignalAwaiter &&) = delete;
    ~SignalAwaiter();

    bool await_ready() noexcept;
    bool await_suspend(std::coroutine_handle<> handle);
    SignalInfo await_resume() noexcept;

private:
    friend struct detail::SignalWaiter;

    int signum_{};
    detail::SignalWaiter *waiter_ = nullptr;
    fiber::event::SignalService *service_ = nullptr;
    SignalInfo info_{};
    bool waiting_ = false;
};

SignalAwaiter wait_signal(int signum);

} // namespace fiber::async

#endif // FIBER_ASYNC_SIGNAL_H
