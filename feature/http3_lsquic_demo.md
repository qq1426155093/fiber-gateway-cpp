# HTTP/3 Demo Server Design (lsquic + sendmsg/recvmsg)

## Scope
- Build a standalone HTTP/3 demo server based on `lsquic`.
- Do not use project abstractions like `EventLoop`, `UdpSocket`, `Http1Server`.
- Use Linux socket APIs directly: `socket`, `setsockopt`, `bind`, `epoll_wait`, `recvmsg`, `sendmsg`.
- Keep it single-threaded and minimal.

## Target Behavior
- Listen on UDP (default `:8443`).
- Accept HTTP/3 requests.
- Return fixed response:
  - status: `200`
  - content-type: `text/plain`
  - body: `hello h3 via lsquic\n`
- Graceful stop on `SIGINT` / `SIGTERM`.

## Suggested Files
- `example/http3_demo_lsquic.cpp`
- Optional helper header if needed:
  - `example/http3_demo_lsquic_ssl.h`

## Core Runtime Objects
- `ServerCtx`
  - `int udp_fd`
  - `int epoll_fd`
  - `lsquic_engine_t* engine`
  - `SSL_CTX* ssl_ctx`
  - `std::atomic<bool> stop`
- `ConnCtx`
  - per-connection bookkeeping (optional for demo)
- `StreamCtx`
  - request parse buffer
  - response state (`headers_sent`, `body_off`, `done`)

## Socket Setup
1. `socket(AF_INET6, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)`
2. `setsockopt`:
- `SO_REUSEADDR=1`
- `SO_REUSEPORT=1` (optional)
- `IPV6_V6ONLY=0` for dual stack (optional)
- `IP_PKTINFO=1` and `IPV6_RECVPKTINFO=1` to learn local destination address
- `IP_RECVTOS=1` and `IPV6_RECVTCLASS=1` for ECN bits
3. `bind()` to `::` and chosen port.
4. Create epoll and register `udp_fd` with `EPOLLIN`.

## lsquic Initialization
1. `lsquic_global_init(LSQUIC_GLOBAL_SERVER)`
2. Build `SSL_CTX` (BoringSSL/OpenSSL API):
- load cert and key
- configure ALPN for h3
3. Fill `lsquic_engine_api`:
- `ea_packets_out = packets_out_cb`
- `ea_packets_out_ctx = &server_ctx`
- `ea_stream_if = &stream_if`
- `ea_stream_if_ctx = &server_ctx`
- `ea_get_ssl_ctx = get_ssl_ctx_cb`
- `ea_settings = &settings`
4. `lsquic_engine_init_settings(&settings, LSENG_SERVER | LSENG_HTTP)`
5. Tune minimal settings for demo (idle timeout, max streams, etc.).
6. `lsquic_engine_new(LSENG_SERVER | LSENG_HTTP, &api)`

## Stream Callback Contract
Use `lsquic_stream_if` callbacks:
- `on_new_conn`: allocate `ConnCtx`.
- `on_conn_closed`: release `ConnCtx`.
- `on_new_stream`: allocate `StreamCtx`, enable read.
- `on_read`: read request bytes, parse headers end (`\r\n\r\n`), switch to write.
- `on_write`: send headers once, send body, then close stream write side.
- `on_close`: free `StreamCtx`.

For a pure demo parser, parse only request line from the synthesized HTTP/1-style header block and ignore the rest.

## RX Path (recvmsg -> packet_in)
For each readable event:
1. Loop `recvmsg` until `EAGAIN`.
2. Capture:
- peer address from `msg_name`
- local destination address from `IP_PKTINFO` / `IPV6_PKTINFO`
- ECN from `IP_TOS` / `IPV6_TCLASS`
3. Feed packet:
- `lsquic_engine_packet_in(engine, buf, nread, &local_sa, &peer_sa, peer_ctx, ecn)`
4. After batch input, call `lsquic_engine_process_conns(engine)`.

## TX Path (packets_out -> sendmsg)
`packets_out_cb` receives an array of `lsquic_out_spec` from lsquic.

For each spec:
1. Build `msghdr` with `iovec[]` from spec payload.
2. Set `msg_name` to peer address (`dest_sa` / equivalent).
3. Add control messages:
- local source address via `IP_PKTINFO` / `IPV6_PKTINFO`
- ECN class via `IP_TOS` / `IPV6_TCLASS` if provided
4. Call `sendmsg(udp_fd, &msg, MSG_DONTWAIT)`.
5. On `EAGAIN`, stop and return number already sent.
6. On hard error, either drop packet (demo) or log and continue.

## Event Loop
Single-thread loop:
1. Query next lsquic deadline:
- `lsquic_engine_earliest_adv_tick(engine, &diff_us)` (or current API equivalent)
2. Convert to epoll timeout.
3. `epoll_wait(epoll_fd, ...)`
4. If UDP readable:
- drain RX with `recvmsg`
- call `lsquic_engine_process_conns(engine)`
5. Even without RX events, call `lsquic_engine_process_conns(engine)` when timeout fires.
6. Exit when `stop=true`.

