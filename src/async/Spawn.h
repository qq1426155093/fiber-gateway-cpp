#ifndef FIBER_ASYNC_SPAWN_H
#define FIBER_ASYNC_SPAWN_H

#include <concepts>
#include <coroutine>
#include <exception>
#include <functional>
#include <type_traits>
#include <utility>

#include "../common/Assert.h"
#include "../event/EventLoop.h"
#include "CoroutinePromiseBase.h"

namespace fiber::async {

class DetachedTask {
public:
    struct promise_type : CoroutinePromiseBase {
        DetachedTask get_return_object() noexcept { return {}; }

        std::suspend_never initial_suspend() noexcept { return {}; }

        std::suspend_never final_suspend() noexcept { return {}; }

        void return_void() noexcept {}

        void unhandled_exception() { FIBER_PANIC("error in spawn"); }
    };
};

template<typename H>
concept CoroutineHandleLike = requires(H handle) {
    { handle.resume() } -> std::same_as<void>;
    { handle.done() } -> std::same_as<bool>;
    { handle.destroy() } -> std::same_as<void>;
    { static_cast<bool>(handle) } -> std::same_as<bool>;
};

template<typename F>
concept SpawnFactory =
        std::invocable<F &> && (CoroutineHandleLike<std::invoke_result_t<F &>> ||
                                std::same_as<std::remove_cvref_t<std::invoke_result_t<F &>>, DetachedTask>);

namespace detail {

template<typename F>
struct SpawnTask {
    fiber::event::EventLoop::NotifyEntry entry{};
    F factory;

    explicit SpawnTask(F &&fn) : factory(std::forward<F>(fn)) {}

    static void run(SpawnTask *task) {
        if (!task) {
            return;
        }
        try {
            task->invoke();
        } catch (...) {
            delete task;
            std::terminate();
        }
        delete task;
    }

private:
    void invoke() {
        if constexpr (CoroutineHandleLike<std::invoke_result_t<F &>>) {
            auto handle = std::invoke(factory);
            if (handle && !handle.done()) {
                handle.resume();
            }
        } else {
            std::invoke(factory);
        }
    }
};

} // namespace detail

template<typename F>
    requires SpawnFactory<F>
void spawn(fiber::event::EventLoop &loop, F &&factory) {
    using Task = detail::SpawnTask<std::decay_t<F>>;
    auto *task = new Task(std::forward<F>(factory));
    loop.post<Task, &Task::entry, &Task::run>(*task);
}

template<typename F>
    requires SpawnFactory<F>
void spawn(F &&factory) {
    auto *loop = fiber::event::EventLoop::current_or_null();
    FIBER_ASSERT(loop != nullptr);
    spawn(*loop, std::forward<F>(factory));
}

} // namespace fiber::async

#endif // FIBER_ASYNC_SPAWN_H
