#include <gtest/gtest.h>

#include <chrono>
#include <coroutine>
#include <future>
#include <string>
#include <unistd.h>

#include "async/CoroutinePromiseBase.h"
#include "async/Spawn.h"
#include "common/IoError.h"
#include "event/EventLoopGroup.h"
#include "net/UnixAddress.h"
#include "net/UnixListener.h"
#include "net/UnixStream.h"

namespace {

class DetachedTask {
public:
    struct promise_type : fiber::async::CoroutinePromiseBase {
        DetachedTask get_return_object() {
            return {};
        }

        std::suspend_never initial_suspend() noexcept {
            return {};
        }

        struct FinalAwaiter {
            bool await_ready() noexcept {
                return false;
            }

            void await_suspend(std::coroutine_handle<promise_type> handle) noexcept {
                handle.destroy();
            }

            void await_resume() noexcept {
            }
        };

        FinalAwaiter final_suspend() noexcept {
            return {};
        }

        void return_void() noexcept {
        }

        void unhandled_exception() {
            std::terminate();
        }
    };
};

std::string make_socket_path() {
    char pattern[] = "/tmp/fiber_unix_stream_test_XXXXXX";
    int fd = ::mkstemp(pattern);
    if (fd < 0) {
        return {};
    }
    ::close(fd);
    ::unlink(pattern);
    return std::string(pattern);
}

DetachedTask accept_once(fiber::event::EventLoop *loop,
                         fiber::net::UnixAddress address,
                         std::promise<fiber::common::IoResult<void>> *ready_promise,
                         std::promise<fiber::common::IoResult<fiber::net::UnixAcceptResult>> *accept_promise) {
    fiber::net::UnixListener listener(*loop);
    fiber::net::UnixListenOptions options{};
    options.unlink_existing = true;

    auto bind_result = listener.bind(address, options);
    if (!bind_result) {
        ready_promise->set_value(std::unexpected(bind_result.error()));
        accept_promise->set_value(std::unexpected(bind_result.error()));
        listener.close();
        fiber::event::EventLoop::current().stop();
        co_return;
    }
    ready_promise->set_value({});

    auto accept_result = co_await listener.accept();
    accept_promise->set_value(accept_result);
    if (accept_result && accept_result->fd >= 0) {
        ::close(accept_result->fd);
    }

    listener.close();
    fiber::event::EventLoop::current().stop();
    co_return;
}

DetachedTask connect_client(fiber::event::EventLoop *loop,
                            fiber::net::UnixAddress address,
                            std::promise<fiber::common::IoResult<void>> *connect_promise) {
    auto infant_result = co_await fiber::net::UnixStream::connect(*loop, address);
    if (!infant_result) {
        connect_promise->set_value(std::unexpected(infant_result.error()));
        fiber::event::EventLoop::current().stop();
        co_return;
    }
    fiber::net::UnixStream stream(std::move(*infant_result));
    stream.close();
    connect_promise->set_value({});
    fiber::event::EventLoop::current().stop();
    co_return;
}

} // namespace

TEST(UnixStreamTest, ConnectsWithAwaiter) {
    fiber::event::EventLoopGroup group(2);
    group.start();

    std::string path = make_socket_path();
    ASSERT_FALSE(path.empty());
    fiber::net::UnixAddress address = fiber::net::UnixAddress::filesystem(path);

    std::promise<fiber::common::IoResult<void>> ready_promise;
    std::promise<fiber::common::IoResult<fiber::net::UnixAcceptResult>> accept_promise;
    std::promise<fiber::common::IoResult<void>> connect_promise;
    auto ready_future = ready_promise.get_future();
    auto accept_future = accept_promise.get_future();
    auto connect_future = connect_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() {
        accept_once(&group.at(0), address, &ready_promise, &accept_promise);
    });

    if (ready_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        ::unlink(path.c_str());
        FAIL() << "server did not bind in time";
        return;
    }
    auto ready_result = ready_future.get();
    ASSERT_TRUE(ready_result);

    fiber::async::spawn(group.at(1), [&]() {
        connect_client(&group.at(1), address, &connect_promise);
    });

    if (connect_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        ::unlink(path.c_str());
        FAIL() << "client did not complete in time";
        return;
    }
    auto connect_result = connect_future.get();
    ASSERT_TRUE(connect_result);

    if (accept_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        ::unlink(path.c_str());
        FAIL() << "server accept did not complete in time";
        return;
    }
    auto accept_result = accept_future.get();
    ASSERT_TRUE(accept_result);
    EXPECT_GE(accept_result->fd, 0);

    group.join();
    ::unlink(path.c_str());
}
