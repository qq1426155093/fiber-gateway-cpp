#include <gtest/gtest.h>

#include <chrono>
#include <cerrno>
#include <coroutine>
#include <future>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>

#include "async/Spawn.h"
#include "common/IoError.h"
#include "event/EventLoopGroup.h"
#include "net/SocketAddress.h"
#include "net/UdpSocket.h"

namespace {

using DetachedTask = fiber::async::DetachedTask;

fiber::common::IoResult<fiber::net::SocketAddress> get_bound_address(int fd) {
    sockaddr_storage bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&bound), &len) != 0) {
        return std::unexpected(fiber::common::io_err_from_errno(errno));
    }
    fiber::net::SocketAddress out;
    if (!fiber::net::SocketAddress::from_sockaddr(reinterpret_cast<const sockaddr *>(&bound), len, out)) {
        return std::unexpected(fiber::common::IoErr::NotSupported);
    }
    return out;
}

DetachedTask server_echo(fiber::event::EventLoop *loop,
                         std::promise<uint16_t> *port_promise,
                         std::promise<fiber::common::IoErr> *done_promise) {
    auto *server = new fiber::net::UdpSocket(*loop);
    fiber::net::UdpBindOptions options{};
    fiber::net::SocketAddress addr(fiber::net::IpAddress::loopback_v4(), 0);

    auto bind_result = server->bind(addr, options);
    if (!bind_result) {
        port_promise->set_value(0);
        done_promise->set_value(bind_result.error());
        delete server;
        co_return;
    }

    auto local_result = get_bound_address(server->fd());
    if (!local_result) {
        port_promise->set_value(0);
        done_promise->set_value(local_result.error());
        server->close();
        delete server;
        co_return;
    }
    port_promise->set_value(local_result->port());

    char buf[64] = {};
    auto recv_result = co_await server->recv_from(buf, sizeof(buf));
    if (!recv_result) {
        done_promise->set_value(recv_result.error());
        server->close();
        delete server;
        co_return;
    }

    constexpr std::string_view reply = "pong";
    auto send_result = co_await server->send_to(reply.data(), reply.size(), recv_result->peer);
    if (!send_result) {
        done_promise->set_value(send_result.error());
    } else {
        done_promise->set_value(fiber::common::IoErr::None);
    }
    server->close();
    delete server;
    co_return;
}

DetachedTask client_echo(fiber::event::EventLoop *loop,
                         uint16_t port,
                         std::promise<fiber::common::IoErr> *done_promise) {
    auto *client = new fiber::net::UdpSocket(*loop);
    fiber::net::UdpBindOptions options{};
    fiber::net::SocketAddress local(fiber::net::IpAddress::loopback_v4(), 0);

    auto bind_result = client->bind(local, options);
    if (!bind_result) {
        done_promise->set_value(bind_result.error());
        delete client;
        co_return;
    }

    constexpr std::string_view message = "ping";
    fiber::net::SocketAddress server_addr(fiber::net::IpAddress::loopback_v4(), port);
    auto send_result = co_await client->send_to(message.data(), message.size(), server_addr);
    if (!send_result) {
        done_promise->set_value(send_result.error());
        client->close();
        delete client;
        co_return;
    }

    char buf[64] = {};
    auto recv_result = co_await client->recv_from(buf, sizeof(buf));
    if (!recv_result) {
        done_promise->set_value(recv_result.error());
        client->close();
        delete client;
        co_return;
    }

    std::string_view received(buf, recv_result->size);
    if (received != "pong") {
        done_promise->set_value(fiber::common::IoErr::Unknown);
    } else {
        done_promise->set_value(fiber::common::IoErr::None);
    }
    client->close();
    delete client;
    co_return;
}

#ifdef SO_REUSEPORT
DetachedTask bind_reuse_port(fiber::event::EventLoop *loop,
                             std::promise<fiber::common::IoErr> *done_promise) {
    auto *first = new fiber::net::UdpSocket(*loop);
    auto *second = new fiber::net::UdpSocket(*loop);
    fiber::net::UdpBindOptions options{};
    options.reuse_port = true;

    fiber::net::SocketAddress addr(fiber::net::IpAddress::loopback_v4(), 0);
    auto bind_first = first->bind(addr, options);
    if (!bind_first) {
        done_promise->set_value(bind_first.error());
        delete first;
        delete second;
        co_return;
    }

    auto local_result = get_bound_address(first->fd());
    if (!local_result) {
        done_promise->set_value(local_result.error());
        first->close();
        delete first;
        delete second;
        co_return;
    }

    fiber::net::SocketAddress second_addr(fiber::net::IpAddress::loopback_v4(), local_result->port());
    auto bind_second = second->bind(second_addr, options);
    if (!bind_second) {
        done_promise->set_value(bind_second.error());
    } else {
        done_promise->set_value(fiber::common::IoErr::None);
    }

    first->close();
    second->close();
    delete first;
    delete second;
    co_return;
}
#endif

} // namespace

TEST(UdpSocketTest, RecvFromSendToRoundTrip) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<uint16_t> port_promise;
    std::promise<fiber::common::IoErr> server_promise;
    std::promise<fiber::common::IoErr> client_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();
    auto client_future = client_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() {
        return server_echo(&group.at(0), &port_promise, &server_promise);
    });

    uint16_t port = port_future.get();
    if (port == 0) {
        group.stop();
        group.join();
        FAIL() << "bind did not produce a valid port";
        return;
    }

    fiber::async::spawn(group.at(0), [&]() {
        return client_echo(&group.at(0), port, &client_promise);
    });

    if (server_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "server did not complete in time";
        return;
    }
    if (client_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "client did not complete in time";
        return;
    }

    EXPECT_EQ(server_future.get(), fiber::common::IoErr::None);
    EXPECT_EQ(client_future.get(), fiber::common::IoErr::None);

    group.stop();
    group.join();
}

#ifdef SO_REUSEPORT
TEST(UdpSocketTest, ReusePortAllowsSecondBind) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<fiber::common::IoErr> result_promise;
    auto result_future = result_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() {
        return bind_reuse_port(&group.at(0), &result_promise);
    });

    if (result_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "reuse_port bind did not complete in time";
        return;
    }

    fiber::common::IoErr result = result_future.get();
    if (result == fiber::common::IoErr::NotSupported) {
        group.stop();
        group.join();
        GTEST_SKIP() << "SO_REUSEPORT not supported";
    }

    EXPECT_EQ(result, fiber::common::IoErr::None);
    group.stop();
    group.join();
}
#endif
