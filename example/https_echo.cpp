#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#include <sys/socket.h>
#include <unistd.h>

#include "async/Spawn.h"
#include "common/IoError.h"
#include "event/EventLoop.h"
#include "http/Http1Server.h"
#include "http/HttpTransport.h"
#include "http/TlsContext.h"
#include "net/SocketAddress.h"

namespace {

const char kSelfSignedCertPem[] = R"(-----BEGIN CERTIFICATE-----
MIIDCTCCAfGgAwIBAgIUEDCdxH6aX38+fEeFx3nlY3pJwdkwDQYJKoZIhvcNAQEL
BQAwFDESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTI2MDExNzEzMDcwNVoXDTI3MDEx
NzEzMDcwNVowFDESMBAGA1UEAwwJbG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEF
AAOCAQ8AMIIBCgKCAQEA4+tN+7EU3WmwFfjE4bn720reQJkTnAOUOYXg9zejQ75q
vHOpFxLU9z866mVpT7jVYAupmKfXrJ9U5Vd9znrWFzZt9rTdg+hISdujXjaEfEf+
GQ+66xthO2tAF3c6XokoqRpJR0GVInJoWaHBpV0PcvRb9AhRfuk+ja3W1dfdHnE8
LWutJCVK0HOWifIBGqpED3YMBNKZxFSKTCKLiqbxmnd6TT1fh8UI+AibEKhuJX4A
m3enMonO1PHeSOUY1dfXpZfdRdnYgjiyVyEw7oQL11r6O2LJZMJsoW912uIUnYrs
A4bDbMMfDgHe+PiyERCG62xydAlj1phGVlbGI/8HOQIDAQABo1MwUTAdBgNVHQ4E
FgQUvM4+Ad+L+GYd6i4nZgRFaPkRo7UwHwYDVR0jBBgwFoAUvM4+Ad+L+GYd6i4n
ZgRFaPkRo7UwDwYDVR0TAQH/BAUwAwEB/zANBgkqhkiG9w0BAQsFAAOCAQEAxo8i
jbyceTsjxiMDoXd/OPtPCD2CcpWOUxMb4hdGk3pMK6xFq8c7bdMcn6oZMF7xpdHg
jDTrfa8TlPITcG/34MtvPS3hq7klCPi948Z9wbtJWGfKAl3rHYK7PIIj3wNipTcQ
IkfIlO/t6VKPSx1S9HQA6nCDOvCufOL54Mfz0vI9Y47c4O1TNtbJiiWUkP/pEjEw
RMeULfoobqmMYTjbjQ8nKC25cQAmhQ0koOqJPquPtAHvaowqBT6jDLEL+8vR4Kfc
9UqEtfRr0+7LgbcofOsseDFYMPBW2GdpPMJ2PMYsQtFMXRoomlhjdpIct6e3rRnd
GiDzEZ0VwkYlJDwF4w==
-----END CERTIFICATE-----
)";

