#ifndef FIBER_ASYNC_TASK_H
#define FIBER_ASYNC_TASK_H

#include <concepts>
#include <coroutine>
#include <optional>
#include <utility>

#include "../common/Assert.h"
#include "CoroutinePromiseBase.h"

namespace fiber::async {

class TaskPromiseBase : public CoroutinePromiseBase {
public:
    std::suspend_always initial_suspend() noexcept {
        return {};
    }

    struct FinalAwaiter {
        bool await_ready() noexcept {
            return false;
        }

        template <typename Promise>
        void await_suspend(std::coroutine_handle<Promise> handle) noexcept {
            Promise &promise = handle.promise();
            std::coroutine_handle<> cont = promise.continuation();
            if (cont) {
                cont.resume();
            }
        }

        void await_resume() noexcept {
        }
    };

    FinalAwaiter final_suspend() noexcept {
        return {};
    }

    void set_continuation(std::coroutine_handle<> handle) noexcept {
        continuation_ = handle;
    }

    std::coroutine_handle<> continuation() const noexcept {
        return continuation_;
    }

private:
    std::coroutine_handle<> continuation_ = nullptr;
};

template <typename T>
class Task {
public:
    struct promise_type : TaskPromiseBase {
        std::optional<T> result_;

        Task get_return_object() noexcept {
            return Task{handle_type::from_promise(*this)};
        }

        void unhandled_exception() {
            FIBER_PANIC("unhandled exception in Task");
        }

        template <typename U>
            requires std::convertible_to<U, T>
        void return_value(U &&value) {
            result_ = std::forward<U>(value);
        }

        T result() {
            if (!result_) {
                FIBER_PANIC("Task missing result");
            }
            return std::move(*result_);
        }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    Task() = default;
    explicit Task(handle_type handle) : handle_(handle) {
    }

    Task(const Task &) = delete;
    Task &operator=(const Task &) = delete;

    Task(Task &&other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    Task &operator=(Task &&other) noexcept {
        if (this == &other) {
            return *this;
        }
        if (handle_) {
            handle_.destroy();
        }
        handle_ = other.handle_;
        other.handle_ = nullptr;
        return *this;
    }

    ~Task() {
        if (handle_) {
            handle_.destroy();
        }
    }

    bool valid() const noexcept {
        return static_cast<bool>(handle_);
    }

    struct Awaiter {
        handle_type handle;

        bool await_ready() const noexcept {
            return !handle || handle.done();
        }

        void await_suspend(std::coroutine_handle<> cont) {
            handle.promise().set_continuation(cont);
            handle.resume();
        }

        T await_resume() {
            return handle.promise().result();
        }
    };

    Awaiter operator co_await() {
        return Awaiter{handle_};
    }

private:
    handle_type handle_ = nullptr;
};

template <>
class Task<void> {
public:
    struct promise_type : TaskPromiseBase {
        bool completed_ = false;

        Task get_return_object() noexcept {
            return Task{handle_type::from_promise(*this)};
        }

        void unhandled_exception() {
            FIBER_PANIC("unhandled exception in Task");
        }

        void return_void() noexcept {
            completed_ = true;
        }

        void result() const {
            if (!completed_) {
                FIBER_PANIC("Task missing result");
            }
        }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    Task() = default;
    explicit Task(handle_type handle) : handle_(handle) {
    }

    Task(const Task &) = delete;
    Task &operator=(const Task &) = delete;

    Task(Task &&other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    Task &operator=(Task &&other) noexcept {
        if (this == &other) {
            return *this;
        }
        if (handle_) {
            handle_.destroy();
        }
        handle_ = other.handle_;
        other.handle_ = nullptr;
        return *this;
    }

    ~Task() {
        if (handle_) {
            handle_.destroy();
        }
    }

    bool valid() const noexcept {
        return static_cast<bool>(handle_);
    }

    struct Awaiter {
        handle_type handle;

        bool await_ready() const noexcept {
            return !handle || handle.done();
        }

        void await_suspend(std::coroutine_handle<> cont) {
            handle.promise().set_continuation(cont);
            handle.resume();
        }

        void await_resume() {
            handle.promise().result();
        }
    };

    Awaiter operator co_await() {
        return Awaiter{handle_};
    }

private:
    handle_type handle_ = nullptr;
};

} // namespace fiber::async

#endif // FIBER_ASYNC_TASK_H
