#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

#include <sys/socket.h>

#include "async/Spawn.h"
#include "common/IoError.h"
#include "event/EventLoop.h"
#include "net/SocketAddress.h"
#include "net/UdpSocket.h"

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
    if (!fiber::net::SocketAddress::from_sockaddr(reinterpret_cast<const sockaddr *>(&bound), len, local)) {
        return std::unexpected(IoErr::NotSupported);
    }
    return local.port();
}

DetachedTask echo_loop(fiber::event::EventLoop *loop, std::uint16_t port) {
    fiber::net::UdpSocket socket(*loop);
    fiber::net::UdpBindOptions options{};
    fiber::net::SocketAddress addr = fiber::net::SocketAddress::any_v4(port);
    auto bind_result = socket.bind(addr, options);
    if (!bind_result) {
        std::cerr << "bind failed: " << fiber::common::io_err_name(bind_result.error()) << '\n';
        loop->stop();
        co_return;
    }

    auto bound_port = resolve_port(socket.fd());
    if (bound_port) {
        std::cout << "listening on 0.0.0.0:" << *bound_port << '\n';
    } else {
        std::cout << "listening on 0.0.0.0\n";
    }

    std::array<char, 4096> buffer{};
    for (;;) {
        auto recv_result = co_await socket.recv_from(buffer.data(), buffer.size());
        if (!recv_result) {
            IoErr err = recv_result.error();
            std::cerr << "recv error: " << fiber::common::io_err_name(err) << '\n';
            if (err == IoErr::BadFd || err == IoErr::Canceled) {
                break;
            }
            continue;
        }

        auto send_result = co_await socket.send_to(buffer.data(), recv_result->size, recv_result->peer);
        if (!send_result) {
            IoErr err = send_result.error();
            std::cerr << "send error to " << recv_result->peer.to_string() << ": "
                      << fiber::common::io_err_name(err) << '\n';
            if (err == IoErr::BadFd || err == IoErr::Canceled) {
                break;
            }
            continue;
        }

        if (*send_result != recv_result->size) {
            std::cerr << "short send to " << recv_result->peer.to_string() << " ("
                      << *send_result << " of " << recv_result->size << ")\n";
        }
    }
    co_return;
}

} // namespace

int main(int argc, char **argv) {
    std::uint16_t port = 9000;
    if (argc > 1) {
        auto parsed = parse_port(argv[1]);
        if (!parsed) {
            std::cerr << "usage: udp_echo [port]\n";
            return 1;
        }
        port = *parsed;
    }

    fiber::event::EventLoop loop;
    fiber::async::spawn(loop, [&]() {
        return echo_loop(&loop, port);
    });
    loop.run();
    return 0;
}
