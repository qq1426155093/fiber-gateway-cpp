#ifndef FIBER_HTTP_HTTP_TASK_H
#define FIBER_HTTP_HTTP_TASK_H

#include <concepts>
#include <coroutine>
#include <optional>
#include <utility>

#include "../async/CoroutinePromiseBase.h"
#include "../common/Assert.h"

namespace fiber::http {

class HttpTaskPromiseBase : public fiber::async::CoroutinePromiseBase {
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
class HttpTask {
public:
    struct promise_type : HttpTaskPromiseBase {
        std::optional<T> result_;

        HttpTask get_return_object() noexcept {
            return HttpTask{handle_type::from_promise(*this)};
        }

        void unhandled_exception() {
            FIBER_PANIC("unhandled exception in HttpTask");
        }

        template <typename U>
            requires std::convertible_to<U, T>
        void return_value(U &&value) {
            result_ = std::forward<U>(value);
        }

        T result() {
            if (!result_) {
                FIBER_PANIC("HttpTask missing result");
            }
            return std::move(*result_);
        }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    HttpTask() = default;
    explicit HttpTask(handle_type handle) : handle_(handle) {}

    HttpTask(const HttpTask &) = delete;
    HttpTask &operator=(const HttpTask &) = delete;

    HttpTask(HttpTask &&other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    HttpTask &operator=(HttpTask &&other) noexcept {
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

    ~HttpTask() {
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
class HttpTask<void> {
public:
    struct promise_type : HttpTaskPromiseBase {
        bool completed_ = false;

        HttpTask get_return_object() noexcept {
            return HttpTask{handle_type::from_promise(*this)};
        }

        void unhandled_exception() {
            FIBER_PANIC("unhandled exception in HttpTask");
        }

        void return_void() noexcept {
            completed_ = true;
        }

        void result() const {
            if (!completed_) {
                FIBER_PANIC("HttpTask missing result");
            }
        }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    HttpTask() = default;
    explicit HttpTask(handle_type handle) : handle_(handle) {}

    HttpTask(const HttpTask &) = delete;
    HttpTask &operator=(const HttpTask &) = delete;

    HttpTask(HttpTask &&other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    HttpTask &operator=(HttpTask &&other) noexcept {
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

    ~HttpTask() {
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

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP_TASK_H
