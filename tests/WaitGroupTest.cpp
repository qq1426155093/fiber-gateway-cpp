#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

#include "async/Sleep.h"
#include "async/Spawn.h"
#include "async/WaitGroup.h"
#include "event/EventLoopGroup.h"

namespace {

using DetachedTask = fiber::async::DetachedTask;
using WaitGroup = fiber::async::WaitGroup;

DetachedTask await_join_and_stop(WaitGroup *wg, std::promise<void> *promise) {
    co_await wg->join();
    promise->set_value();
    fiber::event::EventLoop::current().stop();
    co_return;
}

DetachedTask sleep_then_done(WaitGroup *wg, std::chrono::steady_clock::duration delay) {
    co_await fiber::async::sleep(delay);
    wg->done();
    co_return;
}

DetachedTask record_loop_id(std::promise<std::thread::id> *promise) {
    promise->set_value(std::this_thread::get_id());
    co_return;
}

DetachedTask await_join_record_and_maybe_stop(WaitGroup *wg,
                                              std::promise<std::thread::id> *promise,
                                              std::atomic<int> *resumed,
                                              int stop_when_resumed,
                                              fiber::event::EventLoopGroup *group) {
    co_await wg->join();
    promise->set_value(std::this_thread::get_id());
    if (resumed->fetch_add(1, std::memory_order_acq_rel) + 1 == stop_when_resumed) {
        group->stop();
    }
    co_return;
}

} // namespace

TEST(WaitGroupTest, JoinReturnsImmediatelyWhenEmpty) {
    fiber::event::EventLoopGroup group(1);
    WaitGroup wg;
    std::promise<void> promise;
    auto future = promise.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() {
        return await_join_and_stop(&wg, &promise);
    });

    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "join did not complete in time";
        return;
    }

    future.get();
    group.join();
}

TEST(WaitGroupTest, JoinResumesAfterDone) {
    fiber::event::EventLoopGroup group(1);
    WaitGroup wg;
    std::promise<void> promise;
    auto future = promise.get_future();

    wg.add(1);

    group.start();
    fiber::async::spawn(group.at(0), [&]() {
        return await_join_and_stop(&wg, &promise);
    });
    fiber::async::spawn(group.at(0), [&]() {
        return sleep_then_done(&wg, std::chrono::milliseconds(20));
    });

    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "join waiter did not resume in time";
        return;
    }

    future.get();
    group.join();
}

TEST(WaitGroupTest, WaitersResumeOnOwningLoops) {
    fiber::event::EventLoopGroup group(2);
    WaitGroup wg;
    std::atomic<int> resumed{0};

    std::promise<std::thread::id> loop0_promise;
    std::promise<std::thread::id> loop1_promise;
    auto loop0_future = loop0_promise.get_future();
    auto loop1_future = loop1_promise.get_future();

    std::promise<std::thread::id> waiter0_promise;
    std::promise<std::thread::id> waiter1_promise;
    auto waiter0_future = waiter0_promise.get_future();
    auto waiter1_future = waiter1_promise.get_future();

    wg.add(2);

    group.start();

    fiber::async::spawn(group.at(0), [&]() {
        return record_loop_id(&loop0_promise);
    });
    fiber::async::spawn(group.at(1), [&]() {
        return record_loop_id(&loop1_promise);
    });

    if (loop0_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready ||
        loop1_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "failed to capture loop thread ids in time";
        return;
    }

    fiber::async::spawn(group.at(0), [&]() {
        return await_join_record_and_maybe_stop(&wg, &waiter0_promise, &resumed, 2, &group);
    });
    fiber::async::spawn(group.at(1), [&]() {
        return await_join_record_and_maybe_stop(&wg, &waiter1_promise, &resumed, 2, &group);
    });

    fiber::async::spawn(group.at(0), [&]() {
        return sleep_then_done(&wg, std::chrono::milliseconds(10));
    });
    fiber::async::spawn(group.at(1), [&]() {
        return sleep_then_done(&wg, std::chrono::milliseconds(20));
    });

    if (waiter0_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready ||
        waiter1_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "waiters did not resume in time";
        return;
    }

    EXPECT_EQ(waiter0_future.get(), loop0_future.get());
    EXPECT_EQ(waiter1_future.get(), loop1_future.get());

    group.join();
}

#if GTEST_HAS_DEATH_TEST
TEST(WaitGroupTest, DoneWithoutAddAborts) {
    WaitGroup wg;
    EXPECT_DEATH(wg.done(), "FIBER_ASSERT failed");
}
#endif
