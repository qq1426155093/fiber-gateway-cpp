#ifndef FIBER_NET_UNIX_STREAM_H
#define FIBER_NET_UNIX_STREAM_H

#include <cstddef>
#include <sys/uio.h>
#include <utility>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../event/EventLoop.h"
#include "UnixAddress.h"
#include "detail/ConnectFd.h"
#include "detail/StreamFd.h"

namespace fiber::net {

class UnixStream;

struct UnixConnectTraits {
    using Address = UnixAddress;

    static fiber::common::IoResult<int> create_socket(const Address &peer);
    static fiber::common::IoErr connect_once(int fd, const Address &peer);
};

class UnixStream : public common::NonCopyable, public common::NonMovable {
public:
    using ReadAwaiter = detail::StreamFd::ReadAwaiter;
    using WriteAwaiter = detail::StreamFd::WriteAwaiter;
    using ReadvAwaiter = detail::StreamFd::ReadvAwaiter;
    using WritevAwaiter = detail::StreamFd::WritevAwaiter;
    using ConnectAwaiter = detail::ConnectFd<UnixConnectTraits>::ConnectAwaiter;
    using ConnectInfant = detail::StreamInfant<UnixConnectTraits>;

    UnixStream(fiber::event::EventLoop &loop, int fd);
    UnixStream(fiber::event::EventLoop &loop, int fd, UnixAddress peer);
    UnixStream(ConnectInfant &&infant);
    ~UnixStream();

    [[nodiscard]] static ConnectAwaiter connect(fiber::event::EventLoop &loop, const UnixAddress &peer) noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] const UnixAddress &remote_addr() const noexcept;
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
    UnixAddress remote_addr_ = UnixAddress::unnamed();
};

} // namespace fiber::net

#endif // FIBER_NET_UNIX_STREAM_H
