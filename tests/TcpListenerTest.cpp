#include <gtest/gtest.h>

#include <chrono>
#include <cerrno>
#include <coroutine>
#include <future>
#include <thread>
#include <sys/socket.h>
#include <unistd.h>

#include "async/Spawn.h"
#include "common/IoError.h"
#include "event/EventLoopGroup.h"
#include "net/SocketAddress.h"
#include "net/TcpListener.h"

namespace {

using DetachedTask = fiber::async::DetachedTask;

DetachedTask accept_once(fiber::event::EventLoop *loop,
                         fiber::net::TcpListener **out_listener,
                         std::promise<uint16_t> *port_promise,
                         std::promise<fiber::common::IoResult<fiber::net::AcceptResult>> *accept_promise) {
    fiber::net::TcpListener *listener = new fiber::net::TcpListener(*loop);
    *out_listener = listener;

    fiber::net::ListenOptions options{};
    fiber::net::SocketAddress addr(fiber::net::IpAddress::loopback_v4(), 0);

    auto bind_result = listener->bind(addr, options);
    if (!bind_result) {
        port_promise->set_value(0);
        accept_promise->set_value(std::unexpected(bind_result.error()));
        fiber::event::EventLoop::current().stop();
        co_return;
    }

    sockaddr_storage bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(listener->fd(), reinterpret_cast<sockaddr *>(&bound), &len) != 0) {
        port_promise->set_value(0);
        accept_promise->set_value(std::unexpected(fiber::common::io_err_from_errno(errno)));
        fiber::event::EventLoop::current().stop();
        co_return;
    }
    fiber::net::SocketAddress local;
    if (!fiber::net::SocketAddress::from_sockaddr(reinterpret_cast<sockaddr *>(&bound), len, local)) {
        port_promise->set_value(0);
        accept_promise->set_value(std::unexpected(fiber::common::IoErr::NotSupported));
        fiber::event::EventLoop::current().stop();
        co_return;
    }
    port_promise->set_value(local.port());

    auto result = co_await listener->accept();
    accept_promise->set_value(result);
    if (result && result->fd >= 0) {
        ::close(result->fd);
    }
    listener->close();
    delete listener;
    *out_listener = nullptr;
    fiber::event::EventLoop::current().stop();
    co_return;
}

DetachedTask expect_busy(fiber::net::TcpListener *listener,
                         std::promise<fiber::common::IoErr> *error_promise) {
    auto result = co_await listener->accept();
    if (result) {
        error_promise->set_value(fiber::common::IoErr::None);
        if (result->fd >= 0) {
            ::close(result->fd);
        }
    } else {
        error_promise->set_value(result.error());
    }
    co_return;
}

} // namespace

TEST(TcpListenerTest, AcceptsConnection) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<uint16_t> port_promise;
    std::promise<fiber::common::IoResult<fiber::net::AcceptResult>> accept_promise;
    auto port_future = port_promise.get_future();
    auto accept_future = accept_promise.get_future();
    fiber::net::TcpListener *listener = nullptr;

    fiber::async::spawn(group.at(0), [&]() {
        return accept_once(&group.at(0), &listener, &port_promise, &accept_promise);
    });

    uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    int client = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    ASSERT_GE(client, 0);
    fiber::net::SocketAddress target(fiber::net::IpAddress::loopback_v4(), port);
    sockaddr_storage target_storage{};
    socklen_t target_len = 0;
    ASSERT_TRUE(target.to_sockaddr(target_storage, target_len));
    ASSERT_EQ(::connect(client, reinterpret_cast<sockaddr *>(&target_storage), target_len), 0);
    ::close(client);

    if (accept_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "accept did not complete in time";
        return;
    }
    auto result = accept_future.get();
    ASSERT_TRUE(result);
    EXPECT_GE(result->fd, 0);
    group.join();
}

TEST(TcpListenerTest, OwnerMismatchReturnsBusy) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<uint16_t> port_promise;
    std::promise<fiber::common::IoResult<fiber::net::AcceptResult>> accept_promise;
    std::promise<fiber::common::IoErr> busy_promise;
    auto port_future = port_promise.get_future();
    auto accept_future = accept_promise.get_future();
    auto busy_future = busy_promise.get_future();
    fiber::net::TcpListener *listener = nullptr;

    fiber::async::spawn(group.at(0), [&]() {
        return accept_once(&group.at(0), &listener, &port_promise, &accept_promise);
    });

    uint16_t port = port_future.get();
    ASSERT_NE(port, 0);
    ASSERT_NE(listener, nullptr);

    fiber::async::spawn(group.at(0), [&]() {
        return expect_busy(listener, &busy_promise);
    });

    int client = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    ASSERT_GE(client, 0);
    fiber::net::SocketAddress target(fiber::net::IpAddress::loopback_v4(), port);
    sockaddr_storage target_storage{};
    socklen_t target_len = 0;
    ASSERT_TRUE(target.to_sockaddr(target_storage, target_len));
    ASSERT_EQ(::connect(client, reinterpret_cast<sockaddr *>(&target_storage), target_len), 0);
    ::close(client);

    if (busy_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "owner mismatch did not return in time";
        return;
    }
    EXPECT_EQ(busy_future.get(), fiber::common::IoErr::Busy);

    if (accept_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "accept did not complete in time";
        return;
    }
    auto result = accept_future.get();
    ASSERT_TRUE(result);
    EXPECT_GE(result->fd, 0);
    group.join();
}
