#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <exception>
#include <future>
#include <thread>

#include "async/CoroutinePromiseBase.h"
#include "async/Spawn.h"
#include "async/Sleep.h"
#include "event/EventLoopGroup.h"

namespace {

using DetachedTask = fiber::async::DetachedTask;

DetachedTask run_sleep(std::promise<std::chrono::steady_clock::duration> *promise,
                       std::chrono::steady_clock::duration delay) {
    auto start = std::chrono::steady_clock::now();
    co_await fiber::async::sleep(delay);
    auto elapsed = std::chrono::steady_clock::now() - start;
    promise->set_value(elapsed);
    fiber::event::EventLoop::current().stop();
    co_return;
}

class ManualTask {
public:
    struct promise_type : fiber::async::CoroutinePromiseBase {
        ManualTask get_return_object() {
            return ManualTask{handle_type::from_promise(*this)};
        }

        std::suspend_always initial_suspend() noexcept {
            return {};
        }

        std::suspend_always final_suspend() noexcept {
            return {};
        }

        void return_void() noexcept {
        }

        void unhandled_exception() {
            std::terminate();
        }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    explicit ManualTask(handle_type handle) : handle_(handle) {
    }

    ManualTask(const ManualTask &) = delete;
    ManualTask &operator=(const ManualTask &) = delete;

    ManualTask(ManualTask &&other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    ManualTask &operator=(ManualTask &&other) noexcept {
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

    ~ManualTask() {
        if (handle_) {
            handle_.destroy();
        }
    }

    handle_type release() noexcept {
        handle_type out = handle_;
        handle_ = nullptr;
        return out;
    }

private:
    handle_type handle_{};
};

ManualTask run_sleep_cancel(std::atomic<int> *hits,
                            std::chrono::steady_clock::duration delay) {
    co_await fiber::async::sleep(delay);
    hits->fetch_add(1, std::memory_order_relaxed);
    co_return;
}

} // namespace

TEST(SleepTest, ResumesAfterDelay) {
    fiber::event::EventLoopGroup group(1);
    std::promise<std::chrono::steady_clock::duration> promise;
    auto future = promise.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&promise]() {
        return run_sleep(&promise, std::chrono::milliseconds(30));
    });

    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "sleep did not resume in time";
        return;
    }

    auto elapsed = future.get();
    EXPECT_GE(elapsed, std::chrono::milliseconds(20));
    group.join();
}

TEST(SleepTest, CancelOnDestroy) {
    fiber::event::EventLoopGroup group(1);
    std::promise<void> ready;
    auto future = ready.get_future();
    std::atomic<int> hits{0};

    group.start();
    fiber::async::spawn(group.at(0), [&ready, &hits]() {
        auto task = run_sleep_cancel(&hits, std::chrono::milliseconds(50));
        auto handle = task.release();
        handle.resume();
        handle.destroy();
        ready.set_value();
        return DetachedTask{};
    });

    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "loop did not execute cancellation in time";
        return;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    group.stop();
    group.join();
    EXPECT_EQ(hits.load(std::memory_order_relaxed), 0);
}
