#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

#include <sys/socket.h>

#include "async/Spawn.h"
#include "async/Task.h"
#include "common/IoError.h"
#include "event/EventLoop.h"
#include "http/Http2Server.h"
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

fiber::async::Task<void> handle_echo(fiber::http::HttpExchange &exchange) {
    std::array<char, 4096> buffer{};
    std::string body;
    for (;;) {
        auto read_result = co_await exchange.read_body(buffer.data(), buffer.size());
        if (!read_result) {
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

    if (body.empty()) {
        body = "hello h2\n";
    }
    exchange.set_response_header("content-type", "text/plain");
    exchange.set_response_content_length(body.size());
    auto header_result = co_await exchange.send_response_header(200);
    if (!header_result) {
        co_return;
    }
    co_await exchange.write_body(reinterpret_cast<const uint8_t *>(body.data()), body.size(), true);
    co_return;
}

} // namespace

int main(int argc, char **argv) {
    std::uint16_t port = 8443;
    const char *cert_file = nullptr;
    const char *key_file = nullptr;

    if (argc == 3) {
        cert_file = argv[1];
        key_file = argv[2];
    } else if (argc == 4) {
        auto parsed = parse_port(argv[1]);
        if (!parsed) {
            std::cerr << "usage: http2_echo [port] <cert.pem> <key.pem>\n";
            return 1;
        }
        port = *parsed;
        cert_file = argv[2];
        key_file = argv[3];
    } else {
        std::cerr << "usage: http2_echo [port] <cert.pem> <key.pem>\n";
        return 1;
    }

    fiber::event::EventLoop loop;
    fiber::http::HttpServerOptions server_options{};
    server_options.tls.enabled = true;
    server_options.tls.cert_file = cert_file;
    server_options.tls.key_file = key_file;
    server_options.tls.alpn = {"h2"};

    fiber::http::Http2Server server(loop, handle_echo, server_options);
    fiber::net::ListenOptions options{};
    fiber::net::SocketAddress addr = fiber::net::SocketAddress::any_v4(port);
    auto bind_result = server.bind(addr, options);
    if (!bind_result) {
        std::cerr << "bind failed: " << fiber::common::io_err_name(bind_result.error()) << '\n';
        return 1;
    }

    auto bound_port_result = resolve_port(server.fd());
    std::uint16_t effective_port = bound_port_result ? *bound_port_result : port;
    std::cout << "listening on https://127.0.0.1:" << effective_port << '\n';
    std::cout << "try: curl --http2 -k https://127.0.0.1:" << effective_port << "/ -d 'ping'\n";

    fiber::async::spawn(loop, [&]() {
        return server.serve();
    });
    loop.run();
    return 0;
}
