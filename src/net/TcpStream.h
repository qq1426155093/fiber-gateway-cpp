#ifndef FIBER_NET_TCP_STREAM_H
#define FIBER_NET_TCP_STREAM_H

#include <cstddef>
#include <sys/uio.h>
#include <utility>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../event/EventLoop.h"
#include "SocketAddress.h"
#include "detail/ConnectFd.h"
#include "detail/StreamFd.h"

namespace fiber::net {

class TcpStream;

struct TcpConnectTraits {
    using Address = SocketAddress;

    static fiber::common::IoResult<int> create_socket(const Address &peer);
    static fiber::common::IoErr connect_once(int fd, const Address &peer);
};

class TcpStream : public common::NonCopyable, public common::NonMovable {
public:
    using ReadAwaiter = detail::StreamFd::ReadAwaiter;
    using WriteAwaiter = detail::StreamFd::WriteAwaiter;
    using ReadvAwaiter = detail::StreamFd::ReadvAwaiter;
    using WritevAwaiter = detail::StreamFd::WritevAwaiter;
    using ConnectAwaiter = detail::ConnectFd<TcpConnectTraits>::ConnectAwaiter;
    using ConnectInfant = detail::StreamInfant<TcpConnectTraits>;

    TcpStream(fiber::event::EventLoop &loop, int fd);
    TcpStream(fiber::event::EventLoop &loop, int fd, SocketAddress peer);
    TcpStream(ConnectInfant &&infant);
    ~TcpStream();

    [[nodiscard]] static ConnectAwaiter connect(fiber::event::EventLoop &loop, const SocketAddress &peer) noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] fiber::event::EventLoop &loop() const noexcept;
    [[nodiscard]] const SocketAddress &remote_addr() const noexcept;
    void close();

    [[nodiscard]] ReadAwaiter read(void *buf, size_t len) noexcept;
    [[nodiscard]] WriteAwaiter write(const void *buf, size_t len) noexcept;
    [[nodiscard]] ReadvAwaiter readv(const struct iovec *iov, int iovcnt) noexcept;
    [[nodiscard]] WritevAwaiter writev(const struct iovec *iov, int iovcnt) noexcept;
    [[nodiscard]] fiber::common::IoResult<size_t> try_read(void *buf, size_t len) noexcept;
    [[nodiscard]] fiber::common::IoResult<size_t> try_write(const void *buf, size_t len) noexcept;
    [[nodiscard]] fiber::common::IoResult<size_t> try_readv(const struct iovec *iov, int iovcnt) noexcept;
    [[nodiscard]] fiber::common::IoResult<size_t> try_writev(const struct iovec *iov, int iovcnt) noexcept;

private:
    detail::StreamFd stream_;
    SocketAddress remote_addr_{};
};

} // namespace fiber::net

#endif // FIBER_NET_TCP_STREAM_H
