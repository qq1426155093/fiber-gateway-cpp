#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <coroutine>
#include <cstring>
#include <future>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "async/Sleep.h"
#include "async/Spawn.h"
#include "async/Task.h"
#include "common/IoError.h"
#include "common/mem/IoBuf.h"
#include "event/EventLoopGroup.h"
#include "http/Http1Server.h"
#include "net/SocketAddress.h"

namespace {

using fiber::async::DetachedTask;
using namespace std::chrono_literals;

std::string chain_to_string(fiber::mem::IoBufChain chain) {
    std::string out;
    while (auto *front = chain.front()) {
        if (front->readable() == 0) {
            chain.drop_empty_front();
            continue;
        }
        out.append(reinterpret_cast<const char *>(front->readable_data()), front->readable());
        chain.consume_and_compact(front->readable());
    }
    return out;
}

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

std::string recv_http_response(int fd) {
    std::string out;
    std::array<char, 4096> buf{};
    size_t header_end = std::string::npos;
    size_t content_length = 0;
    for (;;) {
        if (header_end != std::string::npos && out.size() >= header_end + content_length) {
            return out.substr(0, header_end + content_length);
        }
        ssize_t rc = ::recv(fd, buf.data(), buf.size(), 0);
        if (rc <= 0) {
            return out;
        }
        out.append(buf.data(), static_cast<size_t>(rc));
        if (header_end == std::string::npos) {
            size_t pos = out.find("\r\n\r\n");
            if (pos == std::string::npos) {
                continue;
            }
            header_end = pos + 4;
            size_t cl_pos = out.find("Content-Length:");
            if (cl_pos != std::string::npos) {
                cl_pos += sizeof("Content-Length:") - 1;
                while (cl_pos < pos && out[cl_pos] == ' ') {
                    ++cl_pos;
                }
                size_t cl_end = out.find("\r\n", cl_pos);
                if (cl_end != std::string::npos) {
                    content_length = static_cast<size_t>(std::stoul(out.substr(cl_pos, cl_end - cl_pos)));
                }
            }
        }
    }
}

int connect_client(uint16_t port) {
    int client = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (client < 0) {
        return -1;
    }
    timeval tv{};
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    fiber::net::SocketAddress target(fiber::net::IpAddress::loopback_v4(), port);
    sockaddr_storage storage{};
    socklen_t len = 0;
    if (!target.to_sockaddr(storage, len) || ::connect(client, reinterpret_cast<sockaddr *>(&storage), len) != 0) {
        ::close(client);
        return -1;
    }
    return client;
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

DetachedTask start_server(fiber::event::EventLoop *loop, fiber::http::HttpHandler handler,
                          fiber::event::EventLoopGroup *worker_group, std::promise<uint16_t> *port_promise,
                          std::promise<fiber::http::Http1Server *> *server_promise) {
    auto *server = new fiber::http::Http1Server(*loop, std::move(handler), {}, worker_group);
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
    fiber::async::spawn(*loop, [server]() { return server->serve(); });
    co_return;
}

DetachedTask stop_server(fiber::event::EventLoop *loop, fiber::http::Http1Server *server) {
    if (server) {
        co_await server->shutdown_and_wait();
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
            co_await exchange.write_body(reinterpret_cast<const uint8_t *>("ok"), 2, true);
            co_return;
        };
        return start_server(&group.at(0), handler, nullptr, &port_promise, &server_promise);
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

    fiber::async::spawn(group.at(0), [&]() { return stop_server(&group.at(0), server); });
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
            std::string body;
            for (;;) {
                auto read_result = co_await exchange.read_body(64);
                if (!read_result) {
                    exchange.set_response_close();
                    exchange.set_response_content_length(0);
                    co_await exchange.send_response_header(400, "Bad Request");
                    co_return;
                }
                if (read_result->data_chain.readable_bytes() > 0) {
                    body.append(chain_to_string(std::move(read_result->data_chain)));
                }
                if (read_result->last) {
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
                co_await exchange.write_body(reinterpret_cast<const uint8_t *>(body.data()), body.size(), true);
            } else {
                co_await exchange.write_body(nullptr, 0, true);
            }
            co_return;
        };
        return start_server(&group.at(0), handler, nullptr, &port_promise, &server_promise);
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

    const char *request = "POST /echo HTTP/1.1\r\n"
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

    fiber::async::spawn(group.at(0), [&]() { return stop_server(&group.at(0), server); });
    group.join();
    delete server;
}

TEST(Http1ServerTest, ChunkedPostTrailersAreAvailableAfterBody) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<uint16_t> port_promise;
    std::promise<fiber::http::Http1Server *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() {
        auto handler = [](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
            std::string body;
            for (;;) {
                auto read_result = co_await exchange.read_body(64);
                if (!read_result) {
                    exchange.set_response_close();
                    exchange.set_response_content_length(0);
                    co_await exchange.send_response_header(400, "Bad Request");
                    co_return;
                }
                if (read_result->data_chain.readable_bytes() > 0) {
                    body.append(chain_to_string(std::move(read_result->data_chain)));
                }
                if (read_result->last) {
                    break;
                }
            }

            std::string response = body;
            response.push_back('|');
            response.append(exchange.request_trailers().get("digest"));

            exchange.set_response_content_length(response.size());
            auto header_result = co_await exchange.send_response_header(200);
            if (!header_result) {
                co_return;
            }
            co_await exchange.write_body(reinterpret_cast<const uint8_t *>(response.data()), response.size(), true);
            co_return;
        };
        return start_server(&group.at(0), handler, nullptr, &port_promise, &server_promise);
    });