## Minimal Pseudocode
```cpp
int main(int argc, char** argv) {
    ServerCtx s{};
    parse_args(argc, argv, s);
    install_signal_handlers(&s.stop);

    s.udp_fd = make_udp_socket_and_bind(s.bind_addr);
    s.epoll_fd = make_epoll_and_add_udp(s.udp_fd);

    lsquic_global_init(LSQUIC_GLOBAL_SERVER);
    s.ssl_ctx = make_ssl_ctx(s.cert_path, s.key_path);
    s.engine = make_lsquic_engine(&s);

    while (!s.stop.load(std::memory_order_relaxed)) {
        int timeout_ms = compute_timeout_ms_from_lsquic(s.engine);
        int n = epoll_wait(s.epoll_fd, evs, kMaxEvents, timeout_ms);
        if (n > 0 && udp_readable(evs, n, s.udp_fd)) {
            drain_udp_with_recvmsg_and_packet_in(&s);
        }
        lsquic_engine_process_conns(s.engine);
    }

    lsquic_engine_destroy(s.engine);
    SSL_CTX_free(s.ssl_ctx);
    lsquic_global_cleanup();
    close(s.epoll_fd);
    close(s.udp_fd);
    return 0;
}
```

## sendmsg Helper Sketch
```cpp
static int packets_out_cb(void* ctx,
                          const struct lsquic_out_spec* specs,
                          unsigned n_specs) {
    auto* s = static_cast<ServerCtx*>(ctx);
    unsigned sent = 0;

    for (unsigned i = 0; i < n_specs; ++i) {
        msghdr msg{};
        iovec iov[LSQUIC_MAX_OUT_IOVS];
        char cbuf[128];

        fill_iov_from_spec(specs[i], iov, &msg);
        fill_name_from_spec(specs[i], &msg);
        fill_cmsgs_for_pktinfo_and_ecn(specs[i], cbuf, sizeof(cbuf), &msg);

        ssize_t rc = ::sendmsg(s->udp_fd, &msg, MSG_DONTWAIT);
        if (rc < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            continue;  // demo: log and drop
        }
        ++sent;
    }

    return static_cast<int>(sent);
}
```

## Request/Response Handling Sketch
```cpp
static void on_read(lsquic_stream_t* stream, lsquic_stream_ctx_t* h) {
    auto* st = static_cast<StreamCtx*>(h);
    char buf[4096];

    for (;;) {
        ssize_t n = lsquic_stream_read(stream, buf, sizeof(buf));
        if (n > 0) {
            st->req.append(buf, static_cast<size_t>(n));
            if (has_complete_headers(st->req)) {
                st->ready_to_respond = true;
                lsquic_stream_wantread(stream, 0);
                lsquic_stream_wantwrite(stream, 1);
                return;
            }
            continue;
        }
        if (n == 0) {
            break;
        }
        if (errno == EWOULDBLOCK) {
            return;
        }
        lsquic_stream_close(stream);
        return;
    }
}

static void on_write(lsquic_stream_t* stream, lsquic_stream_ctx_t* h) {
    auto* st = static_cast<StreamCtx*>(h);
    if (!st->headers_sent) {
        send_status_and_headers(stream, 200, "text/plain", kBodyLen);
        st->headers_sent = true;
    }

    while (st->body_off < kBodyLen) {
        ssize_t n = lsquic_stream_write(stream, kBody + st->body_off, kBodyLen - st->body_off);
        if (n > 0) {
            st->body_off += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EWOULDBLOCK) {
            return;
        }
        lsquic_stream_close(stream);
        return;
    }

    lsquic_stream_shutdown(stream, 1);
    lsquic_stream_wantwrite(stream, 0);
}
```

## Build Integration
- Keep demo independent from `fiber_lib` internals.
- In `CMakeLists.txt`, for `http3_demo_lsquic` only:
- link `lsquic::lsquic`
- link `boringssl::ssl` and `boringssl::crypto` if lsquic target does not propagate them
- keep existing examples untouched

## Validation Checklist
1. Start server:
- `./build/http3_demo_lsquic 8443 cert.pem key.pem`
2. Client check:
- `curl --http3 -k https://127.0.0.1:8443/`
3. Confirm body and status.
4. Run `tcpdump -i lo udp port 8443` to verify UDP traffic.
5. Kill with `Ctrl+C` and confirm clean shutdown.

## Notes
- For production, replace naive request parsing with lsquic header-set interface.
- Add retries/queueing on TX `EAGAIN` instead of packet drop.
- Consider `recvmmsg`/`sendmmsg` for throughput once demo is stable.
