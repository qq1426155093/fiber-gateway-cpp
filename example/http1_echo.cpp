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
#include "http/Http1Server.h"
#include "net/SocketAddress.h"

namespace {

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
        return std::unexpected(fiber::common::IoErr::NotSupported);
    }
    return local.port();
}

fiber::http::HttpTask<void> handle_echo(fiber::http::HttpExchange &exchange) {
    std::array<char, 4096> buffer{};
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
}

} // namespace

int main(int argc, char **argv) {
    std::uint16_t port = 8080;
    if (argc > 1) {
        auto parsed = parse_port(argv[1]);
        if (!parsed) {
            std::cerr << "usage: http1_echo [port]\n";
            return 1;
        }
        port = *parsed;
    }

    fiber::event::EventLoop loop;

    fiber::http::Http1Server server(loop, handle_echo);
    fiber::net::ListenOptions options{};
    fiber::net::SocketAddress addr = fiber::net::SocketAddress::any_v4(port);
    auto bind_result = server.bind(addr, options);
    if (!bind_result) {
        std::cerr << "bind failed: " << fiber::common::io_err_name(bind_result.error()) << '\n';
        return 1;
    }

    auto bound_port_result = resolve_port(server.fd());
    if (bound_port_result) {
        std::cout << "listening on 0.0.0.0:" << *bound_port_result << '\n';
    } else {
        std::cout << "listening on 0.0.0.0\n";
    }

    fiber::async::spawn(loop, [&]() {
        return server.serve();
    });
    loop.run();
    return 0;
}
