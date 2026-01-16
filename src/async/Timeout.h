#ifndef FIBER_ASYNC_TIMEOUT_H
#define FIBER_ASYNC_TIMEOUT_H

#include <chrono>
#include <concepts>
#include <coroutine>
#include <expected>
#include <functional>
#include <type_traits>
#include <utility>

#include "../common/IoError.h"
#include "../event/EventLoop.h"

namespace fiber::async {

namespace detail {

template<typename T>
concept HasMemberCoAwait = requires(T value) { value.operator co_await(); };

template<typename T>
concept HasFreeCoAwait = requires(T value) { operator co_await(value); };

template<typename T>
decltype(auto) get_awaiter(T &&value) {
    if constexpr (HasMemberCoAwait<T>) {
        return std::forward<T>(value).operator co_await();
    } else if constexpr (HasFreeCoAwait<T>) {
        return operator co_await(std::forward<T>(value));
    } else {
        return std::forward<T>(value);
    }
}

template<typename T>
struct IsIoResult : std::false_type {};

template<typename T>
struct IsIoResult<std::expected<T, fiber::common::IoErr>> : std::true_type {};

template<typename T>
inline constexpr bool kIsIoResult = IsIoResult<std::remove_cvref_t<T>>::value;

template<typename T>
using AwaiterType = std::remove_cvref_t<decltype(get_awaiter(std::declval<T>()))>;

template<typename T>
using TimeoutResult =
        std::conditional_t<kIsIoResult<T>, std::remove_cvref_t<T>, fiber::common::IoResult<std::remove_cvref_t<T>>>;

template<typename T>
concept Awaiter = requires(T awaiter, std::coroutine_handle<> handle) {
    { awaiter.await_ready() } -> std::convertible_to<bool>;
    awaiter.await_suspend(handle);
    awaiter.await_resume();
};

template<typename T>
concept Awaitable = Awaiter<AwaiterType<T>>;

} // namespace detail

template<typename Awaitable>
class TimeoutAwaiter {
public:
    using InnerAwaiter = detail::AwaiterType<Awaitable>;
    using InnerResult = decltype(std::declval<InnerAwaiter &>().await_resume());
    using ReturnResult = detail::TimeoutResult<InnerResult>;

    template<typename A>
    TimeoutAwaiter(A &&awaitable, std::chrono::steady_clock::duration timeout) :
        awaiter_(detail::get_awaiter(std::forward<A>(awaitable))), timeout_(timeout), has_deadline_(false) {}

    template<typename A>
    TimeoutAwaiter(A &&awaitable, std::chrono::steady_clock::time_point deadline) :
        awaiter_(detail::get_awaiter(std::forward<A>(awaitable))), deadline_(deadline), has_deadline_(true) {}

    TimeoutAwaiter(const TimeoutAwaiter &) = delete;
    TimeoutAwaiter &operator=(const TimeoutAwaiter &) = delete;
    TimeoutAwaiter(TimeoutAwaiter &&) = delete;
    TimeoutAwaiter &operator=(TimeoutAwaiter &&) = delete;
    ~TimeoutAwaiter() {
        if (loop_) {
            loop_->cancel<TimeoutAwaiter, &TimeoutAwaiter::timer_entry_>(*this);
        }
    }

    bool await_ready() {
        if (awaiter_.await_ready()) {
            return true;
        }
        if (expired_now()) {
            timed_out_ = true;
            return true;
        }
        return false;
    }

    bool await_suspend(std::coroutine_handle<> handle) {
        handle_ = handle;
        loop_ = &event::EventLoop::current();
        using SuspendReturn = decltype(awaiter_.await_suspend(handle));
        if constexpr (std::is_void_v<SuspendReturn>) {
            awaiter_.await_suspend(handle);
        } else if constexpr (std::is_same_v<SuspendReturn, bool>) {
            if (!awaiter_.await_suspend(handle)) {
                return false;
            }
        } else {
            static_assert(std::is_void_v<SuspendReturn> || std::is_same_v<SuspendReturn, bool>,
                          "await_suspend must return void or bool");
        }
        auto when = deadline_for_timer();
        loop_->post_at<TimeoutAwaiter, &TimeoutAwaiter::timer_entry_, &TimeoutAwaiter::on_timeout>(when, *this);
        return true;
    }

    ReturnResult await_resume() {
        if (timed_out_) {
            return std::unexpected(fiber::common::IoErr::TimedOut);
        }
        if (loop_) {
            loop_->cancel<TimeoutAwaiter, &TimeoutAwaiter::timer_entry_>(*this);
        }
        if constexpr (detail::kIsIoResult<InnerResult>) {
            return awaiter_.await_resume();
        } else if constexpr (std::is_void_v<std::remove_cvref_t<InnerResult>>) {
            awaiter_.await_resume();
            return {};
        } else {
            return awaiter_.await_resume();
        }
    }

private:
    static void on_timeout(TimeoutAwaiter *awaiter) {
        if (!awaiter) {
            return;
        }
        awaiter->timed_out_ = true;
        awaiter->handle_.resume();
    }

    [[nodiscard]] bool expired_now() const {
        if (has_deadline_) {
            return deadline_ <= std::chrono::steady_clock::now();
        }
        return timeout_ <= std::chrono::steady_clock::duration::zero();
    }

    [[nodiscard]] std::chrono::steady_clock::time_point deadline_for_timer() const {
        if (has_deadline_) {
            return deadline_;
        }
        return std::chrono::steady_clock::now() + timeout_;
    }

    InnerAwaiter awaiter_;
    std::chrono::steady_clock::duration timeout_{};
    std::chrono::steady_clock::time_point deadline_{};
    bool has_deadline_ = false;
    fiber::event::EventLoop *loop_ = nullptr;
    std::coroutine_handle<> handle_{};
    fiber::event::EventLoop::TimerEntry timer_entry_{};
    bool timed_out_ = false;
};

template<typename Awaitable>
    requires(!std::is_lvalue_reference_v<Awaitable> && detail::Awaitable<Awaitable>)
auto timeout_for(Awaitable &&awaitable, std::chrono::steady_clock::duration timeout) {
    // Require rvalues to avoid copying non-movable awaiters.
    return TimeoutAwaiter<Awaitable>(std::forward<Awaitable>(awaitable), timeout);
}

template<typename Factory>
    requires(std::invocable<Factory &> && detail::Awaitable<std::invoke_result_t<Factory &>>)
auto timeout_for(Factory &&factory, std::chrono::steady_clock::duration timeout) {
    using Awaitable = std::invoke_result_t<Factory &>;
    static_assert(!std::is_lvalue_reference_v<Awaitable>, "timeout_for factory must return prvalue awaitable");
    return TimeoutAwaiter<Awaitable>(std::invoke(factory), timeout);
}

} // namespace fiber::async

#endif // FIBER_ASYNC_TIMEOUT_H
