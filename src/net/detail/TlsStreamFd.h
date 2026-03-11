#ifndef FIBER_NET_DETAIL_TLS_STREAM_FD_H
#define FIBER_NET_DETAIL_TLS_STREAM_FD_H

#include <coroutine>
#include <cstddef>
#include <optional>
#include <string>

#include "../../common/IoError.h"
#include "../../common/NonCopyable.h"
#include "../../common/NonMovable.h"
#include "../../event/Poller.h"
#include "StreamFd.h"

struct ssl_ctx_st;
typedef struct ssl_ctx_st SSL_CTX;
struct ssl_st;
typedef struct ssl_st SSL;

namespace fiber::net::detail {

class TlsStreamFd : public common::NonCopyable, public common::NonMovable {
public:
    class ReadAwaiter;
    class WriteAwaiter;
    class HandshakeAwaiter;
    class ShutdownAwaiter;

    TlsStreamFd(fiber::event::EventLoop &loop, int fd);
    ~TlsStreamFd();

    common::IoResult<void> init(SSL_CTX *ctx, bool is_server);

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] fiber::event::EventLoop &loop() const noexcept;
    [[nodiscard]] std::string selected_alpn() const noexcept;
    void close();

    [[nodiscard]] ReadAwaiter read(void *buf, size_t len) noexcept;
    [[nodiscard]] WriteAwaiter write(const void *buf, size_t len) noexcept;
    [[nodiscard]] fiber::common::IoResult<size_t> try_read(void *buf, size_t len) noexcept;
    [[nodiscard]] fiber::common::IoResult<size_t> try_write(const void *buf, size_t len) noexcept;
    [[nodiscard]] HandshakeAwaiter handshake() noexcept;
    [[nodiscard]] ShutdownAwaiter shutdown() noexcept;
    [[nodiscard]] StreamFd::WaitReadableAwaiter wait_readable() noexcept;
    [[nodiscard]] StreamFd::WaitWritableAwaiter wait_writable() noexcept;
    fiber::common::IoErr poll_handshake(fiber::event::IoEvent &event) noexcept;
    fiber::common::IoErr poll_shutdown(fiber::event::IoEvent &event) noexcept;
    fiber::common::IoErr poll_read(void *buf, size_t len, size_t &out, fiber::event::IoEvent &event) noexcept;
    fiber::common::IoErr poll_write(const void *buf, size_t len, size_t &out, fiber::event::IoEvent &event) noexcept;

private:
    friend class ReadAwaiter;
    friend class WriteAwaiter;
    friend class HandshakeAwaiter;
    friend class ShutdownAwaiter;

    fiber::common::IoErr handshake_once(fiber::event::IoEvent &event) noexcept;
    fiber::common::IoErr shutdown_once(fiber::event::IoEvent &event) noexcept;
    fiber::common::IoErr read_once(void *buf, size_t len, size_t &out, fiber::event::IoEvent &event) noexcept;
    fiber::common::IoErr write_once(const void *buf, size_t len, size_t &out, fiber::event::IoEvent &event) noexcept;

    StreamFd stream_fd_;
    SSL *ssl_ = nullptr;
    bool handshake_done_ = false;
    bool busy_ = false;
};

class TlsStreamFd::ReadAwaiter {
public:
    ReadAwaiter(TlsStreamFd &stream, void *buf, size_t len) noexcept;

    ReadAwaiter(const ReadAwaiter &) = delete;
    ReadAwaiter &operator=(const ReadAwaiter &) = delete;
    ReadAwaiter(ReadAwaiter &&) = delete;
    ReadAwaiter &operator=(ReadAwaiter &&) = delete;
    ~ReadAwaiter();

    bool await_ready() noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> handle);
    fiber::common::IoResult<size_t> await_resume() noexcept;

private:
    TlsStreamFd *stream_ = nullptr;
    void *buf_ = nullptr;
    size_t len_ = 0;
    size_t result_ = 0;
    fiber::common::IoErr err_ = fiber::common::IoErr::None;
    std::optional<StreamFd::WaitReadableAwaiter> read_waiter_{};
    std::optional<StreamFd::WaitWritableAwaiter> write_waiter_{};
    bool waiting_ = false;
    bool completed_ = false;
};

class TlsStreamFd::WriteAwaiter {
public:
    WriteAwaiter(TlsStreamFd &stream, const void *buf, size_t len) noexcept;

    WriteAwaiter(const WriteAwaiter &) = delete;
    WriteAwaiter &operator=(const WriteAwaiter &) = delete;
    WriteAwaiter(WriteAwaiter &&) = delete;
    WriteAwaiter &operator=(WriteAwaiter &&) = delete;
    ~WriteAwaiter();

    bool await_ready() noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> handle);
    fiber::common::IoResult<size_t> await_resume() noexcept;

private:
    TlsStreamFd *stream_ = nullptr;
    const void *buf_ = nullptr;
    size_t len_ = 0;
    size_t result_ = 0;
    fiber::common::IoErr err_ = fiber::common::IoErr::None;
    std::optional<StreamFd::WaitReadableAwaiter> read_waiter_{};
    std::optional<StreamFd::WaitWritableAwaiter> write_waiter_{};
    bool waiting_ = false;
    bool completed_ = false;
};

class TlsStreamFd::HandshakeAwaiter {
public:
    explicit HandshakeAwaiter(TlsStreamFd &stream) noexcept;

    HandshakeAwaiter(const HandshakeAwaiter &) = delete;
    HandshakeAwaiter &operator=(const HandshakeAwaiter &) = delete;
    HandshakeAwaiter(HandshakeAwaiter &&) = delete;
    HandshakeAwaiter &operator=(HandshakeAwaiter &&) = delete;
    ~HandshakeAwaiter();

    bool await_ready() noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> handle);
    fiber::common::IoResult<void> await_resume() noexcept;

private:
    TlsStreamFd *stream_ = nullptr;
    fiber::common::IoErr err_ = fiber::common::IoErr::None;
    std::optional<StreamFd::WaitReadableAwaiter> read_waiter_{};
    std::optional<StreamFd::WaitWritableAwaiter> write_waiter_{};
    bool waiting_ = false;
    bool completed_ = false;
};

class TlsStreamFd::ShutdownAwaiter {
public:
    explicit ShutdownAwaiter(TlsStreamFd &stream) noexcept;

    ShutdownAwaiter(const ShutdownAwaiter &) = delete;
    ShutdownAwaiter &operator=(const ShutdownAwaiter &) = delete;
    ShutdownAwaiter(ShutdownAwaiter &&) = delete;
    ShutdownAwaiter &operator=(ShutdownAwaiter &&) = delete;
    ~ShutdownAwaiter();

    bool await_ready() noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> handle);
    fiber::common::IoResult<void> await_resume() noexcept;

private:
    TlsStreamFd *stream_ = nullptr;
    fiber::common::IoErr err_ = fiber::common::IoErr::None;
    std::optional<StreamFd::WaitReadableAwaiter> read_waiter_{};
    std::optional<StreamFd::WaitWritableAwaiter> write_waiter_{};
    bool waiting_ = false;
    bool completed_ = false;
};

} // namespace fiber::net::detail

#endif // FIBER_NET_DETAIL_TLS_STREAM_FD_H
