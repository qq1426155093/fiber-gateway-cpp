#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <exception>
#include <future>
#include <thread>

#include "async/CoroutinePromiseBase.h"
#include "async/Mutex.h"
#include "async/Spawn.h"
#include "async/Sleep.h"
#include "event/EventLoopGroup.h"

namespace {

using DetachedTask = fiber::async::DetachedTask;

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

DetachedTask hold_lock(fiber::async::Mutex *mutex,
                       std::atomic<int> *state) {
    auto guard = co_await mutex->lock();
    state->store(1, std::memory_order_relaxed);
    co_await fiber::async::sleep(std::chrono::milliseconds(30));
    state->store(2, std::memory_order_relaxed);
    co_return;
}

DetachedTask wait_lock(fiber::async::Mutex *mutex,
                       std::atomic<int> *state,
                       std::promise<int> *promise) {
    auto guard = co_await mutex->lock();
    promise->set_value(state->load(std::memory_order_relaxed));
    fiber::event::EventLoop::current().stop();
    co_return;
}

DetachedTask hold_and_finish(fiber::async::Mutex *mutex,
                             std::promise<void> *done) {
    {
        auto guard = co_await mutex->lock();
        co_await fiber::async::sleep(std::chrono::milliseconds(30));
    }
    done->set_value();
    fiber::event::EventLoop::current().stop();
    co_return;
}

ManualTask wait_then_hit(fiber::async::Mutex *mutex,
                         std::atomic<int> *hits) {
    auto guard = co_await mutex->lock();
    hits->fetch_add(1, std::memory_order_relaxed);
    co_return;
}

DetachedTask hold_lock_with_signal(fiber::async::Mutex *mutex,
                                   std::promise<void> *locked,
                                   std::chrono::steady_clock::duration delay) {
    auto guard = co_await mutex->lock();
    locked->set_value();
    co_await fiber::async::sleep(delay);
    co_return;
}

DetachedTask wait_lock_thread(fiber::async::Mutex *mutex,
                              std::promise<std::thread::id> *resumed) {
    auto guard = co_await mutex->lock();
    resumed->set_value(std::this_thread::get_id());
    co_return;
}

} // namespace

TEST(MutexTest, ResumesWaiterAfterUnlock) {
    fiber::event::EventLoopGroup group(1);
    fiber::async::Mutex mutex;
    std::atomic<int> state{0};
    std::promise<int> promise;
    auto future = promise.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&mutex, &state, &promise]() {
        hold_lock(&mutex, &state);
        wait_lock(&mutex, &state, &promise);
        return DetachedTask{};
    });

    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "mutex waiter did not resume in time";
        return;
    }

    EXPECT_EQ(future.get(), 2);
    group.join();
}

TEST(MutexTest, CancelWaiterDoesNotResume) {
    fiber::event::EventLoopGroup group(1);
    fiber::async::Mutex mutex;
    std::atomic<int> hits{0};
    std::promise<void> done;
    auto future = done.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&mutex, &hits, &done]() {
        hold_and_finish(&mutex, &done);

        auto waiter = wait_then_hit(&mutex, &hits);
        auto handle = waiter.release();
        handle.resume();
        handle.destroy();
        return DetachedTask{};
    });

    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "mutex holder did not finish in time";
        return;
    }

    EXPECT_EQ(hits.load(std::memory_order_relaxed), 0);
    group.join();
}

TEST(MutexTest, ResumesOnWaiterLoopThread) {
    fiber::event::EventLoopGroup group(2);
    fiber::async::Mutex mutex;
    std::promise<std::thread::id> loop0_id;
    std::promise<std::thread::id> loop1_id;
    auto loop0_future = loop0_id.get_future();
    auto loop1_future = loop1_id.get_future();
    std::promise<void> locked;
    auto locked_future = locked.get_future();
    std::promise<std::thread::id> resumed;
    auto resumed_future = resumed.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&loop0_id]() {
        loop0_id.set_value(std::this_thread::get_id());
        return DetachedTask{};
    });
    fiber::async::spawn(group.at(1), [&loop1_id]() {
        loop1_id.set_value(std::this_thread::get_id());
        return DetachedTask{};
    });

    if (loop0_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready ||
        loop1_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "loop threads did not report ids in time";
        return;
    }

    fiber::async::spawn(group.at(0), [&mutex, &locked]() {
        return hold_lock_with_signal(&mutex, &locked, std::chrono::milliseconds(50));
    });

    if (locked_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "mutex was not locked in time";
        return;
    }

    fiber::async::spawn(group.at(1), [&mutex, &resumed]() {
        return wait_lock_thread(&mutex, &resumed);
    });

    if (resumed_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "waiter did not resume in time";
        return;
    }

    EXPECT_EQ(resumed_future.get(), loop1_future.get());
    group.stop();
    group.join();
}
