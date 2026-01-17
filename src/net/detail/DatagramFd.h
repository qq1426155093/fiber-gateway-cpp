#ifndef FIBER_NET_DETAIL_DATAGRAM_FD_H
#define FIBER_NET_DETAIL_DATAGRAM_FD_H

#include <coroutine>
#include <cstddef>
#include "../../common/Assert.h"
#include "../../common/IoError.h"
#include "../../common/NonCopyable.h"
#include "../../common/NonMovable.h"
#include "../../event/EventLoop.h"
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

private:
    friend class RecvFromAwaiter;
    friend class SendToAwaiter;

    struct DatagramItem : fiber::event::Poller::Item {
        DatagramFd *socket = nullptr;
    };

    struct WaiterBase {
        fiber::event::IoEvent event_{};
        fiber::common::IoErr err_{fiber::common::IoErr::None};
        std::coroutine_handle<> coro_ = nullptr;
    };

    struct LocalThreadWaiter : WaiterBase {};

    template<fiber::event::IoEvent Event>
    LocalThreadWaiter *&waiter_slot() noexcept {
        if constexpr (Event == fiber::event::IoEvent::Read) {
            return read_waiter_;
        }
        return write_waiter_;
    }

    template<fiber::event::IoEvent Event>
    fiber::common::IoErr begin_event(LocalThreadWaiter *waiter) noexcept {
        static_assert(Event == fiber::event::IoEvent::Read || Event == fiber::event::IoEvent::Write);
        FIBER_ASSERT(loop_.in_loop());
        FIBER_ASSERT(waiter);
        if (fd_ < 0) {
            return fiber::common::IoErr::BadFd;
        }

        FIBER_ASSERT((watching_ & Event) == fiber::event::IoEvent::None);
        fiber::event::IoEvent desired = watching_ | Event;

        fiber::common::IoErr rc = fiber::common::IoErr::None;
        if (!registered_) {
            rc = loop_.poller().add(fd_, desired, &item_, fiber::event::Poller::Mode::OneShot);
            if (rc == fiber::common::IoErr::None) {
                registered_ = true;
            }
        } else {
            rc = loop_.poller().mod(fd_, desired, &item_, fiber::event::Poller::Mode::OneShot);
        }
        if (rc != fiber::common::IoErr::None) {
            return rc;
        }
        watching_ = desired;
        auto &slot = waiter_slot<Event>();
        FIBER_ASSERT(slot == nullptr);
        slot = waiter;
        return fiber::common::IoErr::None;
    }

    template<fiber::event::IoEvent Event>
    fiber::common::IoErr cancel_event(LocalThreadWaiter *waiter) noexcept {
        static_assert(Event == fiber::event::IoEvent::Read || Event == fiber::event::IoEvent::Write);
        FIBER_ASSERT(loop_.in_loop());
        FIBER_ASSERT(waiter);
        auto &slot = waiter_slot<Event>();
        FIBER_ASSERT(static_cast<void *>(slot) == static_cast<void *>(waiter));
        slot = nullptr;
        fiber::event::IoEvent desired = watching_ & ~Event;
        if (desired == watching_) {
            return fiber::common::IoErr::None;
        }
        fiber::common::IoErr io_err = fiber::common::IoErr::None;
        if (registered_ && desired != fiber::event::IoEvent::None) {
            io_err = loop_.poller().mod(fd_, desired, &item_, fiber::event::Poller::Mode::OneShot);
        }
        watching_ = desired;
        return io_err;
    }

    fiber::common::IoErr recv_from_once(void *buf, size_t len, SocketAddress &peer, size_t &out);
    fiber::common::IoErr send_to_once(const void *buf, size_t len, const SocketAddress &peer, size_t &out);

    static void on_events(fiber::event::Poller::Item *item, int fd, fiber::event::IoEvent events);
    void handle_events(fiber::event::IoEvent events);

    static constexpr int kInvalidFd = -1;

    fiber::event::EventLoop &loop_;
    DatagramItem item_{};
    int fd_ = kInvalidFd;
    fiber::event::IoEvent watching_ = fiber::event::IoEvent::None;
    bool registered_ = false;

    LocalThreadWaiter *read_waiter_ = nullptr;
    LocalThreadWaiter *write_waiter_ = nullptr;
};

class DatagramFd::RecvFromAwaiter : public LocalThreadWaiter {
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
    bool waiting_ = false;
    size_t result_ = 0;
    bool completed_ = false;
};

class DatagramFd::SendToAwaiter : public LocalThreadWaiter {
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
    bool waiting_ = false;
    size_t result_ = 0;
    bool completed_ = false;
};

} // namespace fiber::net::detail

#endif // FIBER_NET_DETAIL_DATAGRAM_FD_H
