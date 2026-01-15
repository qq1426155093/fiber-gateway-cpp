#ifndef FIBER_TEST_TEST_HELPERS_H
#define FIBER_TEST_TEST_HELPERS_H

#include <type_traits>
#include <utility>

#include "event/EventLoop.h"

namespace fiber::test {

template <typename F>
struct NotifyTask {
    fiber::event::EventLoop::NotifyEntry entry{};
    F fn;

    explicit NotifyTask(F &&func) : fn(std::forward<F>(func)) {}

    static void run(NotifyTask *task) {
        task->fn();
        delete task;
    }

    static void cancel(NotifyTask *task) {
        delete task;
    }
};

template <typename F>
void post_task(fiber::event::EventLoop &loop, F &&fn) {
    using Task = NotifyTask<std::decay_t<F>>;
    auto *task = new Task(std::forward<F>(fn));
    loop.post<Task, &Task::entry, &Task::run, &Task::cancel>(*task);
}

} // namespace fiber::test

#endif // FIBER_TEST_TEST_HELPERS_H