    uint16_t port = port_future.get();
    ASSERT_NE(port, 0);
    auto *server = server_future.get();
    ASSERT_NE(server, nullptr);

    int client = connect_client(port);
    ASSERT_GE(client, 0);

    const char *request = "POST /trailers HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Transfer-Encoding: chunked\r\n"
                          "Connection: close\r\n"
                          "\r\n"
                          "4\r\nWiki\r\n"
                          "5\r\npedia\r\n"
                          "0\r\n"
                          "Digest: sha-256=xyz\r\n"
                          "\r\n";
    ASSERT_EQ(::send(client, request, std::strlen(request), 0), static_cast<ssize_t>(std::strlen(request)));

    std::string response = recv_all(client);
    ::close(client);

    EXPECT_NE(response.find("200"), std::string::npos);
    EXPECT_NE(response.find("Wikipedia|sha-256=xyz"), std::string::npos);

    fiber::async::spawn(group.at(0), [&]() { return stop_server(&group.at(0), server); });
    group.join();
    delete server;
}

TEST(Http1ServerTest, ChunkedPostWaitsForCompleteTrailersBeforeLastChunk) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<uint16_t> port_promise;
    std::promise<fiber::http::Http1Server *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() {
        auto handler = [](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
            std::string body;
            for (;;) {
                auto read_result = co_await exchange.read_body(64);
                if (!read_result) {
                    exchange.set_response_close();
                    exchange.set_response_content_length(0);
                    co_await exchange.send_response_header(400, "Bad Request");
                    co_return;
                }
                if (read_result->data_chain.readable_bytes() > 0) {
                    body.append(chain_to_string(std::move(read_result->data_chain)));
                }
                if (read_result->last) {
                    break;
                }
            }

            std::string response = body;
            response.push_back('|');
            response.append(exchange.request_trailers().get("digest"));

            exchange.set_response_content_length(response.size());
            auto header_result = co_await exchange.send_response_header(200);
            if (!header_result) {
                co_return;
            }
            co_await exchange.write_body(reinterpret_cast<const uint8_t *>(response.data()), response.size(), true);
            co_return;
        };
        return start_server(&group.at(0), handler, nullptr, &port_promise, &server_promise);
    });

    uint16_t port = port_future.get();
    ASSERT_NE(port, 0);
    auto *server = server_future.get();
    ASSERT_NE(server, nullptr);

    int client = connect_client(port);
    ASSERT_GE(client, 0);

    const char *part1 = "POST /trailers HTTP/1.1\r\n"
                        "Host: localhost\r\n"
                        "Transfer-Encoding: chunked\r\n"
                        "Connection: close\r\n"
                        "\r\n"
                        "4\r\nWiki\r\n"
                        "5\r\npedia\r\n"
                        "0\r\n"
                        "Digest: sha-25";
    ASSERT_EQ(::send(client, part1, std::strlen(part1), 0), static_cast<ssize_t>(std::strlen(part1)));

    std::thread sender([client]() {
        std::this_thread::sleep_for(50ms);
        const char *part2 = "6=xyz\r\n\r\n";
        EXPECT_EQ(::send(client, part2, std::strlen(part2), 0), static_cast<ssize_t>(std::strlen(part2)));
    });

    std::string response = recv_all(client);
    sender.join();
    ::close(client);

    EXPECT_NE(response.find("200"), std::string::npos);
    EXPECT_NE(response.find("Wikipedia|sha-256=xyz"), std::string::npos);

    fiber::async::spawn(group.at(0), [&]() { return stop_server(&group.at(0), server); });
    group.join();
    delete server;
}

