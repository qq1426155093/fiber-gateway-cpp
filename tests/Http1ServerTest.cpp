#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <cerrno>
#include <coroutine>
#include <future>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

#include "async/Spawn.h"
#include "async/Task.h"
#include "common/IoError.h"
#include "event/EventLoopGroup.h"
#include "http/Http1Server.h"
#include "net/SocketAddress.h"

namespace {

using fiber::async::DetachedTask;

std::string recv_all(int fd) {
    std::string out;
    std::array<char, 4096> buf{};
    for (;;) {
        ssize_t rc = ::recv(fd, buf.data(), buf.size(), 0);
        if (rc == 0) {
            break;
        }
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        out.append(buf.data(), static_cast<size_t>(rc));
    }
    return out;
}

fiber::common::IoResult<uint16_t> resolve_port(int fd) {
    sockaddr_storage bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&bound), &len) != 0) {
        return std::unexpected(fiber::common::io_err_from_errno(errno));
    }
    fiber::net::SocketAddress local;
    if (!fiber::net::SocketAddress::from_sockaddr(reinterpret_cast<sockaddr *>(&bound), len, local)) {
        return std::unexpected(fiber::common::IoErr::NotSupported);
    }
    return local.port();
}

DetachedTask start_server(fiber::event::EventLoop *loop,
                          fiber::http::HttpHandler handler,
                          std::promise<uint16_t> *port_promise,
                          std::promise<fiber::http::Http1Server *> *server_promise) {
    auto *server = new fiber::http::Http1Server(*loop, std::move(handler));
    fiber::net::ListenOptions options{};
    fiber::net::SocketAddress addr(fiber::net::IpAddress::loopback_v4(), 8080);
    auto bind_result = server->bind(addr, options);
    if (!bind_result) {
        port_promise->set_value(0);
        server_promise->set_value(nullptr);
        delete server;
        co_return;
    }
    auto port = resolve_port(server->fd());
    port_promise->set_value(port ? *port : 0);
    server_promise->set_value(server);
    fiber::async::spawn(*loop, [server]() {
        return server->serve();
    });
    co_return;
}

DetachedTask stop_server(fiber::event::EventLoop *loop, fiber::http::Http1Server *server) {
    if (server) {
        server->close();
    }
    loop->stop();
    co_return;
}

} // namespace

TEST(Http1ServerTest, BasicGet) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<uint16_t> port_promise;
    std::promise<fiber::http::Http1Server *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() {
        auto handler = [](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
            exchange.set_response_header("Content-Type", "text/plain");
            exchange.set_response_content_length(2);
            auto header_result = co_await exchange.send_response_header(200);
            if (!header_result) {
                co_return;
            }
            co_await exchange.write_body("ok", 2, true);
            co_return;
        };
        return start_server(&group.at(0), handler, &port_promise, &server_promise);
    });

    uint16_t port = port_future.get();
    ASSERT_NE(port, 0);
    auto *server = server_future.get();
    ASSERT_NE(server, nullptr);

    int client = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    ASSERT_GE(client, 0);
    fiber::net::SocketAddress target(fiber::net::IpAddress::loopback_v4(), port);
    sockaddr_storage storage{};
    socklen_t len = 0;
    ASSERT_TRUE(target.to_sockaddr(storage, len));
    ASSERT_EQ(::connect(client, reinterpret_cast<sockaddr *>(&storage), len), 0);

    const char *request = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, request, std::strlen(request), 0), static_cast<ssize_t>(std::strlen(request)));

    std::string response = recv_all(client);
    ::close(client);

    EXPECT_NE(response.find("200"), std::string::npos);
    EXPECT_NE(response.find("ok"), std::string::npos);

    fiber::async::spawn(group.at(0), [&]() {
        return stop_server(&group.at(0), server);
    });
    group.join();
    delete server;
}

TEST(Http1ServerTest, ChunkedPost) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<uint16_t> port_promise;
    std::promise<fiber::http::Http1Server *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() {
        auto handler = [](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
            std::array<char, 64> buffer{};
            std::string body;
            for (;;) {
                auto read_result = co_await exchange.read_body(buffer.data(), buffer.size());
                if (!read_result) {
                    exchange.set_response_close();
                    exchange.set_response_content_length(0);
                    co_await exchange.send_response_header(400, "Bad Request");
                    co_return;
                }
                if (read_result->size > 0) {
                    body.append(buffer.data(), read_result->size);
                }
                if (read_result->end) {
                    break;
                }
            }
            exchange.set_response_header("Content-Type", "text/plain");
            exchange.set_response_content_length(body.size());
            auto header_result = co_await exchange.send_response_header(200);
            if (!header_result) {
                co_return;
            }
            if (!body.empty()) {
                co_await exchange.write_body(body.data(), body.size(), true);
            } else {
                co_await exchange.write_body(nullptr, 0, true);
            }
            co_return;
        };
        return start_server(&group.at(0), handler, &port_promise, &server_promise);
    });

    uint16_t port = port_future.get();
    ASSERT_NE(port, 0);
    auto *server = server_future.get();
    ASSERT_NE(server, nullptr);

    int client = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    ASSERT_GE(client, 0);
    fiber::net::SocketAddress target(fiber::net::IpAddress::loopback_v4(), port);
    sockaddr_storage storage{};
    socklen_t len = 0;
    ASSERT_TRUE(target.to_sockaddr(storage, len));
    ASSERT_EQ(::connect(client, reinterpret_cast<sockaddr *>(&storage), len), 0);

    const char *request =
        "POST /echo HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: close\r\n"
        "\r\n"
        "4\r\nWiki\r\n"
        "5\r\npedia\r\n"
        "0\r\n\r\n";
    ASSERT_EQ(::send(client, request, std::strlen(request), 0), static_cast<ssize_t>(std::strlen(request)));

    std::string response = recv_all(client);
    ::close(client);

    EXPECT_NE(response.find("200"), std::string::npos);
    EXPECT_NE(response.find("Wikipedia"), std::string::npos);

    fiber::async::spawn(group.at(0), [&]() {
        return stop_server(&group.at(0), server);
    });
    group.join();
    delete server;
}