const char kSelfSignedKeyPem[] = R"(-----BEGIN PRIVATE KEY-----
MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQDj6037sRTdabAV
+MThufvbSt5AmROcA5Q5heD3N6NDvmq8c6kXEtT3PzrqZWlPuNVgC6mYp9esn1Tl
V33OetYXNm32tN2D6EhJ26NeNoR8R/4ZD7rrG2E7a0AXdzpeiSipGklHQZUicmhZ
ocGlXQ9y9Fv0CFF+6T6NrdbV190ecTwta60kJUrQc5aJ8gEaqkQPdgwE0pnEVIpM
IouKpvGad3pNPV+HxQj4CJsQqG4lfgCbd6cyic7U8d5I5RjV19ell91F2diCOLJX
ITDuhAvXWvo7Yslkwmyhb3Xa4hSdiuwDhsNswx8OAd74+LIREIbrbHJ0CWPWmEZW
VsYj/wc5AgMBAAECggEAHomvmDKg1g3MHxWG46u0uCwu3T7lZrkACjkK7HTS9ke0
K23f0Qyf5kTdkvxlgN4GEOlfHuoWNrXefSAc5iaFOvT7BNw09fCQhvzbxcrOM4y9
2gPGiqvPelOjccFy26nK/eVcviRmZAgqPSA0PwDaCg/9phPbP4Lm87rAF0TmBqbq
n5s+7MXf4iFTbRIec2zTikWfbUglhNmKr3eC/4+K+hk3TX95Wltvz6dGz+godV/L
FilwLEa+e0cSTUA8FYzYtoEUiV7/8dl8VBIvQWtx8sRNNihCmnlYrJ3N8tw/hO6F
PKpfoOo+L9uRJG4LGtAkM0Pqs9U9uN5v7F5HNMxO1QKBgQD61LhiF/ftPlTRFQm2
CrnIN4PcQtIDRar/cuwgyq3F8AAfJ5PSYD/GvitaQYxa9Ya1IM3T7UPx6L3OmJl6
updR3Mh/+6BtAYwSwoWLv0tHQ01xOe9pwML52JShVocVXQFE/UXNtuffuUpXVeWk
miVen8SI4CHLeFU+6Dfcp0l3owKBgQDonbYbB9bRVzG0gbgdp2K1pxvMQizR8IkU
GsYaT/LMooBpRBOHrane+9KCztkghjmTyDKEl7jwt65fvFl0ttkipq1ISTepV6Rt
Cmdc5PnBc+ON49/6ivTGFAdU5CY3sE/7L6ngPqZq6bq8nBJ0NPcjpfEl2JfBeND8
NisrSQEjcwKBgQDlcp1QLji/LtuLf0Eo41rbCd13KTDPiXVIw6m4vW6EuGyEE0In
mZ/9f4xMvdVUh3C4U8+04z/aFFs8l18eY310hxBp8pXn4RhvOL3M/iowgCJhRuv4
wzoYLsSXaX2cTz2QDFdEPOKTRv34Mj0le1Rf4Kp5wv1nESZ5qxceo3CTHQKBgFWb
jSR/ixB57YIH53GKY6qEuJdAl2wgAOLUQ6n1WF71Qxr6gdGCGS1GMiAP7hqpK1F2
8RiZGegFQXhcQfPRQzIcc1NSFtkMtyemF4o5fq0ycEGM5qY3M4QeZOBaIrKGAblo
vjUX+XkJUb8OFUCNKZMGBCywfJEoXIklilegw3l/AoGBALtmVrX28WQ42DOYWdKD
dmDMBg1+21d8wIWs4k5bu1LdlY8XqMnV9TAHwOwGcleK2uM3AfoLOho6HwFwdyhJ
x20XBogOziImjh+cvWNpm951EC3oWHOFYPsMjX1mRCye88LQHwm3gQ8iCIOzPj+8
RB6SahiCZEhAtLq/9Q/O1bL5
-----END PRIVATE KEY-----
)";

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

std::string make_temp_path(const char *tag) {
    std::string path = "/tmp/fiber_https_";
    path.append(tag);
    path.push_back('_');
    path.append(std::to_string(static_cast<long>(::getpid())));
    path.append(".pem");
    return path;
}

bool write_file(const std::string &path, std::string_view data) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return out.good();
}

struct TempFile {
    std::string path;
    bool ok = false;

    TempFile(const char *tag, std::string_view data) {
        path = make_temp_path(tag);
        ok = write_file(path, data);
        if (!ok) {
            path.clear();
        }
    }

    ~TempFile() {
        if (!path.empty()) {
            ::unlink(path.c_str());
        }
    }
};

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

fiber::http::HttpTask<void> handle_plain(fiber::http::HttpExchange &exchange) {
    const char *body = "hello https\n";
    exchange.set_response_header("Content-Type", "text/plain");
    exchange.set_response_content_length(std::strlen(body));
    auto header_result = co_await exchange.send_response_header(200);
    if (!header_result) {
        co_return;
    }
    co_await exchange.write_body(body, std::strlen(body), true);
    co_return;
}

