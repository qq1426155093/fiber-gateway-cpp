#ifndef FIBER_NET_TCP_STREAM_H
#define FIBER_NET_TCP_STREAM_H

#include <cstddef>

#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../event/EventLoop.h"
#include "detail/StreamFd.h"

namespace fiber::net {

class TcpStream : public common::NonCopyable, public common::NonMovable {
public:
    using ReadAwaiter = detail::StreamFd::ReadAwaiter;
    using WriteAwaiter = detail::StreamFd::WriteAwaiter;

    TcpStream(fiber::event::EventLoop &loop, int fd);
    ~TcpStream();

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] int fd() const noexcept;
    void close();

    [[nodiscard]] ReadAwaiter read(void *buf, size_t len) noexcept;
    [[nodiscard]] WriteAwaiter write(const void *buf, size_t len) noexcept;

private:
    detail::StreamFd stream_;
};

} // namespace fiber::net

#endif // FIBER_NET_TCP_STREAM_H
