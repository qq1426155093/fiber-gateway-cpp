#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cerrno>
#include <coroutine>
#include <future>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include "async/CoroutinePromiseBase.h"
#include "async/Spawn.h"
#include "common/IoError.h"
#include "event/EventLoopGroup.h"
#include "net/SocketAddress.h"
#include "net/TcpListener.h"
#include "net/TcpStream.h"

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

bool send_all(int fd, const char *data, size_t len) {
    size_t offset = 0;
    while (offset < len) {
        ssize_t rc = ::send(fd, data + offset, len - offset, 0);
        if (rc > 0) {
            offset += static_cast<size_t>(rc);
            continue;
        }
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

bool recv_all(int fd, char *data, size_t len) {
    size_t offset = 0;
    while (offset < len) {
        ssize_t rc = ::recv(fd, data + offset, len - offset, 0);
        if (rc > 0) {
            offset += static_cast<size_t>(rc);
            continue;
        }
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

void consume_iov(struct iovec *iov, int &iovcnt, size_t consumed) {
    size_t remaining = consumed;
    for (int i = 0; i < iovcnt && remaining > 0; ++i) {
        if (remaining >= iov[i].iov_len) {
            remaining -= iov[i].iov_len;
            iov[i].iov_base = static_cast<char *>(iov[i].iov_base) + iov[i].iov_len;
            iov[i].iov_len = 0;
        } else {
            iov[i].iov_base = static_cast<char *>(iov[i].iov_base) + remaining;
            iov[i].iov_len -= remaining;
            remaining = 0;
        }
    }
    int shift = 0;
    while (shift < iovcnt && iov[shift].iov_len == 0) {
        ++shift;
    }
    if (shift > 0) {
        for (int i = 0; i + shift < iovcnt; ++i) {
            iov[i] = iov[i + shift];
        }
        iovcnt -= shift;
    }
}

DetachedTask read_write_server(fiber::event::EventLoop *loop,
                               std::promise<uint16_t> *port_promise,
                               std::promise<fiber::common::IoResult<std::string>> *read_promise,
                               std::promise<fiber::common::IoResult<void>> *write_promise,
                               std::string request,
                               std::string response) {
    fiber::net::TcpListener listener(*loop);
    fiber::net::ListenOptions options{};
    fiber::net::SocketAddress addr(fiber::net::IpAddress::loopback_v4(), 0);

    bool port_set = false;
    bool read_set = false;
    bool write_set = false;
    auto set_port = [&](uint16_t port) {
        if (!port_set) {
            port_promise->set_value(port);
            port_set = true;
        }
    };
    auto set_read = [&](fiber::common::IoResult<std::string> result) {
        if (!read_set) {
            read_promise->set_value(std::move(result));
            read_set = true;
        }
    };
    auto set_write = [&](fiber::common::IoResult<void> result) {
        if (!write_set) {
            write_promise->set_value(std::move(result));
            write_set = true;
        }
    };

    auto fail = [&](fiber::common::IoErr err,
                    std::unique_ptr<fiber::net::TcpStream> &stream,
                    int &accepted_fd) {
        if (!port_set) {
            set_port(0);
        }
        set_read(std::unexpected(err));
        set_write(std::unexpected(err));
        if (stream) {
            stream->close();
            stream.reset();
        } else if (accepted_fd >= 0) {
            ::close(accepted_fd);
            accepted_fd = -1;
        }
        listener.close();
        fiber::event::EventLoop::current().stop();
    };

    auto bind_result = listener.bind(addr, options);
    if (!bind_result) {
        std::unique_ptr<fiber::net::TcpStream> none;
        int fd = -1;
        fail(bind_result.error(), none, fd);
        co_return;
    }
    auto port_result = resolve_port(listener.fd());
    if (!port_result) {
        std::unique_ptr<fiber::net::TcpStream> none;
        int fd = -1;
        fail(port_result.error(), none, fd);
        co_return;
    }
    set_port(*port_result);

    auto accept_result = co_await listener.accept();
    if (!accept_result) {
        std::unique_ptr<fiber::net::TcpStream> none;
        int fd = -1;
        fail(accept_result.error(), none, fd);
        co_return;
    }
    int accepted_fd = accept_result->fd;
    if (accepted_fd < 0) {
        std::unique_ptr<fiber::net::TcpStream> none;
        fail(fiber::common::IoErr::BadFd, none, accepted_fd);
        co_return;
    }
    std::unique_ptr<fiber::net::TcpStream> stream =
        std::make_unique<fiber::net::TcpStream>(*loop, accepted_fd);
    accepted_fd = -1;

    std::string buffer(request.size(), '\0');
    size_t offset = 0;
    while (offset < buffer.size()) {
        auto result = co_await stream->read(buffer.data() + offset, buffer.size() - offset);
        if (!result || *result == 0) {
            auto err = result ? fiber::common::IoErr::NotConnected : result.error();
            fail(err, stream, accepted_fd);
            co_return;
        }
        offset += *result;
    }
    set_read(buffer);

    size_t write_offset = 0;
    while (write_offset < response.size()) {
        auto result = co_await stream->write(response.data() + write_offset, response.size() - write_offset);
        if (!result || *result == 0) {
            auto err = result ? fiber::common::IoErr::NotConnected : result.error();
            fail(err, stream, accepted_fd);
            co_return;
        }
        write_offset += *result;
    }
    set_write({});

    stream->close();
    listener.close();
    fiber::event::EventLoop::current().stop();
    co_return;
}

DetachedTask readv_writev_server(fiber::event::EventLoop *loop,
                                 std::promise<uint16_t> *port_promise,
                                 std::promise<fiber::common::IoResult<std::string>> *read_promise,
                                 std::promise<fiber::common::IoResult<void>> *write_promise,
                                 std::string request_part1,
                                 std::string request_part2,
                                 std::string response_part1,
                                 std::string response_part2) {
    fiber::net::TcpListener listener(*loop);
    fiber::net::ListenOptions options{};
    fiber::net::SocketAddress addr(fiber::net::IpAddress::loopback_v4(), 0);

    bool port_set = false;
    bool read_set = false;
    bool write_set = false;
    auto set_port = [&](uint16_t port) {
        if (!port_set) {
            port_promise->set_value(port);
            port_set = true;
        }
    };
    auto set_read = [&](fiber::common::IoResult<std::string> result) {
        if (!read_set) {
            read_promise->set_value(std::move(result));
            read_set = true;
        }
    };
    auto set_write = [&](fiber::common::IoResult<void> result) {
        if (!write_set) {
            write_promise->set_value(std::move(result));
            write_set = true;
        }
    };

    auto fail = [&](fiber::common::IoErr err,
                    std::unique_ptr<fiber::net::TcpStream> &stream,
                    int &accepted_fd) {
        if (!port_set) {
            set_port(0);
        }
        set_read(std::unexpected(err));
        set_write(std::unexpected(err));
        if (stream) {
            stream->close();
            stream.reset();
        } else if (accepted_fd >= 0) {
            ::close(accepted_fd);
            accepted_fd = -1;
        }
        listener.close();
        fiber::event::EventLoop::current().stop();
    };

    auto bind_result = listener.bind(addr, options);
    if (!bind_result) {
        std::unique_ptr<fiber::net::TcpStream> none;
        int fd = -1;
        fail(bind_result.error(), none, fd);
        co_return;
    }
    auto port_result = resolve_port(listener.fd());
    if (!port_result) {
        std::unique_ptr<fiber::net::TcpStream> none;
        int fd = -1;
        fail(port_result.error(), none, fd);
        co_return;
    }
    set_port(*port_result);

    auto accept_result = co_await listener.accept();
    if (!accept_result) {
        std::unique_ptr<fiber::net::TcpStream> none;
        int fd = -1;
        fail(accept_result.error(), none, fd);
        co_return;
    }
    int accepted_fd = accept_result->fd;
    if (accepted_fd < 0) {
        std::unique_ptr<fiber::net::TcpStream> none;
        fail(fiber::common::IoErr::BadFd, none, accepted_fd);
        co_return;
    }
    std::unique_ptr<fiber::net::TcpStream> stream =
        std::make_unique<fiber::net::TcpStream>(*loop, accepted_fd);
    accepted_fd = -1;

    std::string buffer1(request_part1.size(), '\0');
    std::string buffer2(request_part2.size(), '\0');
    std::array<iovec, 2> iov{};
    iov[0].iov_base = buffer1.data();
    iov[0].iov_len = buffer1.size();
    iov[1].iov_base = buffer2.data();
    iov[1].iov_len = buffer2.size();
    int iovcnt = static_cast<int>(iov.size());
    size_t remaining = buffer1.size() + buffer2.size();
    while (remaining > 0) {
        auto result = co_await stream->readv(iov.data(), iovcnt);
        if (!result || *result == 0) {
            auto err = result ? fiber::common::IoErr::NotConnected : result.error();
            fail(err, stream, accepted_fd);
            co_return;
        }
        remaining -= *result;
        consume_iov(iov.data(), iovcnt, *result);
    }
    set_read(buffer1 + buffer2);

    std::array<iovec, 2> out_iov{};
    out_iov[0].iov_base = const_cast<char *>(response_part1.data());
    out_iov[0].iov_len = response_part1.size();
    out_iov[1].iov_base = const_cast<char *>(response_part2.data());
    out_iov[1].iov_len = response_part2.size();
    int out_cnt = static_cast<int>(out_iov.size());
    size_t out_remaining = response_part1.size() + response_part2.size();
    while (out_remaining > 0) {
        auto result = co_await stream->writev(out_iov.data(), out_cnt);
        if (!result || *result == 0) {
            auto err = result ? fiber::common::IoErr::NotConnected : result.error();
            fail(err, stream, accepted_fd);
            co_return;
        }
        out_remaining -= *result;
        consume_iov(out_iov.data(), out_cnt, *result);
    }
    set_write({});

    stream->close();
    listener.close();
    fiber::event::EventLoop::current().stop();
    co_return;
}

DetachedTask connect_client(fiber::event::EventLoop *loop,
                            fiber::net::SocketAddress target,
                            std::promise<fiber::common::IoResult<std::string>> *response_promise,
                            std::string request,
                            std::string expected_response) {
    auto infant_result = co_await fiber::net::TcpStream::connect(*loop, target);
    if (!infant_result) {
        response_promise->set_value(std::unexpected(infant_result.error()));
        fiber::event::EventLoop::current().stop();
        co_return;
    }
    fiber::net::TcpStream stream(std::move(*infant_result));

    size_t write_offset = 0;
    while (write_offset < request.size()) {
        auto result = co_await stream.write(request.data() + write_offset, request.size() - write_offset);
        if (!result || *result == 0) {
            auto err = result ? fiber::common::IoErr::NotConnected : result.error();
            response_promise->set_value(std::unexpected(err));
            stream.close();
            fiber::event::EventLoop::current().stop();
            co_return;
        }
        write_offset += *result;
    }

    std::string buffer(expected_response.size(), '\0');
    size_t read_offset = 0;
    while (read_offset < buffer.size()) {
        auto result = co_await stream.read(buffer.data() + read_offset, buffer.size() - read_offset);
        if (!result || *result == 0) {
            auto err = result ? fiber::common::IoErr::NotConnected : result.error();
            response_promise->set_value(std::unexpected(err));
            stream.close();
            fiber::event::EventLoop::current().stop();
            co_return;
        }
        read_offset += *result;
    }
    response_promise->set_value(buffer);
    stream.close();
    fiber::event::EventLoop::current().stop();
    co_return;
}

} // namespace

TEST(TcpStreamTest, ReadWriteRoundTrip) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<uint16_t> port_promise;
    std::promise<fiber::common::IoResult<std::string>> read_promise;
    std::promise<fiber::common::IoResult<void>> write_promise;
    auto port_future = port_promise.get_future();
    auto read_future = read_promise.get_future();
    auto write_future = write_promise.get_future();

    const std::string request = "ping";
    const std::string response = "pong";

    fiber::async::spawn(group.at(0), [&]() {
        read_write_server(&group.at(0), &port_promise, &read_promise, &write_promise, request, response);
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
    ASSERT_TRUE(send_all(client, request.data(), request.size()));

    std::string response_buffer(response.size(), '\0');
    ASSERT_TRUE(recv_all(client, response_buffer.data(), response_buffer.size()));
    EXPECT_EQ(response_buffer, response);
    ::close(client);

    if (read_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "read did not complete in time";
        return;
    }
    if (write_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "write did not complete in time";
        return;
    }

    auto read_result = read_future.get();
    ASSERT_TRUE(read_result);
    EXPECT_EQ(*read_result, request);
    auto write_result = write_future.get();
    ASSERT_TRUE(write_result);
    group.join();
}

TEST(TcpStreamTest, ConnectsWithAwaiter) {
    fiber::event::EventLoopGroup group(2);
    group.start();

    std::promise<uint16_t> port_promise;
    std::promise<fiber::common::IoResult<std::string>> read_promise;
    std::promise<fiber::common::IoResult<void>> write_promise;
    std::promise<fiber::common::IoResult<std::string>> response_promise;
    auto port_future = port_promise.get_future();
    auto read_future = read_promise.get_future();
    auto write_future = write_promise.get_future();
    auto response_future = response_promise.get_future();

    const std::string request = "ping";
    const std::string response = "pong";

    fiber::async::spawn(group.at(0), [&]() {
        read_write_server(&group.at(0), &port_promise, &read_promise, &write_promise, request, response);
    });

    uint16_t port = port_future.get();
    ASSERT_NE(port, 0);

    fiber::net::SocketAddress target(fiber::net::IpAddress::loopback_v4(), port);
    fiber::async::spawn(group.at(1), [&]() {
        connect_client(&group.at(1), target, &response_promise, request, response);
    });

    if (response_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "connect client did not complete in time";
        return;
    }
    auto response_result = response_future.get();
    ASSERT_TRUE(response_result);
    EXPECT_EQ(*response_result, response);

    if (read_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "server read did not complete in time";
        return;
    }
    if (write_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "server write did not complete in time";
        return;
    }
    auto read_result = read_future.get();
    ASSERT_TRUE(read_result);
    EXPECT_EQ(*read_result, request);
    auto write_result = write_future.get();
    ASSERT_TRUE(write_result);
    group.join();
}

TEST(TcpStreamTest, ReadvWritevRoundTrip) {
    fiber::event::EventLoopGroup group(1);
    group.start();

    std::promise<uint16_t> port_promise;
    std::promise<fiber::common::IoResult<std::string>> read_promise;
    std::promise<fiber::common::IoResult<void>> write_promise;
    auto port_future = port_promise.get_future();
    auto read_future = read_promise.get_future();
    auto write_future = write_promise.get_future();

    const std::string request_part1 = "abc";
    const std::string request_part2 = "defg";
    const std::string response_part1 = "hi";
    const std::string response_part2 = "there";

    fiber::async::spawn(group.at(0), [&]() {
        readv_writev_server(&group.at(0),
                            &port_promise,
                            &read_promise,
                            &write_promise,
                            request_part1,
                            request_part2,
                            response_part1,
                            response_part2);
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
    const std::string request = request_part1 + request_part2;
    ASSERT_TRUE(send_all(client, request.data(), request.size()));

    const std::string expected_response = response_part1 + response_part2;
    std::string response_buffer(expected_response.size(), '\0');
    ASSERT_TRUE(recv_all(client, response_buffer.data(), response_buffer.size()));
    EXPECT_EQ(response_buffer, expected_response);
    ::close(client);

    if (read_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "readv did not complete in time";
        return;
    }
    if (write_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "writev did not complete in time";
        return;
    }

    auto read_result = read_future.get();
    ASSERT_TRUE(read_result);
    EXPECT_EQ(*read_result, request);
    auto write_result = write_future.get();
    ASSERT_TRUE(write_result);
    group.join();
}