TEST(Http1ServerTest, InvalidChunkedPostReturnsBadRequest) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<uint16_t> port_promise;
    std::promise<fiber::http::Http1Server *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() {
        auto handler = [](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
            for (;;) {
                auto read_result = co_await exchange.read_body(64);
                if (!read_result) {
                    exchange.set_response_close();
                    exchange.set_response_content_length(0);
                    co_await exchange.send_response_header(400, "Bad Request");
                    co_return;
                }
                if (read_result->last) {
                    break;
                }
            }
            exchange.set_response_content_length(0);
            co_await exchange.send_response_header(204, "No Content");
            co_await exchange.write_body(nullptr, 0, true);
            co_return;
        };
        return start_server(&group.at(0), handler, nullptr, &port_promise, &server_promise);
    });

    uint16_t port = port_future.get();
    ASSERT_NE(port, 0);
    auto *server = server_future.get();
    ASSERT_NE(server, nullptr);

    int client = connect_client(port);
    ASSERT_GE(client, 0);

    const char *request = "POST /echo HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Transfer-Encoding: chunked\r\n"
                          "Connection: close\r\n"
                          "\r\n"
                          "+4\r\nWiki\r\n"
                          "0\r\n\r\n";
    ASSERT_EQ(::send(client, request, std::strlen(request), 0), static_cast<ssize_t>(std::strlen(request)));

    std::string response = recv_all(client);
    ::close(client);

    EXPECT_NE(response.find("400 Bad Request"), std::string::npos);

    fiber::async::spawn(group.at(0), [&]() { return stop_server(&group.at(0), server); });
    group.join();
    delete server;
}

TEST(Http1ServerTest, ChunkedResponseCanSendTrailers) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<uint16_t> port_promise;
    std::promise<fiber::http::Http1Server *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() {
        auto handler = [](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
            exchange.set_response_chunked();
            exchange.set_response_header("Trailer", "digest, x-md5");
            auto header_result = co_await exchange.send_response_header(200);
            if (!header_result) {
                co_return;
            }

            auto body_result = co_await exchange.write_body(reinterpret_cast<const uint8_t *>("hello"), 5, false);
            if (!body_result) {
                co_return;
            }

            exchange.set_response_trailer("digest", "sha-256=xyz");
            exchange.set_response_trailer("x-md5", "abc123");
            co_await exchange.finish_response();
            co_return;
        };
        return start_server(&group.at(0), handler, nullptr, &port_promise, &server_promise);
    });

    uint16_t port = port_future.get();
    ASSERT_NE(port, 0);
    auto *server = server_future.get();
    ASSERT_NE(server, nullptr);

    int client = connect_client(port);
    ASSERT_GE(client, 0);

    const char *request = "GET /trailers HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Connection: close\r\n"
                          "\r\n";
    ASSERT_EQ(::send(client, request, std::strlen(request), 0), static_cast<ssize_t>(std::strlen(request)));

    std::string response = recv_all(client);
    ::close(client);

    EXPECT_NE(response.find("Transfer-Encoding: chunked"), std::string::npos);
    EXPECT_NE(response.find("Trailer: digest, x-md5"), std::string::npos);
    EXPECT_NE(response.find("\r\n5\r\nhello\r\n0\r\n"), std::string::npos);
    EXPECT_NE(response.find("digest: sha-256=xyz\r\n"), std::string::npos);
    EXPECT_NE(response.find("x-md5: abc123\r\n"), std::string::npos);

    fiber::async::spawn(group.at(0), [&]() { return stop_server(&group.at(0), server); });
    group.join();
    delete server;
}

