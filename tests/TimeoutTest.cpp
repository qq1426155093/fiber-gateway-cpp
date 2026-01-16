#include <gtest/gtest.h>

#include <chrono>
#include <future>

#include "async/CoroutinePromiseBase.h"
#include "async/Sleep.h"
#include "async/Spawn.h"
#include "async/Timeout.h"
#include "common/IoError.h"
#include "event/EventLoopGroup.h"

namespace {

using DetachedTask = fiber::async::DetachedTask;

DetachedTask run_timeout_factory(std::promise<fiber::common::IoErr> *promise, std::chrono::steady_clock::duration delay,
                                 std::chrono::steady_clock::duration timeout) {
    auto result = co_await fiber::async::timeout_for([delay] { return fiber::async::sleep(delay); }, timeout);
    if (result) {
        promise->set_value(fiber::common::IoErr::None);
    } else {
        promise->set_value(result.error());
    }
    fiber::event::EventLoop::current().stop();
    co_return;
}

} // namespace

TEST(TimeoutTest, CompletesBeforeTimeout) {
    fiber::event::EventLoopGroup group(1);
    std::promise<fiber::common::IoErr> promise;
    auto future = promise.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&promise]() {
        return run_timeout_factory(&promise, std::chrono::milliseconds(10), std::chrono::milliseconds(200));
    });

    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "await did not resume in time";
        return;
    }

    EXPECT_EQ(future.get(), fiber::common::IoErr::None);
    group.join();
}
