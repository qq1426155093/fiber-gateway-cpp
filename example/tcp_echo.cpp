#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

#include <sys/socket.h>

#include "async/Spawn.h"
#include "async/Timeout.h"
#include "common/IoError.h"
#include "event/EventLoop.h"
#include "net/SocketAddress.h"
#include "net/TcpListener.h"
#include "net/TcpStream.h"

namespace {

using fiber::async::DetachedTask;
using fiber::common::IoErr;

std::optional<std::uint16_t> parse_port(const char *text) {
    if (!text) {
        return std::nullopt;
    }
    char *end = nullptr;
    unsigned long value = std::strtoul(text, &end, 10);
    if (!end || *end != '\0' || value > 65535) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(value);
}

fiber::common::IoResult<std::uint16_t> resolve_port(int fd) {
    sockaddr_storage bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&bound), &len) != 0) {
        return std::unexpected(fiber::common::io_err_from_errno(errno));
    }
    fiber::net::SocketAddress local;
    if (!fiber::net::SocketAddress::from_sockaddr(reinterpret_cast<sockaddr *>(&bound), len, local)) {
        return std::unexpected(IoErr::NotSupported);
    }
    return local.port();
}

DetachedTask echo_session(std::unique_ptr<fiber::net::TcpStream> stream) {
    std::array<char, 4096> buffer{};
    const std::string peer = stream->remote_addr().to_string();
    for (;;) {
        auto read_result = co_await fiber::async::timeout_for(
            [&]() { return stream->read(buffer.data(), buffer.size()); },
            std::chrono::minutes(1));
        if (!read_result) {
            if (read_result.error() == IoErr::TimedOut) {
                std::cerr << "idle timeout from " << peer << '\n';
            } else {
                std::cerr << "read error from " << peer << ": "
                          << fiber::common::io_err_name(read_result.error()) << '\n';
            }
            break;
        }
        if (*read_result == 0) {
            break;
        }
        size_t offset = 0;
        while (offset < *read_result) {
            auto write_result =
                co_await stream->write(buffer.data() + offset, *read_result - offset);
            if (!write_result) {
                std::cerr << "write error to " << peer << ": "
                          << fiber::common::io_err_name(write_result.error()) << '\n';
                stream->close();
                co_return;
            }
            if (*write_result == 0) {
                stream->close();
                co_return;
            }
            offset += *write_result;
        }
    }
    stream->close();
    co_return;
}

DetachedTask accept_loop(fiber::event::EventLoop *loop, std::uint16_t port) {
    fiber::net::TcpListener listener(*loop);
    fiber::net::ListenOptions options{};
    fiber::net::SocketAddress addr = fiber::net::SocketAddress::any_v4(port);
    auto bind_result = listener.bind(addr, options);
    if (!bind_result) {
        std::cerr << "bind failed: " << fiber::common::io_err_name(bind_result.error()) << '\n';
        loop->stop();
        co_return;
    }

    auto bound_port = resolve_port(listener.fd());
    if (bound_port) {
        std::cout << "listening on 0.0.0.0:" << *bound_port << '\n';
    } else {
        std::cout << "listening on 0.0.0.0\n";
    }

    for (;;) {
        auto accept_result = co_await listener.accept();
        if (!accept_result) {
            IoErr err = accept_result.error();
            std::cerr << "accept error: " << fiber::common::io_err_name(err) << '\n';
            if (err == IoErr::BadFd || err == IoErr::Canceled) {
                break;
            }
            continue;
        }
        if (accept_result->fd < 0) {
            std::cerr << "accept returned invalid fd\n";
            continue;
        }
        std::cerr << "client connected: " << accept_result->peer.to_string() << '\n';
        auto stream = std::make_unique<fiber::net::TcpStream>(*loop,
                                                              accept_result->fd,
                                                              accept_result->peer);
        fiber::async::spawn(*loop, [stream = std::move(stream)]() mutable {
            return echo_session(std::move(stream));
        });
    }
    co_return;
}

} // namespace

int main(int argc, char **argv) {
    std::uint16_t port = 9000;
    if (argc > 1) {
        auto parsed = parse_port(argv[1]);
        if (!parsed) {
            std::cerr << "usage: tcp_echo [port]\n";
            return 1;
        }
        port = *parsed;
    }

    fiber::event::EventLoop loop;

    fiber::async::spawn(loop, [&]() {
        return accept_loop(&loop, port);
    });
    loop.run();
    return 0;
}
