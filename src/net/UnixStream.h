#ifndef FIBER_NET_UNIX_STREAM_H
#define FIBER_NET_UNIX_STREAM_H

#include <cstddef>
#include <sys/uio.h>
#include <utility>

#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../event/EventLoop.h"
#include "UnixAddress.h"
#include "detail/StreamFd.h"

namespace fiber::net {

class UnixStream : public common::NonCopyable, public common::NonMovable {
public:
    using ReadAwaiter = detail::StreamFd::ReadAwaiter;
    using WriteAwaiter = detail::StreamFd::WriteAwaiter;
    using ReadvAwaiter = detail::StreamFd::ReadvAwaiter;
    using WritevAwaiter = detail::StreamFd::WritevAwaiter;

    UnixStream(fiber::event::EventLoop &loop, int fd);
    UnixStream(fiber::event::EventLoop &loop, int fd, UnixAddress peer);
    ~UnixStream();

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] const UnixAddress &remote_addr() const noexcept;
    void close();

    [[nodiscard]] ReadAwaiter read(void *buf, size_t len) noexcept;
    [[nodiscard]] WriteAwaiter write(const void *buf, size_t len) noexcept;
    [[nodiscard]] ReadvAwaiter readv(const struct iovec *iov, int iovcnt) noexcept;
    [[nodiscard]] WritevAwaiter writev(const struct iovec *iov, int iovcnt) noexcept;

private:
    detail::StreamFd stream_;
    UnixAddress remote_addr_ = UnixAddress::unnamed();
};

} // namespace fiber::net

#endif // FIBER_NET_UNIX_STREAM_H
