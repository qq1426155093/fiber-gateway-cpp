#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <lsquic.h>
#include <lsxpack_header.h>
#include <openssl/ssl.h>

namespace {

constexpr size_t kMaxDatagram = 65535;
constexpr size_t kMaxEvents = 8;
constexpr std::string_view kBody = "hello h3 via lsquic\n";

struct ServerCtx;

struct ConnCtx {
};

struct StreamCtx {
    std::string request;
    size_t body_offset = 0;
    bool ready_to_respond = false;
    bool headers_sent = false;
};

struct ServerCtx {
    int udp_fd = -1;
    int epoll_fd = -1;
    uint16_t port = 8443;
    uint32_t epoll_events = EPOLLIN;
    lsquic_engine_t *engine = nullptr;
    lsquic_engine_settings settings{};
    SSL_CTX *ssl_ctx = nullptr;
    std::atomic<bool> stop{false};
    bool tx_blocked = false;
};

ServerCtx *g_server = nullptr;

struct HeaderBuf {
    unsigned off = 0;
    std::array<char, 1024> buf{};
};

socklen_t sockaddr_length(const sockaddr *sa) {
    if (!sa) {
        return 0;
    }
    switch (sa->sa_family) {
        case AF_INET:
            return sizeof(sockaddr_in);
        case AF_INET6:
            return sizeof(sockaddr_in6);
        default:
            return sizeof(sockaddr_storage);
    }
}

bool set_sockopt_int(int fd, int level, int optname, int value) {
    return ::setsockopt(fd, level, optname, &value, sizeof(value)) == 0;
}

bool parse_port(const char *text, uint16_t &out) {
    if (!text) {
        return false;
    }
    char *end = nullptr;
    unsigned long v = std::strtoul(text, &end, 10);
    if (!end || *end != '\0' || v > 65535UL) {
        return false;
    }
    out = static_cast<uint16_t>(v);
    return true;
}

int open_udp_socket(uint16_t port) {
    int fd = ::socket(AF_INET6, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }

    set_sockopt_int(fd, SOL_SOCKET, SO_REUSEADDR, 1);
#ifdef SO_REUSEPORT
    set_sockopt_int(fd, SOL_SOCKET, SO_REUSEPORT, 1);
#endif
    set_sockopt_int(fd, IPPROTO_IPV6, IPV6_V6ONLY, 0);
    set_sockopt_int(fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, 1);
    set_sockopt_int(fd, IPPROTO_IPV6, IPV6_RECVTCLASS, 1);
    set_sockopt_int(fd, IPPROTO_IP, IP_PKTINFO, 1);
    set_sockopt_int(fd, IPPROTO_IP, IP_RECVTOS, 1);

    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;
    addr.sin6_port = htons(port);
    if (::bind(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool setup_epoll(ServerCtx &server) {
    server.epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
    if (server.epoll_fd < 0) {
        return false;
    }
    epoll_event ev{};
    ev.events = server.epoll_events;
    ev.data.fd = server.udp_fd;
    return ::epoll_ctl(server.epoll_fd, EPOLL_CTL_ADD, server.udp_fd, &ev) == 0;
}

bool update_epoll_interest(ServerCtx &server, uint32_t wanted_events) {
    if (wanted_events == server.epoll_events) {
        return true;
    }
    epoll_event ev{};
    ev.events = wanted_events;
    ev.data.fd = server.udp_fd;
    if (::epoll_ctl(server.epoll_fd, EPOLL_CTL_MOD, server.udp_fd, &ev) != 0) {
        return false;
    }
    server.epoll_events = wanted_events;
    return true;
}

int header_set_ptr(lsxpack_header *hdr,
                   HeaderBuf &header_buf,
                   std::string_view name,
                   std::string_view value) {
    if (header_buf.off + name.size() + value.size() > header_buf.buf.size()) {
        return -1;
    }
    char *dst = header_buf.buf.data() + header_buf.off;
    std::memcpy(dst, name.data(), name.size());
    std::memcpy(dst + name.size(), value.data(), value.size());
    lsxpack_header_set_offset2(hdr,
                               dst,
                               0,
                               name.size(),
                               name.size(),
                               value.size());
    header_buf.off += static_cast<unsigned>(name.size() + value.size());
    return 0;
}

size_t fill_tx_control(const lsquic_out_spec &spec, std::array<unsigned char, 128> &control, msghdr &msg) {
    msg.msg_control = nullptr;
    msg.msg_controllen = 0;
    if (!spec.local_sa && spec.ecn <= 0) {
        return 0;
    }

    control.fill(0);
    msg.msg_control = control.data();
    msg.msg_controllen = control.size();
    cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg) {
        msg.msg_control = nullptr;
        msg.msg_controllen = 0;
        return 0;
    }

    auto advance = [&](cmsghdr *cur) -> cmsghdr * {
        auto *next = CMSG_NXTHDR(&msg, cur);
        if (!next) {
            msg.msg_control = nullptr;
            msg.msg_controllen = 0;
        }
        return next;
    };

    if (spec.local_sa) {
        if (spec.local_sa->sa_family == AF_INET) {
            const auto *sa = reinterpret_cast<const sockaddr_in *>(spec.local_sa);
            cmsg->cmsg_level = IPPROTO_IP;
            cmsg->cmsg_type = IP_PKTINFO;
            cmsg->cmsg_len = CMSG_LEN(sizeof(in_pktinfo));
            auto *pkt = reinterpret_cast<in_pktinfo *>(CMSG_DATA(cmsg));
            std::memset(pkt, 0, sizeof(*pkt));
            pkt->ipi_spec_dst = sa->sin_addr;
            cmsg = advance(cmsg);
            if (!cmsg) {
                return 0;
            }
        } else if (spec.local_sa->sa_family == AF_INET6) {
            const auto *sa6 = reinterpret_cast<const sockaddr_in6 *>(spec.local_sa);
            cmsg->cmsg_level = IPPROTO_IPV6;
            cmsg->cmsg_type = IPV6_PKTINFO;
            cmsg->cmsg_len = CMSG_LEN(sizeof(in6_pktinfo));
            auto *pkt6 = reinterpret_cast<in6_pktinfo *>(CMSG_DATA(cmsg));
            std::memset(pkt6, 0, sizeof(*pkt6));
            pkt6->ipi6_addr = sa6->sin6_addr;
            pkt6->ipi6_ifindex = sa6->sin6_scope_id;
            cmsg = advance(cmsg);
            if (!cmsg) {
                return 0;
            }
        }
    }

    if (spec.ecn >= 0) {
        int ecn = spec.ecn & 0x03;
        int family = spec.dest_sa ? spec.dest_sa->sa_family : AF_UNSPEC;
        if (family == AF_INET) {
            cmsg->cmsg_level = IPPROTO_IP;
            cmsg->cmsg_type = IP_TOS;
            cmsg->cmsg_len = CMSG_LEN(sizeof(ecn));
            std::memcpy(CMSG_DATA(cmsg), &ecn, sizeof(ecn));
            cmsg = advance(cmsg);
            if (!cmsg) {
                return 0;
            }
        } else if (family == AF_INET6) {
            cmsg->cmsg_level = IPPROTO_IPV6;
            cmsg->cmsg_type = IPV6_TCLASS;
            cmsg->cmsg_len = CMSG_LEN(sizeof(ecn));
            std::memcpy(CMSG_DATA(cmsg), &ecn, sizeof(ecn));
            cmsg = advance(cmsg);
            if (!cmsg) {
                return 0;
            }
        }
    }

    size_t used = reinterpret_cast<unsigned char *>(cmsg) - control.data();
    msg.msg_controllen = used;
    return used;
}

int packets_out_cb(void *ctx, const struct lsquic_out_spec *specs, unsigned n_specs) {
    auto *server = static_cast<ServerCtx *>(ctx);
    if (!server || server->udp_fd < 0) {
        return -1;
    }

    unsigned sent = 0;
    for (unsigned i = 0; i < n_specs; ++i) {
        const auto &spec = specs[i];
        msghdr msg{};
        std::array<unsigned char, 128> control{};
        msg.msg_name = const_cast<sockaddr *>(spec.dest_sa);
        msg.msg_namelen = sockaddr_length(spec.dest_sa);
        msg.msg_iov = spec.iov;
        msg.msg_iovlen = spec.iovlen;
        fill_tx_control(spec, control, msg);

        ssize_t rc = ::sendmsg(server->udp_fd, &msg, MSG_DONTWAIT);
        if (rc < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                server->tx_blocked = true;
                break;
            }
            if (errno == EMSGSIZE) {
                continue;
            }
            break;
        }
        ++sent;
    }
    return static_cast<int>(sent);
}

SSL_CTX *lookup_cert_cb(void *ctx, const sockaddr *local, const char *sni) {
    (void) local;
    (void) sni;
    auto *server = static_cast<ServerCtx *>(ctx);
    return server ? server->ssl_ctx : nullptr;
}

SSL_CTX *get_ssl_ctx_cb(void *peer_ctx, const sockaddr *local) {
    (void) local;
    auto *server = static_cast<ServerCtx *>(peer_ctx);
    if (server) {
        return server->ssl_ctx;
    }
    return g_server ? g_server->ssl_ctx : nullptr;
}

lsquic_conn_ctx_t *on_new_conn(void *stream_if_ctx, lsquic_conn_t *conn) {
    (void) stream_if_ctx;
    (void) conn;
    return reinterpret_cast<lsquic_conn_ctx_t *>(new ConnCtx());
}

void on_conn_closed(lsquic_conn_t *conn) {
    auto *ctx = reinterpret_cast<ConnCtx *>(lsquic_conn_get_ctx(conn));
    delete ctx;
}

lsquic_stream_ctx_t *on_new_stream(void *stream_if_ctx, lsquic_stream_t *stream) {
    (void) stream_if_ctx;
    auto *ctx = new StreamCtx();
    lsquic_stream_wantread(stream, 1);
    return reinterpret_cast<lsquic_stream_ctx_t *>(ctx);
}

void on_read(lsquic_stream_t *stream, lsquic_stream_ctx_t *h) {
    auto *ctx = reinterpret_cast<StreamCtx *>(h);
    if (!ctx) {
        lsquic_stream_close(stream);
        return;
    }

    std::array<char, 4096> buf{};
    for (;;) {
        ssize_t nr = lsquic_stream_read(stream, buf.data(), buf.size());
        if (nr > 0) {
            ctx->request.append(buf.data(), static_cast<size_t>(nr));
            if (ctx->request.find("\r\n\r\n") != std::string::npos) {
                ctx->ready_to_respond = true;
                lsquic_stream_wantread(stream, 0);
                lsquic_stream_wantwrite(stream, 1);
                return;
            }
            continue;
        }
        if (nr == 0) {
            ctx->ready_to_respond = true;
            lsquic_stream_wantread(stream, 0);
            lsquic_stream_wantwrite(stream, 1);
            return;
        }
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            return;
        }
        lsquic_stream_close(stream);
        return;
    }
}

void on_write(lsquic_stream_t *stream, lsquic_stream_ctx_t *h) {
    auto *ctx = reinterpret_cast<StreamCtx *>(h);
    if (!ctx || !ctx->ready_to_respond) {
        return;
    }

    if (!ctx->headers_sent) {
        HeaderBuf header_buf{};
        std::array<lsxpack_header, 3> headers_arr{};
        std::string content_len = std::to_string(kBody.size());
        if (0 != header_set_ptr(&headers_arr[0], header_buf, ":status", "200") ||
            0 != header_set_ptr(&headers_arr[1], header_buf, "content-type", "text/plain") ||
            0 != header_set_ptr(&headers_arr[2], header_buf, "content-length", content_len)) {
            lsquic_stream_close(stream);
            return;
        }

        lsquic_http_headers headers{};
        headers.count = static_cast<int>(headers_arr.size());
        headers.headers = headers_arr.data();
        if (0 != lsquic_stream_send_headers(stream, &headers, 0)) {
            lsquic_stream_close(stream);
            return;
        }
        ctx->headers_sent = true;
    }

    while (ctx->body_offset < kBody.size()) {
        const char *ptr = kBody.data() + ctx->body_offset;
        size_t left = kBody.size() - ctx->body_offset;
        ssize_t nw = lsquic_stream_write(stream, ptr, left);
        if (nw > 0) {
            ctx->body_offset += static_cast<size_t>(nw);
            continue;
        }
        if (nw == 0 || errno == EWOULDBLOCK || errno == EAGAIN) {
            return;
        }
        lsquic_stream_close(stream);
        return;
    }

    lsquic_stream_shutdown(stream, 1);
    lsquic_stream_wantwrite(stream, 0);
}

void on_close(lsquic_stream_t *stream, lsquic_stream_ctx_t *h) {
    (void) stream;
    auto *ctx = reinterpret_cast<StreamCtx *>(h);
    delete ctx;
}

lsquic_stream_if make_stream_if() {
    lsquic_stream_if iface{};
    iface.on_new_conn = on_new_conn;
    iface.on_conn_closed = on_conn_closed;
    iface.on_new_stream = on_new_stream;
    iface.on_read = on_read;
    iface.on_write = on_write;
    iface.on_close = on_close;
    return iface;
}

const lsquic_stream_if kStreamIf = make_stream_if();

sockaddr_storage default_local_for_peer(int family, uint16_t port) {
    sockaddr_storage local{};
    if (family == AF_INET) {
        auto *sa = reinterpret_cast<sockaddr_in *>(&local);
        sa->sin_family = AF_INET;
        sa->sin_port = htons(port);
        sa->sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        auto *sa = reinterpret_cast<sockaddr_in6 *>(&local);
        sa->sin6_family = AF_INET6;
        sa->sin6_port = htons(port);
        sa->sin6_addr = in6addr_any;
    }
    return local;
}

void fill_local_and_ecn(const msghdr &msg,
                        sockaddr_storage &local,
                        int &ecn,
                        uint16_t bound_port,
                        int peer_family) {
    local = default_local_for_peer(peer_family, bound_port);
    ecn = 0;

    for (cmsghdr *cmsg = CMSG_FIRSTHDR(const_cast<msghdr *>(&msg));
         cmsg;
         cmsg = CMSG_NXTHDR(const_cast<msghdr *>(&msg), cmsg)) {
        if (cmsg->cmsg_level == IPPROTO_IPV6 && cmsg->cmsg_type == IPV6_PKTINFO &&
            cmsg->cmsg_len >= CMSG_LEN(sizeof(in6_pktinfo))) {
            auto *pkt = reinterpret_cast<in6_pktinfo *>(CMSG_DATA(cmsg));
            auto *sa = reinterpret_cast<sockaddr_in6 *>(&local);
            sa->sin6_family = AF_INET6;
            sa->sin6_port = htons(bound_port);
            sa->sin6_addr = pkt->ipi6_addr;
            continue;
        }
        if (cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_PKTINFO &&
            cmsg->cmsg_len >= CMSG_LEN(sizeof(in_pktinfo))) {
            auto *pkt = reinterpret_cast<in_pktinfo *>(CMSG_DATA(cmsg));
            auto *sa = reinterpret_cast<sockaddr_in *>(&local);
            sa->sin_family = AF_INET;
            sa->sin_port = htons(bound_port);
            sa->sin_addr = pkt->ipi_addr;
            continue;
        }
        if (cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_TOS &&
            cmsg->cmsg_len >= CMSG_LEN(sizeof(unsigned char))) {
            int tos = *reinterpret_cast<unsigned char *>(CMSG_DATA(cmsg));
            ecn = tos & 0x03;
            continue;
        }
        if (cmsg->cmsg_level == IPPROTO_IPV6 && cmsg->cmsg_type == IPV6_TCLASS &&
            cmsg->cmsg_len >= CMSG_LEN(sizeof(int))) {
            int tclass = *reinterpret_cast<int *>(CMSG_DATA(cmsg));
            ecn = tclass & 0x03;
            continue;
        }
    }
}

void drain_udp_and_feed_engine(ServerCtx &server) {
    std::array<unsigned char, kMaxDatagram> packet{};
    std::array<unsigned char, CMSG_SPACE(sizeof(in6_pktinfo)) +
                              CMSG_SPACE(sizeof(in_pktinfo)) +
                              CMSG_SPACE(sizeof(int))> control{};

    for (;;) {
        sockaddr_storage peer{};
        iovec iov{};
        iov.iov_base = packet.data();
        iov.iov_len = packet.size();

        msghdr msg{};
        msg.msg_name = &peer;
        msg.msg_namelen = sizeof(peer);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control.data();
        msg.msg_controllen = control.size();

        ssize_t nr = ::recvmsg(server.udp_fd, &msg, MSG_DONTWAIT);
        if (nr < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (nr == 0) {
            continue;
        }
        if (msg.msg_namelen == 0) {
            continue;
        }

        int peer_family = reinterpret_cast<sockaddr *>(&peer)->sa_family;
        if (peer_family != AF_INET && peer_family != AF_INET6) {
            continue;
        }

        sockaddr_storage local{};
        int ecn = 0;
        fill_local_and_ecn(msg, local, ecn, server.port, peer_family);

        lsquic_engine_packet_in(server.engine,
                                packet.data(),
                                static_cast<size_t>(nr),
                                reinterpret_cast<const sockaddr *>(&local),
                                reinterpret_cast<const sockaddr *>(&peer),
                                &server,
                                ecn);
    }
}

int compute_wait_timeout_ms(lsquic_engine_t *engine, bool tx_blocked) {
    int timeout_ms = -1;
    int diff_us = 0;
    if (lsquic_engine_earliest_adv_tick(engine, &diff_us)) {
        timeout_ms = diff_us <= 0 ? 0 : static_cast<int>((diff_us + 999) / 1000);
    }
    if (tx_blocked && (timeout_ms < 0 || timeout_ms > 5)) {
        timeout_ms = 5;
    }
    return timeout_ms;
}

void on_signal(int signo) {
    (void) signo;
    if (g_server) {
        g_server->stop.store(true, std::memory_order_release);
    }
}

bool install_signal_handlers() {
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, nullptr) != 0) {
        return false;
    }
    if (sigaction(SIGTERM, &sa, nullptr) != 0) {
        return false;
    }
    return true;
}

