#ifndef FIBER_NET_DETAIL_DATAGRAM_FD_H
#define FIBER_NET_DETAIL_DATAGRAM_FD_H

#include <coroutine>
#include <cstddef>
#include <optional>
#include "../../common/Assert.h"
#include "../../common/IoError.h"
#include "../../common/NonCopyable.h"
#include "../../common/NonMovable.h"
#include "../../event/EventLoop.h"
#include "RWFd.h"
#include "../SocketAddress.h"

namespace fiber::net {

struct UdpBindOptions;
struct UdpRecvResult;

} // namespace fiber::net

namespace fiber::net::detail {

class DatagramFd : public common::NonCopyable, public common::NonMovable {
public:
    class RecvFromAwaiter;
    class SendToAwaiter;

    explicit DatagramFd(fiber::event::EventLoop &loop);
    ~DatagramFd();

    fiber::common::IoResult<void> bind(const SocketAddress &addr,
                                       const UdpBindOptions &options);
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] int fd() const noexcept;
    void close();

    [[nodiscard]] RecvFromAwaiter recv_from(void *buf, size_t len) noexcept;
    [[nodiscard]] SendToAwaiter send_to(const void *buf, size_t len,
                                        const SocketAddress &peer) noexcept;
    [[nodiscard]] fiber::common::IoResult<UdpRecvResult> try_recv_from(void *buf, size_t len) noexcept;
    [[nodiscard]] fiber::common::IoResult<size_t> try_send_to(const void *buf,
                                                              size_t len,
                                                              const SocketAddress &peer) noexcept;

private:
    fiber::common::IoErr recv_from_once(void *buf, size_t len, SocketAddress &peer, size_t &out);
    fiber::common::IoErr send_to_once(const void *buf, size_t len, const SocketAddress &peer, size_t &out);

    RWFd rwfd_;
};

class DatagramFd::RecvFromAwaiter {
public:
    RecvFromAwaiter(DatagramFd &socket, void *buf, size_t len) noexcept;

    RecvFromAwaiter(const RecvFromAwaiter &) = delete;
    RecvFromAwaiter &operator=(const RecvFromAwaiter &) = delete;
    RecvFromAwaiter(RecvFromAwaiter &&) = delete;
    RecvFromAwaiter &operator=(RecvFromAwaiter &&) = delete;
    ~RecvFromAwaiter();

    bool await_ready() noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> handle);
    fiber::common::IoResult<UdpRecvResult> await_resume() noexcept;

private:
    DatagramFd *socket_ = nullptr;
    void *buf_ = nullptr;
    size_t len_ = 0;
    SocketAddress peer_{};
    fiber::common::IoErr err_{fiber::common::IoErr::None};
    std::optional<RWFd::WaitReadableAwaiter> waiter_{};
    bool waiting_ = false;
    size_t result_ = 0;
    bool completed_ = false;
};

class DatagramFd::SendToAwaiter {
public:
    SendToAwaiter(DatagramFd &socket, const void *buf, size_t len, SocketAddress peer) noexcept;

    SendToAwaiter(const SendToAwaiter &) = delete;
    SendToAwaiter &operator=(const SendToAwaiter &) = delete;
    SendToAwaiter(SendToAwaiter &&) = delete;
    SendToAwaiter &operator=(SendToAwaiter &&) = delete;
    ~SendToAwaiter();

    bool await_ready() noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> handle);
    fiber::common::IoResult<size_t> await_resume() noexcept;

private:
    DatagramFd *socket_ = nullptr;
    const void *buf_ = nullptr;
    size_t len_ = 0;
    SocketAddress peer_{};
    fiber::common::IoErr err_{fiber::common::IoErr::None};
    std::optional<RWFd::WaitWritableAwaiter> waiter_{};
    bool waiting_ = false;
    size_t result_ = 0;
    bool completed_ = false;
};

} // namespace fiber::net::detail

#endif // FIBER_NET_DETAIL_DATAGRAM_FD_H
