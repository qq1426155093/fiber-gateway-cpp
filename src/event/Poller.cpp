#include "Poller.h"

#include <cerrno>
#include <unistd.h>

namespace fiber::event {

namespace {

constexpr std::uint32_t to_mask(Poller::Event events) { return static_cast<std::uint32_t>(events); }

constexpr std::uint32_t to_mask(Poller::Mode mode) { return static_cast<std::uint32_t>(mode); }

std::uint32_t to_epoll_events(Poller::Event events, Poller::Mode mode) {
    std::uint32_t mask = 0;
    auto bits = to_mask(events);
    if (bits & to_mask(Poller::Event::Read)) {
        mask |= EPOLLIN;
    }
    if (bits & to_mask(Poller::Event::Write)) {
        mask |= EPOLLOUT;
    }
    if (mask) {
        mask |= EPOLLERR | EPOLLHUP;
        auto mode_bits = to_mask(mode);
        if (mode_bits & to_mask(Poller::Mode::Edge)) {
            mask |= EPOLLET;
        }
        if (mode_bits & to_mask(Poller::Mode::OneShot)) {
            mask |= EPOLLONESHOT;
        }
    }
    return mask;
}

} // namespace

Poller::Poller() { epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC); }

Poller::~Poller() {
    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
    }
}

bool Poller::valid() const { return epoll_fd_ >= 0; }

fiber::common::IoErr Poller::add(int fd, Event events, Item *item, Mode mode) {
    if (item) {
        item->fd_ = fd;
        item->interested_ = events;
    }
    epoll_event ev{};
    ev.events = to_epoll_events(events, mode);
    ev.data.ptr = item;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) == 0) {
        return fiber::common::IoErr::None;
    }
    return fiber::common::io_err_from_errno(errno);
}

fiber::common::IoErr Poller::mod(int fd, Event events, Item *item, Mode mode) {
    if (item) {
        item->fd_ = fd;
        item->interested_ = events;
    }
    epoll_event ev{};
    ev.events = to_epoll_events(events, mode);
    ev.data.ptr = item;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) == 0) {
        return fiber::common::IoErr::None;
    }
    return fiber::common::io_err_from_errno(errno);
}

fiber::common::IoErr Poller::del(int fd) {
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) == 0) {
        return fiber::common::IoErr::None;
    }
    return fiber::common::io_err_from_errno(errno);
}

int Poller::wait(epoll_event *events, int max_events, int timeout_ms) {
    return ::epoll_wait(epoll_fd_, events, max_events, timeout_ms);
}

} // namespace fiber::event
