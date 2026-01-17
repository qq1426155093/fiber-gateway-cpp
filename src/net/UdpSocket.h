#ifndef FIBER_NET_UDP_SOCKET_H
#define FIBER_NET_UDP_SOCKET_H

#include <cstddef>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../event/EventLoop.h"
#include "SocketAddress.h"

namespace fiber::net {

struct UdpBindOptions {
    bool reuse_addr = true;
    bool reuse_port = false;
    bool v6_only = false;
};

struct UdpRecvResult {
    size_t size = 0;
    SocketAddress peer{};
};

} // namespace fiber::net

#include "detail/DatagramFd.h"

namespace fiber::net {

class UdpSocket : public common::NonCopyable, public common::NonMovable {
public:
    using RecvFromAwaiter = detail::DatagramFd::RecvFromAwaiter;
    using SendToAwaiter = detail::DatagramFd::SendToAwaiter;

    explicit UdpSocket(fiber::event::EventLoop &loop);
    ~UdpSocket();

    fiber::common::IoResult<void> bind(const SocketAddress &addr,
                                       const UdpBindOptions &options);
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] int fd() const noexcept;
    void close();

    [[nodiscard]] RecvFromAwaiter recv_from(void *buf, size_t len) noexcept;
    [[nodiscard]] SendToAwaiter send_to(const void *buf, size_t len,
                                        const SocketAddress &peer) noexcept;

private:
    detail::DatagramFd socket_;
};

} // namespace fiber::net

#endif // FIBER_NET_UDP_SOCKET_H
