#include "Efd.h"

#include <unistd.h>

namespace fiber::net::detail {

Efd::Efd(fiber::event::EventLoop &loop,
         void *owner,
         EventCallback callback,
         fiber::event::Poller::Mode mode) noexcept
    : loop_(loop), owner_(owner), callback_(callback), mode_(mode) {
    item_.efd = this;
    item_.callback = &Efd::on_poller_event;
}

Efd::~Efd() {
    if (fd_ < 0) {
        return;
    }

    if (registered_) {
        if (!loop_.in_loop()) {
            FIBER_ASSERT(false);
            return;
        }
        close_fd();
        return;
    }

    int fd = fd_;
    fd_ = -1;
    watching_ = fiber::event::IoEvent::None;
    ::close(fd);
}

void Efd::set_owner(void *owner, EventCallback callback) noexcept {
    owner_ = owner;
    callback_ = callback;
}

fiber::common::IoErr Efd::attach(int fd) noexcept {
    if (fd_ >= 0) {
        return fiber::common::IoErr::Already;
    }
    if (fd < 0) {
        return fiber::common::IoErr::Invalid;
    }
    FIBER_ASSERT(!registered_);
    FIBER_ASSERT(watching_ == fiber::event::IoEvent::None);
    fd_ = fd;
    return fiber::common::IoErr::None;
}

int Efd::release_fd() noexcept {
    FIBER_ASSERT(!registered_);
    FIBER_ASSERT(watching_ == fiber::event::IoEvent::None);
    int fd = fd_;
    fd_ = -1;
    return fd;
}

void Efd::close_fd() noexcept {
    if (fd_ < 0) {
        return;
    }
    if (registered_) {
        FIBER_ASSERT(loop_.in_loop());
        loop_.poller().del(fd_);
        registered_ = false;
    }
    int fd = fd_;
    fd_ = -1;
    watching_ = fiber::event::IoEvent::None;
    ::close(fd);
}

fiber::common::IoErr Efd::unwatch_all() noexcept {
    if (!registered_) {
        watching_ = fiber::event::IoEvent::None;
        return fiber::common::IoErr::None;
    }
    if (fd_ < 0) {
        return fiber::common::IoErr::BadFd;
    }
    FIBER_ASSERT(loop_.in_loop());
    fiber::common::IoErr err = loop_.poller().del(fd_);
    if (err != fiber::common::IoErr::None) {
        return err;
    }
    registered_ = false;
    watching_ = fiber::event::IoEvent::None;
    return fiber::common::IoErr::None;
}

fiber::common::IoErr Efd::watch_set(fiber::event::IoEvent desired) noexcept {
    if (fd_ < 0) {
        return fiber::common::IoErr::BadFd;
    }
    FIBER_ASSERT(loop_.in_loop());

    if (desired == watching_ && (fiber::event::any(desired) || !registered_)) {
        return fiber::common::IoErr::None;
    }

    if (!fiber::event::any(desired)) {
        if (!registered_) {
            watching_ = fiber::event::IoEvent::None;
            return fiber::common::IoErr::None;
        }
        fiber::common::IoErr err = loop_.poller().del(fd_);
        if (err != fiber::common::IoErr::None) {
            return err;
        }
        registered_ = false;
        watching_ = fiber::event::IoEvent::None;
        return fiber::common::IoErr::None;
    }

    fiber::common::IoErr err = fiber::common::IoErr::None;
    if (!registered_) {
        err = loop_.poller().add(fd_, desired, &item_, mode_);
        if (err != fiber::common::IoErr::None) {
            return err;
        }
        registered_ = true;
    } else {
        err = loop_.poller().mod(fd_, desired, &item_, mode_);
        if (err != fiber::common::IoErr::None) {
            return err;
        }
    }
    watching_ = desired;
    return fiber::common::IoErr::None;
}

fiber::common::IoErr Efd::watch_add(fiber::event::IoEvent events) noexcept {
    return watch_set(watching_ | events);
}

fiber::common::IoErr Efd::watch_del(fiber::event::IoEvent events) noexcept {
    return watch_set(watching_ & ~events);
}

fiber::common::IoErr Efd::consume_ready(fiber::event::IoEvent ready) noexcept {
    if (!fiber::event::any(ready)) {
        return fiber::common::IoErr::None;
    }
    if (fd_ < 0) {
        return fiber::common::IoErr::BadFd;
    }
    FIBER_ASSERT(loop_.in_loop());

    fiber::event::IoEvent desired = watching_ & ~ready;
    if (desired == watching_) {
        return fiber::common::IoErr::None;
    }

    if (registered_ && fiber::event::any(desired)) {
        fiber::common::IoErr err = loop_.poller().mod(fd_, desired, &item_, mode_);
        if (err != fiber::common::IoErr::None) {
            return err;
        }
    }

    watching_ = desired;
    return fiber::common::IoErr::None;
}

void Efd::on_poller_event(fiber::event::Poller::Item *item, int fd, fiber::event::IoEvent events) {
    (void) fd;
    auto *efd_item = static_cast<Item *>(item);
    if (!efd_item || !efd_item->efd) {
        return;
    }
    Efd *efd = efd_item->efd;
    if (!efd->callback_) {
        return;
    }
    efd->callback_(efd->owner_, events);
}

} // namespace fiber::net::detail