TEST(Http1ServerTest, KeepAliveReuse) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<uint16_t> port_promise;
    std::promise<fiber::http::Http1Server *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();
    std::atomic<int> request_count{0};

    fiber::async::spawn(group.at(0), [&]() {
        auto handler = [&](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
            int count = request_count.fetch_add(1, std::memory_order_relaxed) + 1;
            std::string body = std::to_string(count);
            exchange.set_response_content_length(body.size());
            auto header_result = co_await exchange.send_response_header(200);
            if (!header_result) {
                co_return;
            }
            co_await exchange.write_body(reinterpret_cast<const uint8_t *>(body.data()), body.size(), true);
            co_return;
        };
        return start_server(&group.at(0), handler, nullptr, &port_promise, &server_promise);
    });

    uint16_t port = port_future.get();
    ASSERT_NE(port, 0);
    auto *server = server_future.get();
    ASSERT_NE(server, nullptr);

    int client = connect_client(port);
    ASSERT_GE(client, 0);

    const char *request1 = "GET /one HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ASSERT_EQ(::send(client, request1, std::strlen(request1), 0), static_cast<ssize_t>(std::strlen(request1)));
    std::string response1 = recv_http_response(client);
    EXPECT_NE(response1.find("\r\n\r\n1"), std::string::npos);

    const char *request2 = "GET /two HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, request2, std::strlen(request2), 0), static_cast<ssize_t>(std::strlen(request2)));
    std::string response2 = recv_all(client);
    ::close(client);

    EXPECT_NE(response2.find("\r\n\r\n2"), std::string::npos);
    EXPECT_EQ(request_count.load(std::memory_order_relaxed), 2);

    fiber::async::spawn(group.at(0), [&]() { return stop_server(&group.at(0), server); });
    group.join();
    delete server;
}

TEST(Http1ServerTest, ChunkedKeepAlivePipelinedNextRequest) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<uint16_t> port_promise;
    std::promise<fiber::http::Http1Server *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();
    std::atomic<int> request_count{0};

    fiber::async::spawn(group.at(0), [&]() {
        auto handler = [&](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
            int count = request_count.fetch_add(1, std::memory_order_relaxed) + 1;
            std::string body;
            for (;;) {
                auto read_result = co_await exchange.read_body(64);
                if (!read_result) {
                    exchange.set_response_close();
                    exchange.set_response_content_length(0);
                    co_await exchange.send_response_header(400, "Bad Request");
                    co_return;
                }
                if (read_result->data_chain.readable_bytes() > 0) {
                    body.append(chain_to_string(std::move(read_result->data_chain)));
                }
                if (read_result->last) {
                    break;
                }
            }

            std::string response = std::to_string(count);
            response.push_back(':');
            response.append(body);
            if (count == 1) {
                response.push_back('|');
                response.append(exchange.request_trailers().get("digest"));
            } else {
                exchange.set_response_close();
            }

            exchange.set_response_content_length(response.size());
            auto header_result = co_await exchange.send_response_header(200);
            if (!header_result) {
                co_return;
            }
            co_await exchange.write_body(reinterpret_cast<const uint8_t *>(response.data()), response.size(), true);
            co_return;
        };
        return start_server(&group.at(0), handler, nullptr, &port_promise, &server_promise);
    });

    uint16_t port = port_future.get();
    ASSERT_NE(port, 0);
    auto *server = server_future.get();
    ASSERT_NE(server, nullptr);

    int client = connect_client(port);
    ASSERT_GE(client, 0);

    const char *request = "POST /one HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Transfer-Encoding: chunked\r\n"
                          "\r\n"
                          "4\r\nWiki\r\n"
                          "5\r\npedia\r\n"
                          "0\r\n"
                          "Digest: sha-256=xyz\r\n"
                          "\r\n"
                          "GET /two HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Connection: close\r\n"
                          "\r\n";
    ASSERT_EQ(::send(client, request, std::strlen(request), 0), static_cast<ssize_t>(std::strlen(request)));

    std::string response1 = recv_http_response(client);
    std::string response2 = recv_all(client);
    ::close(client);

    EXPECT_NE(response1.find("\r\n\r\n1:Wikipedia|sha-256=xyz"), std::string::npos);
    EXPECT_NE(response2.find("\r\n\r\n2:"), std::string::npos);
    EXPECT_EQ(request_count.load(std::memory_order_relaxed), 2);

    fiber::async::spawn(group.at(0), [&]() { return stop_server(&group.at(0), server); });
    group.join();
    delete server;
}

