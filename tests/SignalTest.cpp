#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <future>
#include <initializer_list>
#include <thread>

#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include "async/CoroutinePromiseBase.h"
#include "async/Signal.h"
#include "async/Spawn.h"
#include "event/EventLoopGroup.h"
#include "event/SignalService.h"

namespace {

fiber::async::SignalSet block_signals(std::initializer_list<int> sigs) {
    fiber::async::SignalSet mask;
    for (int sig : sigs) {
        mask.add(sig);
    }
    pthread_sigmask(SIG_BLOCK, &mask.native(), nullptr);
    return mask;
}

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

DetachedTask wait_once(std::promise<fiber::async::SignalInfo> *promise,
                       fiber::event::SignalService *service,
                       int signum) {
    auto info = co_await fiber::async::wait_signal(signum);
    promise->set_value(info);
    service->detach();
    fiber::event::EventLoop::current().stop();
    co_return;
}

DetachedTask wait_and_record(std::atomic<int> *order,
                             std::array<int, 2> *seen,
                             int id,
                             std::promise<void> *notify,
                             fiber::event::SignalService *service,
                             bool stop_on_resume) {
    auto info = co_await fiber::async::wait_signal(SIGUSR1);
    (void) info;
    int idx = order->fetch_add(1, std::memory_order_acq_rel);
    (*seen)[static_cast<std::size_t>(idx)] = id;
    if (notify) {
        notify->set_value();
    }
    if (stop_on_resume) {
        service->detach();
        fiber::event::EventLoop::current().stop();
    }
    co_return;
}

ManualTask wait_and_hit(std::atomic<int> *hits) {
    auto info = co_await fiber::async::wait_signal(SIGUSR1);
    (void) info;
    hits->fetch_add(1, std::memory_order_relaxed);
    co_return;
}

} // namespace

TEST(SignalTest, SingleWaiterReceivesSignal) {
    auto mask = block_signals({SIGUSR1});
    fiber::event::EventLoopGroup group(1);
    fiber::event::SignalService service(group.at(0));
    std::promise<void> attached;
    auto attached_future = attached.get_future();
    std::promise<fiber::async::SignalInfo> promise;
    auto future = promise.get_future();

    group.start(mask);
    fiber::async::spawn(group.at(0), [&]() {
        bool ok = service.attach(mask);
        attached.set_value();
        if (!ok) {
            fiber::event::EventLoop::current().stop();
            return DetachedTask{};
        }
        return wait_once(&promise, &service, SIGUSR1);
    });

    if (attached_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "SignalService did not attach in time";
        return;
    }

    ::kill(getpid(), SIGUSR1);

    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "Signal wait did not resume in time";
        return;
    }

    auto info = future.get();
    EXPECT_EQ(info.signum, SIGUSR1);
    group.join();
}

TEST(SignalTest, PendingBeforeAwait) {
    auto mask = block_signals({SIGUSR1});
    fiber::event::EventLoopGroup group(1);
    fiber::event::SignalService service(group.at(0));
    std::promise<void> attached;
    auto attached_future = attached.get_future();
    std::promise<fiber::async::SignalInfo> promise;
    auto future = promise.get_future();

    group.start(mask);
    fiber::async::spawn(group.at(0), [&]() {
        bool ok = service.attach(mask);
        attached.set_value();
        if (!ok) {
            fiber::event::EventLoop::current().stop();
            return DetachedTask{};
        }
        return DetachedTask{};
    });

    if (attached_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "SignalService did not attach in time";
        return;
    }

    ::kill(getpid(), SIGUSR1);

    fiber::async::spawn(group.at(0), [&]() {
        return wait_once(&promise, &service, SIGUSR1);
    });

    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "Signal wait did not resume in time";
        return;
    }

    auto info = future.get();
    EXPECT_EQ(info.signum, SIGUSR1);
    group.join();
}

TEST(SignalTest, FifoFairness) {
    auto mask = block_signals({SIGUSR1});
    fiber::event::EventLoopGroup group(1);
    fiber::event::SignalService service(group.at(0));
    std::promise<void> attached;
    auto attached_future = attached.get_future();
    std::promise<void> first_ready;
    auto first_future = first_ready.get_future();
    std::promise<void> done;
    auto done_future = done.get_future();
    std::atomic<int> order{0};
    std::array<int, 2> seen{0, 0};

    group.start(mask);
    fiber::async::spawn(group.at(0), [&]() {
        bool ok = service.attach(mask);
        attached.set_value();
        if (!ok) {
            fiber::event::EventLoop::current().stop();
            return DetachedTask{};
        }
        wait_and_record(&order, &seen, 1, &first_ready, &service, false);
        wait_and_record(&order, &seen, 2, &done, &service, true);
        ::kill(getpid(), SIGUSR1);
        return DetachedTask{};
    });

    if (attached_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "SignalService did not attach in time";
        return;
    }

    if (first_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "First waiter did not resume in time";
        return;
    }

    ::kill(getpid(), SIGUSR1);

    if (done_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "Second waiter did not resume in time";
        return;
    }

    EXPECT_EQ(seen[0], 1);
    EXPECT_EQ(seen[1], 2);
    group.join();
}

TEST(SignalTest, CancelOnDestroy) {
    auto mask = block_signals({SIGUSR1});
    fiber::event::EventLoopGroup group(1);
    fiber::event::SignalService service(group.at(0));
    std::promise<void> ready;
    auto ready_future = ready.get_future();
    std::promise<void> stopped;
    auto stopped_future = stopped.get_future();
    std::atomic<int> hits{0};

    group.start(mask);
    fiber::async::spawn(group.at(0), [&]() {
        bool ok = service.attach(mask);
        if (!ok) {
            ready.set_value();
            fiber::event::EventLoop::current().stop();
            return DetachedTask{};
        }
        auto task = wait_and_hit(&hits);
        auto handle = task.release();
        handle.resume();
        handle.destroy();
        ready.set_value();
        return DetachedTask{};
    });

    if (ready_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "Cancellation task did not execute in time";
        return;
    }

    ::kill(getpid(), SIGUSR1);

    fiber::async::spawn(group.at(0), [&]() {
        service.detach();
        fiber::event::EventLoop::current().stop();
        stopped.set_value();
        return DetachedTask{};
    });

    if (stopped_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "Loop did not stop in time";
        return;
    }

    group.join();
    EXPECT_EQ(hits.load(std::memory_order_relaxed), 0);
}
