#ifndef FIBER_NET_DETAIL_TLS_STREAM_FD_H
#define FIBER_NET_DETAIL_TLS_STREAM_FD_H

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <string>

#include "../../common/IoError.h"
#include "../../common/NonCopyable.h"
#include "../../common/NonMovable.h"
#include "../../event/EventLoop.h"

struct ssl_ctx_st;
typedef struct ssl_ctx_st SSL_CTX;
struct ssl_st;
typedef struct ssl_st SSL;

namespace fiber::net::detail {

class TlsStreamFd;

using TlsOpFn = fiber::common::IoErr (*)(TlsStreamFd &, void *, fiber::event::IoEvent &);

struct TlsWaiterBase {
    TlsStreamFd *stream_ = nullptr;
    fiber::event::IoEvent event_{};
    fiber::common::IoErr err_{fiber::common::IoErr::None};
    std::coroutine_handle<> coro_ = nullptr;
    void *op_ctx_ = nullptr;
    TlsOpFn op_ = nullptr;
};

struct TlsLocalThreadWaiter : TlsWaiterBase {};
struct TlsCrossThreadWaiter;

enum class TlsWaiterState : std::uint8_t {
    Notify_Watch,
    Notify_Resume,
    Watching_Event,
    Request_Cancel,
    Waiting_Cancel,
    Canceled,
};

class TlsStreamFd : public common::NonCopyable, public common::NonMovable {
public:
    class ReadAwaiter;
    class WriteAwaiter;
    class HandshakeAwaiter;
    class ShutdownAwaiter;

    TlsStreamFd(fiber::event::EventLoop &loop, int fd, bool owns_fd = false);
    ~TlsStreamFd();

    common::IoResult<void> init(SSL_CTX *ctx, bool is_server);

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] std::string selected_alpn() const noexcept;
    void close();

    [[nodiscard]] ReadAwaiter read(void *buf, size_t len) noexcept;
    [[nodiscard]] WriteAwaiter write(const void *buf, size_t len) noexcept;
    [[nodiscard]] HandshakeAwaiter handshake() noexcept;
    [[nodiscard]] ShutdownAwaiter shutdown() noexcept;

private:
    friend struct TlsCrossThreadWaiter;

    struct StreamItem : fiber::event::Poller::Item {
        TlsStreamFd *stream = nullptr;
    };

    struct StartResult {
        bool waiting = false;
        fiber::common::IoErr err = fiber::common::IoErr::None;
    };

    StartResult start_op(TlsWaiterBase *waiter, bool local) noexcept;
    void complete_wait(TlsWaiterBase *waiter, fiber::common::IoErr err) noexcept;
    fiber::common::IoErr begin_event(TlsWaiterBase *waiter, fiber::event::IoEvent event, bool local) noexcept;
    fiber::common::IoErr arm_event(TlsWaiterBase *waiter, fiber::event::IoEvent event) noexcept;
    fiber::common::IoErr cancel_event(TlsWaiterBase *waiter) noexcept;

    fiber::common::IoErr handshake_once(fiber::event::IoEvent &event) noexcept;
    fiber::common::IoErr shutdown_once(fiber::event::IoEvent &event) noexcept;
    fiber::common::IoErr read_once(void *buf, size_t len, size_t &out, fiber::event::IoEvent &event) noexcept;
    fiber::common::IoErr write_once(const void *buf, size_t len, size_t &out, fiber::event::IoEvent &event) noexcept;

    static void on_events(fiber::event::Poller::Item *item, int fd, fiber::event::IoEvent events);
    void handle_events(fiber::event::IoEvent events);

    fiber::event::EventLoop &loop_;
    StreamItem item_{};
    int fd_ = -1;
    bool owns_fd_ = false;
    fiber::event::IoEvent watching_ = fiber::event::IoEvent::None;
    bool registered_ = false;
    bool busy_ = false;
    bool local_waiting_ = false;
    union {
        TlsLocalThreadWaiter *local_waiter_ = nullptr;
        TlsCrossThreadWaiter *cross_waiter_;
    };
    SSL *ssl_ = nullptr;
    bool handshake_done_ = false;
};

struct TlsCrossThreadWaiter : TlsWaiterBase {
    fiber::event::EventLoop *loop_ = nullptr;
    fiber::event::EventLoop::NotifyEntry notify_entry_{};
    fiber::event::EventLoop::NotifyEntry cancel_entry_{};
    std::atomic<TlsWaiterState> state_{TlsWaiterState::Notify_Watch};

    void cancel_wait() noexcept;

    static void do_notify_resume(TlsCrossThreadWaiter *waiter) noexcept;
    static void on_notify_watch(TlsCrossThreadWaiter *waiter);
    static void on_notify_cancel(TlsCrossThreadWaiter *waiter);
    static void on_notify_resume(TlsCrossThreadWaiter *waiter);
};

class TlsStreamFd::ReadAwaiter : public TlsLocalThreadWaiter {
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
    static fiber::common::IoErr do_op(TlsStreamFd &stream, void *ctx, fiber::event::IoEvent &event);

    void *buf_ = nullptr;
    size_t len_ = 0;
    size_t result_ = 0;
    bool waiting_ = false;
    bool completed_ = false;
    TlsCrossThreadWaiter *waiter_ = nullptr;
};

class TlsStreamFd::WriteAwaiter : public TlsLocalThreadWaiter {
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
    static fiber::common::IoErr do_op(TlsStreamFd &stream, void *ctx, fiber::event::IoEvent &event);

    const void *buf_ = nullptr;
    size_t len_ = 0;
    size_t result_ = 0;
    bool waiting_ = false;
    bool completed_ = false;
    TlsCrossThreadWaiter *waiter_ = nullptr;
};

class TlsStreamFd::HandshakeAwaiter : public TlsLocalThreadWaiter {
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
    static fiber::common::IoErr do_op(TlsStreamFd &stream, void *ctx, fiber::event::IoEvent &event);

    bool waiting_ = false;
    bool completed_ = false;
    TlsCrossThreadWaiter *waiter_ = nullptr;
};

class TlsStreamFd::ShutdownAwaiter : public TlsLocalThreadWaiter {
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
    static fiber::common::IoErr do_op(TlsStreamFd &stream, void *ctx, fiber::event::IoEvent &event);

    bool waiting_ = false;
    bool completed_ = false;
    TlsCrossThreadWaiter *waiter_ = nullptr;
};

} // namespace fiber::net::detail

#endif // FIBER_NET_DETAIL_TLS_STREAM_FD_H
