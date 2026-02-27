#include "StreamFd.h"

#include <cerrno>
#include <sys/socket.h>
#include <sys/uio.h>

#include "../../common/Assert.h"

namespace fiber::net::detail {

StreamFd::StreamFd(fiber::event::EventLoop &loop, int fd) : rwfd_(loop, fd) {}

StreamFd::~StreamFd() {
    if (!rwfd_.valid()) {
        return;
    }
    if (rwfd_.loop().in_loop()) {
        close();
        return;
    }
    FIBER_ASSERT(false);
}

bool StreamFd::valid() const noexcept { return rwfd_.valid(); }

int StreamFd::fd() const noexcept { return rwfd_.fd(); }

fiber::event::EventLoop &StreamFd::loop() const noexcept { return rwfd_.loop(); }

RWFd &StreamFd::rwfd() noexcept { return rwfd_; }

int StreamFd::release_fd() noexcept { return rwfd_.release_fd(); }

void StreamFd::close() { rwfd_.close(); }

StreamFd::ReadAwaiter StreamFd::read(void *buf, size_t len) noexcept { return {*this, buf, len}; }

StreamFd::WriteAwaiter StreamFd::write(const void *buf, size_t len) noexcept { return {*this, buf, len}; }

StreamFd::ReadvAwaiter StreamFd::readv(const struct iovec *iov, int iovcnt) noexcept { return {*this, iov, iovcnt}; }

StreamFd::WritevAwaiter StreamFd::writev(const struct iovec *iov, int iovcnt) noexcept { return {*this, iov, iovcnt}; }

StreamFd::WaitReadableAwaiter StreamFd::wait_readable() noexcept { return rwfd_.wait_readable(); }

StreamFd::WaitWritableAwaiter StreamFd::wait_writable() noexcept { return rwfd_.wait_writable(); }

fiber::common::IoResult<size_t> StreamFd::try_read(void *buf, size_t len) noexcept {
    size_t out = 0;
    fiber::common::IoErr err = read_once(buf, len, out);
    if (err == fiber::common::IoErr::None) {
        return out;
    }
    return std::unexpected(err);
}

fiber::common::IoResult<size_t> StreamFd::try_write(const void *buf, size_t len) noexcept {
    size_t out = 0;
    fiber::common::IoErr err = write_once(buf, len, out);
    if (err == fiber::common::IoErr::None) {
        return out;
    }
    return std::unexpected(err);
}

fiber::common::IoResult<size_t> StreamFd::try_readv(const struct iovec *iov, int iovcnt) noexcept {
    size_t out = 0;
    fiber::common::IoErr err = readv_once(iov, iovcnt, out);
    if (err == fiber::common::IoErr::None) {
        return out;
    }
    return std::unexpected(err);
}

fiber::common::IoResult<size_t> StreamFd::try_writev(const struct iovec *iov, int iovcnt) noexcept {
    size_t out = 0;
    fiber::common::IoErr err = writev_once(iov, iovcnt, out);
    if (err == fiber::common::IoErr::None) {
        return out;
    }
    return std::unexpected(err);
}

fiber::common::IoErr StreamFd::read_once(void *buf, size_t len, size_t &out) {
    out = 0;
    int socket_fd = rwfd_.fd();
    if (socket_fd < 0) {
        return fiber::common::IoErr::BadFd;
    }
    for (;;) {
        ssize_t rc = ::recv(socket_fd, buf, len, 0);
        if (rc >= 0) {
            out = static_cast<size_t>(rc);
            return fiber::common::IoErr::None;
        }
        int err = errno;
        if (err == EINTR) {
            continue;
        }
        if (err == EAGAIN || err == EWOULDBLOCK) {
            return fiber::common::IoErr::WouldBlock;
        }
        return fiber::common::io_err_from_errno(err);
    }
}

fiber::common::IoErr StreamFd::write_once(const void *buf, size_t len, size_t &out) {
    out = 0;
    int socket_fd = rwfd_.fd();
    if (socket_fd < 0) {
        return fiber::common::IoErr::BadFd;
    }
    for (;;) {
        ssize_t rc = ::send(socket_fd, buf, len, 0);
        if (rc >= 0) {
            out = static_cast<size_t>(rc);
            return fiber::common::IoErr::None;
        }
        int err = errno;
        if (err == EINTR) {
            continue;
        }
        if (err == EAGAIN || err == EWOULDBLOCK) {
            return fiber::common::IoErr::WouldBlock;
        }
        return fiber::common::io_err_from_errno(err);
    }
}

fiber::common::IoErr StreamFd::readv_once(const struct iovec *iov, int iovcnt, size_t &out) {
    out = 0;
    int socket_fd = rwfd_.fd();
    if (socket_fd < 0) {
        return fiber::common::IoErr::BadFd;
    }
    for (;;) {
        ssize_t rc = ::readv(socket_fd, iov, iovcnt);
        if (rc >= 0) {
            out = static_cast<size_t>(rc);
            return fiber::common::IoErr::None;
        }
        int err = errno;
        if (err == EINTR) {
            continue;
        }
        if (err == EAGAIN || err == EWOULDBLOCK) {
            return fiber::common::IoErr::WouldBlock;
        }
        return fiber::common::io_err_from_errno(err);
    }
}

fiber::common::IoErr StreamFd::writev_once(const struct iovec *iov, int iovcnt, size_t &out) {
    out = 0;
    int socket_fd = rwfd_.fd();
    if (socket_fd < 0) {
        return fiber::common::IoErr::BadFd;
    }
    for (;;) {
        ssize_t rc = ::writev(socket_fd, iov, iovcnt);
        if (rc >= 0) {
            out = static_cast<size_t>(rc);
            return fiber::common::IoErr::None;
        }
        int err = errno;
        if (err == EINTR) {
            continue;
        }
        if (err == EAGAIN || err == EWOULDBLOCK) {
            return fiber::common::IoErr::WouldBlock;
        }
        return fiber::common::io_err_from_errno(err);
    }
}

template<typename Op>
StreamFd::ReadWriteAwaiter<Op>::~ReadWriteAwaiter() = default;

template<typename Op>
bool StreamFd::ReadWriteAwaiter<Op>::await_suspend(std::coroutine_handle<> handle) {
    err_ = fiber::common::IoErr::None;
    completed_ = false;

    size_t out = 0;
    fiber::common::IoErr err = op_.once(*stream_, out);
    if (err == fiber::common::IoErr::None) {
        result_ = out;
        completed_ = true;
        return false;
    }
    if (err != fiber::common::IoErr::WouldBlock) {
        err_ = err;
        completed_ = true;
        return false;
    }

    waiting_ = true;
    waiter_.emplace(stream_->rwfd_);
    return waiter_->await_suspend(handle);
}

template<typename Op>
fiber::common::IoResult<size_t> StreamFd::ReadWriteAwaiter<Op>::await_resume() noexcept {
    waiting_ = false;
    if (completed_) {
        completed_ = false;
        if (err_ == fiber::common::IoErr::None) {
            return result_;
        }
        return std::unexpected(err_);
    }

    if (waiter_) {
        fiber::common::IoResult<void> wait_result = waiter_->await_resume();
        waiter_.reset();
        if (!wait_result) {
            return std::unexpected(wait_result.error());
        }
    }

    size_t out = 0;
    fiber::common::IoErr err = op_.once(*stream_, out);
    if (err == fiber::common::IoErr::None) {
        return out;
    }
    return std::unexpected(err);
}

template class StreamFd::ReadWriteAwaiter<StreamFd::ReadOp>;
template class StreamFd::ReadWriteAwaiter<StreamFd::WriteOp>;
template class StreamFd::ReadWriteAwaiter<StreamFd::ReadvOp>;
template class StreamFd::ReadWriteAwaiter<StreamFd::WritevOp>;

} // namespace fiber::net::detail