fiber::async::DetachedTask run_demo_client(fiber::event::EventLoop *loop,
                                           fiber::http::Http1Server *server,
                                           std::uint16_t port,
                                           bool *ok) {
    auto fail = [&](std::string_view message, fiber::common::IoErr err) {
        std::cerr << message << ": " << fiber::common::io_err_name(err) << '\n';
        if (ok) {
            *ok = false;
        }
        if (server) {
            server->close();
        }
        if (loop) {
            loop->stop();
        }
    };

    fiber::net::SocketAddress target(fiber::net::IpAddress::loopback_v4(), port);
    auto infant_result = co_await fiber::net::TcpStream::connect(*loop, target);
    if (!infant_result) {
        fail("client connect failed", infant_result.error());
        co_return;
    }

    auto stream = std::make_unique<fiber::net::TcpStream>(std::move(*infant_result));

    fiber::http::TlsOptions tls_options{};
    tls_options.alpn = {"http/1.1"};
    fiber::http::TlsContext client_ctx(tls_options, false);
    auto ctx_result = client_ctx.init();
    if (!ctx_result) {
        fail("tls context init failed", ctx_result.error());
        co_return;
    }

    auto transport_result = fiber::http::TlsTransport::create(std::move(stream), client_ctx);
    if (!transport_result) {
        fail("tls transport create failed", transport_result.error());
        co_return;
    }
    auto transport = std::move(*transport_result);

    auto hs_result = co_await transport->handshake(std::chrono::seconds(5));
    if (!hs_result) {
        fail("tls handshake failed", hs_result.error());
        co_return;
    }

    const char *request =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "\r\n";
    size_t request_len = std::strlen(request);
    size_t write_offset = 0;
    while (write_offset < request_len) {
        auto write_result = co_await transport->write(request + write_offset,
                                                      request_len - write_offset,
                                                      std::chrono::seconds(5));
        if (!write_result || *write_result == 0) {
            auto err = write_result ? fiber::common::IoErr::BrokenPipe : write_result.error();
            fail("client write failed", err);
            co_return;
        }
        write_offset += *write_result;
    }

    std::string response;
    std::array<char, 4096> buffer{};
    for (;;) {
        auto read_result = co_await transport->read(buffer.data(),
                                                    buffer.size(),
                                                    std::chrono::seconds(5));
        if (!read_result) {
            fail("client read failed", read_result.error());
            co_return;
        }
        if (*read_result == 0) {
            break;
        }
        response.append(buffer.data(), *read_result);
    }

    std::cout << "demo response:\n" << response << "\n";

    transport->close();
    if (server) {
        server->close();
    }
    if (loop) {
        loop->stop();
    }
    co_return;
}

} // namespace

int main(int argc, char **argv) {
    bool demo = false;
    std::uint16_t port = 8443;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--demo") {
            demo = true;
            continue;
        }
        auto parsed = parse_port(argv[i]);
        if (!parsed) {
            std::cerr << "usage: https_echo [port] [--demo]\n";
            return 1;
        }
        port = *parsed;
    }

    TempFile cert_file("cert", kSelfSignedCertPem);
    TempFile key_file("key", kSelfSignedKeyPem);
    if (!cert_file.ok || !key_file.ok) {
        std::cerr << "write temp cert/key failed\n";
        return 1;
    }

    fiber::event::EventLoop loop;

    fiber::http::HttpServerOptions server_options{};
    server_options.tls.enabled = true;
    server_options.tls.cert_file = cert_file.path;
    server_options.tls.key_file = key_file.path;

    fiber::http::Http1Server server(loop, handle_plain, server_options);
    fiber::net::ListenOptions options{};
    fiber::net::SocketAddress addr(fiber::net::IpAddress::loopback_v4(), port);
    auto bind_result = server.bind(addr, options);
    if (!bind_result) {
        std::cerr << "bind failed: " << fiber::common::io_err_name(bind_result.error()) << '\n';
        return 1;
    }

    auto bound_port_result = resolve_port(server.fd());
    std::uint16_t effective_port = bound_port_result ? *bound_port_result : port;
    if (bound_port_result) {
        std::cout << "listening on https://127.0.0.1:" << *bound_port_result << '\n';
    } else {
        std::cout << "listening on https://127.0.0.1\n";
    }

    fiber::async::spawn(loop, [&]() {
        return server.serve();
    });

    if (demo) {
        bool ok = true;
        fiber::async::spawn(loop, [&]() {
            return run_demo_client(&loop, &server, effective_port, &ok);
        });
        loop.run();
        return ok ? 0 : 1;
    }

    std::cout << "try: curl -k https://127.0.0.1:" << effective_port << "/\n";
    loop.run();
    return 0;
}
