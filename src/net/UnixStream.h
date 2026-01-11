#ifndef FIBER_NET_UNIX_STREAM_H
#define FIBER_NET_UNIX_STREAM_H

#include <cstddef>

#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../event/EventLoop.h"
#include "detail/StreamFd.h"

namespace fiber::net {

class UnixStream : public common::NonCopyable, public common::NonMovable {
public:
    using ReadAwaiter = detail::StreamFd::ReadAwaiter;
    using WriteAwaiter = detail::StreamFd::WriteAwaiter;

    UnixStream(fiber::event::EventLoop &loop, int fd);
    ~UnixStream();

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] int fd() const noexcept;
    void close();

    [[nodiscard]] ReadAwaiter read(void *buf, size_t len) noexcept;
    [[nodiscard]] WriteAwaiter write(const void *buf, size_t len) noexcept;

private:
    detail::StreamFd stream_;
};

} // namespace fiber::net

#endif // FIBER_NET_UNIX_STREAM_H
