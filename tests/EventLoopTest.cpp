#include <gtest/gtest.h>

#include <chrono>
#include <future>

#include "async/CoroutineFramePool.h"
#include "async/Spawn.h"
#include "event/EventLoopGroup.h"

TEST(EventLoopTest, FramePoolInstalledOnLoopThread) {
    fiber::event::EventLoopGroup group(1);
    std::promise<bool> promise;
    auto future = promise.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&promise]() {
        auto &loop = fiber::event::EventLoop::current();
        auto *pool = fiber::async::CoroutineFramePool::current();
        bool ok = pool != nullptr && pool == &loop.frame_pool();
        promise.set_value(ok);
        loop.stop();
        return fiber::async::DetachedTask{};
    });

    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "EventLoop thread did not process task in time";
        return;
    }

    EXPECT_TRUE(future.get());
    group.join();
}