SSL_CTX *create_server_ssl_ctx(const char *cert_file, const char *key_file) {
    SSL_CTX *ssl = SSL_CTX_new(TLS_method());
    if (!ssl) {
        return nullptr;
    }

    SSL_CTX_set_min_proto_version(ssl, TLS1_3_VERSION);
    if (SSL_CTX_use_certificate_chain_file(ssl, cert_file) != 1) {
        SSL_CTX_free(ssl);
        return nullptr;
    }
    if (SSL_CTX_use_PrivateKey_file(ssl, key_file, SSL_FILETYPE_PEM) != 1) {
        SSL_CTX_free(ssl);
        return nullptr;
    }
    if (SSL_CTX_check_private_key(ssl) != 1) {
        SSL_CTX_free(ssl);
        return nullptr;
    }
    return ssl;
}

lsquic_engine_t *create_engine(ServerCtx &server) {
    lsquic_engine_init_settings(&server.settings, LSENG_SERVER | LSENG_HTTP);

    lsquic_engine_api api{};
    api.ea_packets_out = packets_out_cb;
    api.ea_packets_out_ctx = &server;
    api.ea_stream_if = &kStreamIf;
    api.ea_stream_if_ctx = &server;
    api.ea_settings = &server.settings;
    api.ea_lookup_cert = lookup_cert_cb;
    api.ea_cert_lu_ctx = &server;
    api.ea_get_ssl_ctx = get_ssl_ctx_cb;

    return lsquic_engine_new(LSENG_SERVER | LSENG_HTTP, &api);
}