TEST(Http1ServerTest, EventLoopGroupDispatch) {
    fiber::event::EventLoopGroup group(2);
    group.start();

    std::promise<uint16_t> port_promise;
    std::promise<fiber::http::Http1Server *> server_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();
    std::atomic<bool> saw_worker_loop{false};

    fiber::async::spawn(group.at(0), [&]() -> fiber::async::DetachedTask {
        auto handler = [&](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
            if (&fiber::event::EventLoop::current() == &group.at(1)) {
                saw_worker_loop.store(true, std::memory_order_release);
            }
            exchange.set_response_content_length(2);
            auto header_result = co_await exchange.send_response_header(200);
            if (!header_result) {
                co_return;
            }
            co_await exchange.write_body(reinterpret_cast<const uint8_t *>("ok"), 2, true);
            co_return;
        };
        auto *server = new fiber::http::Http1Server(group.at(0), handler, {}, &group);
        fiber::net::ListenOptions options{};
        fiber::net::SocketAddress addr(fiber::net::IpAddress::loopback_v4(), 8080);
        auto bind_result = server->bind(addr, options);
        if (!bind_result) {
            port_promise.set_value(0);
            server_promise.set_value(nullptr);
            delete server;
            co_return;
        }
        auto port = resolve_port(server->fd());
        port_promise.set_value(port ? *port : 0);
        server_promise.set_value(server);
        fiber::async::spawn(group.at(0), [server]() { return server->serve(); });
        co_return;
    });

    uint16_t port = port_future.get();
    ASSERT_NE(port, 0);
    auto *server = server_future.get();
    ASSERT_NE(server, nullptr);

    for (int i = 0; i < 2; ++i) {
        int client = connect_client(port);
        ASSERT_GE(client, 0);
        const char *request = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        ASSERT_EQ(::send(client, request, std::strlen(request), 0), static_cast<ssize_t>(std::strlen(request)));
        std::string response = recv_all(client);
        ::close(client);
        EXPECT_NE(response.find("200"), std::string::npos);
    }

    EXPECT_TRUE(saw_worker_loop.load(std::memory_order_acquire));

    std::promise<void> shutdown_promise;
    auto shutdown_future = shutdown_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() -> fiber::async::DetachedTask {
        co_await server->shutdown_and_wait();
        shutdown_promise.set_value();
        co_return;
    });
    ASSERT_EQ(shutdown_future.wait_for(2s), std::future_status::ready);
    group.stop();
    group.join();
    delete server;
}

TEST(Http1ServerTest, ShutdownAndWait) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<uint16_t> port_promise;
    std::promise<fiber::http::Http1Server *> server_promise;
    std::promise<void> entered_promise;
    std::promise<void> shutdown_promise;
    auto port_future = port_promise.get_future();
    auto server_future = server_promise.get_future();
    auto entered_future = entered_promise.get_future();
    auto shutdown_future = shutdown_promise.get_future();
    std::atomic<bool> release_handler{false};

    fiber::async::spawn(group.at(0), [&]() {
        auto handler = [&](fiber::http::HttpExchange &exchange) -> fiber::async::Task<void> {
            entered_promise.set_value();
            while (!release_handler.load(std::memory_order_acquire)) {
                co_await fiber::async::sleep(10ms);
            }
            exchange.set_response_content_length(2);
            auto header_result = co_await exchange.send_response_header(200);
            if (!header_result) {
                co_return;
            }
            co_await exchange.write_body(reinterpret_cast<const uint8_t *>("ok"), 2, true);
            co_return;
        };
        return start_server(&group.at(0), handler, nullptr, &port_promise, &server_promise);
    });

    uint16_t port = port_future.get();
    ASSERT_NE(port, 0);
    auto *server = server_future.get();
    ASSERT_NE(server, nullptr);

    int client = connect_client(port);
    ASSERT_GE(client, 0);
    const char *request = "GET /slow HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ASSERT_EQ(::send(client, request, std::strlen(request), 0), static_cast<ssize_t>(std::strlen(request)));

    entered_future.get();
    fiber::async::spawn(group.at(0), [&]() -> fiber::async::DetachedTask {
        co_await server->shutdown_and_wait();
        shutdown_promise.set_value();
        co_return;
    });

    EXPECT_EQ(shutdown_future.wait_for(100ms), std::future_status::timeout);

    release_handler.store(true, std::memory_order_release);
    std::string response = recv_all(client);
    ::close(client);

    EXPECT_NE(response.find("Connection: close"), std::string::npos);
    EXPECT_EQ(shutdown_future.wait_for(2s), std::future_status::ready);

    group.stop();
    group.join();
    delete server;
}
