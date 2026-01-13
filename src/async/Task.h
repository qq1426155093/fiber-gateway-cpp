#ifndef FIBER_SCRIPT_ASYNC_TASK_H
#define FIBER_SCRIPT_ASYNC_TASK_H

#include <concepts>
#include <coroutine>
#include <expected>
#include <optional>
#include <string>
#include <utility>

#include "../../async/CoroutinePromiseBase.h"
#include "../../common/Assert.h"

namespace fiber::script::async {

class TaskPromiseBase : public fiber::async::CoroutinePromiseBase {
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
            if (!cont) {
                return;
            }
            cont.resume();
        }

        void await_resume() noexcept {
        }
    };

    FinalAwaiter final_suspend() noexcept {
        return {};
    }

    void set_continuation(std::coroutine_handle<> handle) {
        continuation_ = handle;
    }

    std::coroutine_handle<> continuation() const {
        return continuation_;
    }

private:
    std::coroutine_handle<> continuation_ = nullptr;
};

struct TaskError {
    std::string message;
};

template <typename T>
class Task {
public:
    struct promise_type : TaskPromiseBase {
        std::optional<std::expected<T, TaskError>> result_;

        Task get_return_object() {
            return Task{handle_type::from_promise(*this)};
        }

        void unhandled_exception() {
            FIBER_PANIC("unhandled exception in Task");
        }

        template <typename U>
            requires std::convertible_to<U, T>
        void return_value(U &&value) {
            result_ = std::expected<T, TaskError>(std::in_place, std::forward<U>(value));
        }

        void return_value(std::expected<T, TaskError> value) {
            result_ = std::move(value);
        }

        std::expected<T, TaskError> result() {
            if (!result_) {
                return std::unexpected(TaskError{"no result"});
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

    bool valid() const {
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

        std::expected<T, TaskError> await_resume() {
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
        std::optional<std::expected<void, TaskError>> result_;

        Task get_return_object() {
            return Task{handle_type::from_promise(*this)};
        }

        void unhandled_exception() {
            FIBER_PANIC("unhandled exception in Task");
        }

        void return_value(std::expected<void, TaskError> value) {
            result_ = std::move(value);
        }

        std::expected<void, TaskError> result() {
            if (!result_) {
                return std::unexpected(TaskError{"no result"});
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

    bool valid() const {
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

        std::expected<void, TaskError> await_resume() {
            return handle.promise().result();
        }
    };

    Awaiter operator co_await() {
        return Awaiter{handle_};
    }

private:
    handle_type handle_ = nullptr;
};

} // namespace fiber::script::async

#endif // FIBER_SCRIPT_ASYNC_TASK_H