void close_fd(int &fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

} // namespace

int main(int argc, char **argv) {
    uint16_t port = 8443;
    const char *cert_file = nullptr;
    const char *key_file = nullptr;
    if (argc == 3) {
        cert_file = argv[1];
        key_file = argv[2];
    } else if (argc == 4) {
        if (!parse_port(argv[1], port)) {
            std::cerr << "invalid port: " << argv[1] << '\n';
            return 1;
        }
        cert_file = argv[2];
        key_file = argv[3];
    } else {
        std::cerr << "usage: http3_demo_lsquic [port] <cert.pem> <key.pem>\n";
        return 1;
    }

    ServerCtx server{};
    server.port = port;
    g_server = &server;

    if (!install_signal_handlers()) {
        std::cerr << "failed to install signal handlers\n";
        return 1;
    }

    server.udp_fd = open_udp_socket(server.port);
    if (server.udp_fd < 0) {
        std::cerr << "udp bind failed: " << std::strerror(errno) << '\n';
        return 1;
    }

    if (!setup_epoll(server)) {
        std::cerr << "epoll setup failed: " << std::strerror(errno) << '\n';
        close_fd(server.udp_fd);
        return 1;
    }

    if (lsquic_global_init(LSQUIC_GLOBAL_SERVER) != 0) {
        std::cerr << "lsquic_global_init failed\n";
        close_fd(server.epoll_fd);
        close_fd(server.udp_fd);
        return 1;
    }

    server.ssl_ctx = create_server_ssl_ctx(cert_file, key_file);
    if (!server.ssl_ctx) {
        std::cerr << "SSL_CTX init failed\n";
        close_fd(server.epoll_fd);
        close_fd(server.udp_fd);
        lsquic_global_cleanup();
        return 1;
    }

    server.engine = create_engine(server);
    if (!server.engine) {
        std::cerr << "lsquic_engine_new failed\n";
        SSL_CTX_free(server.ssl_ctx);
        close_fd(server.epoll_fd);
        close_fd(server.udp_fd);
        lsquic_global_cleanup();
        return 1;
    }

    std::cout << "listening on udp://[::]:" << server.port << '\n';
    std::cout << "try: curl --http3 -k https://127.0.0.1:" << server.port << "/\n";

    std::array<epoll_event, kMaxEvents> events{};
    while (!server.stop.load(std::memory_order_acquire)) {
        int timeout_ms = compute_wait_timeout_ms(server.engine, server.tx_blocked);
        int n = ::epoll_wait(server.epoll_fd, events.data(), static_cast<int>(events.size()), timeout_ms);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "epoll_wait failed: " << std::strerror(errno) << '\n';
            break;
        }

        for (int i = 0; i < n; ++i) {
            if (events[i].data.fd != server.udp_fd) {
                continue;
            }
            uint32_t re = events[i].events;
            if (re & EPOLLIN) {
                drain_udp_and_feed_engine(server);
            }
            if (re & EPOLLOUT) {
                server.tx_blocked = false;
                lsquic_engine_send_unsent_packets(server.engine);
            }
            if (re & (EPOLLERR | EPOLLHUP)) {
                server.stop.store(true, std::memory_order_release);
            }
        }

        lsquic_engine_process_conns(server.engine);
        if (!server.tx_blocked && lsquic_engine_has_unsent_packets(server.engine)) {
            lsquic_engine_send_unsent_packets(server.engine);
        }
        if (lsquic_engine_has_unsent_packets(server.engine)) {
            server.tx_blocked = true;
        }

        uint32_t wanted = EPOLLIN | (server.tx_blocked ? EPOLLOUT : 0U);
        if (!update_epoll_interest(server, wanted)) {
            std::cerr << "failed to update epoll events: " << std::strerror(errno) << '\n';
            break;
        }
    }

    lsquic_engine_destroy(server.engine);
    SSL_CTX_free(server.ssl_ctx);
    lsquic_global_cleanup();
    close_fd(server.epoll_fd);
    close_fd(server.udp_fd);
    g_server = nullptr;
    return 0;
}
