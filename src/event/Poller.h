#ifndef FIBER_EVENT_POLLER_H
#define FIBER_EVENT_POLLER_H

#include <cstdint>
#include <sys/epoll.h>
#include <type_traits>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"

namespace fiber::event {

class Poller {
public:
    enum class Event : std::uint32_t { None = 0, Read = 1u << 0, Write = 1u << 1 };
    enum class Mode : std::uint32_t { None = 0, Edge = 1u << 0, OneShot = 1u << 1 };

    struct Item : common::NonCopyable, common::NonMovable {
        using Callback = void (*)(Item *, int fd, Event);
        Callback callback{};
        int fd() const noexcept {
            return fd_;
        }
        friend class Poller;

    private:
        int fd_{};
        Event interested_{Event::None};
        friend class EventLoop;
    };

    Poller();
    ~Poller();

    Poller(const Poller &) = delete;
    Poller &operator=(const Poller &) = delete;
    Poller(Poller &&) = delete;
    Poller &operator=(Poller &&) = delete;

    bool valid() const;

    fiber::common::IoErr add(int fd, Event events, Item *item, Mode mode = Mode::None);
    fiber::common::IoErr mod(int fd, Event events, Item *item, Mode mode = Mode::None);
    fiber::common::IoErr del(int fd);
    int wait(epoll_event *events, int max_events, int timeout_ms);

private:
    int epoll_fd_ = -1;
};

constexpr Poller::Event operator|(Poller::Event left, Poller::Event right) noexcept {
    using U = std::underlying_type_t<Poller::Event>;
    return static_cast<Poller::Event>(static_cast<U>(left) | static_cast<U>(right));
}

constexpr Poller::Event operator&(Poller::Event left, Poller::Event right) noexcept {
    using U = std::underlying_type_t<Poller::Event>;
    return static_cast<Poller::Event>(static_cast<U>(left) & static_cast<U>(right));
}

constexpr Poller::Event operator^(Poller::Event left, Poller::Event right) noexcept {
    using U = std::underlying_type_t<Poller::Event>;
    return static_cast<Poller::Event>(static_cast<U>(left) ^ static_cast<U>(right));
}

constexpr Poller::Event operator~(Poller::Event value) noexcept {
    using U = std::underlying_type_t<Poller::Event>;
    return static_cast<Poller::Event>(~static_cast<U>(value));
}

constexpr Poller::Mode operator|(Poller::Mode left, Poller::Mode right) noexcept {
    using U = std::underlying_type_t<Poller::Mode>;
    return static_cast<Poller::Mode>(static_cast<U>(left) | static_cast<U>(right));
}

constexpr Poller::Mode operator&(Poller::Mode left, Poller::Mode right) noexcept {
    using U = std::underlying_type_t<Poller::Mode>;
    return static_cast<Poller::Mode>(static_cast<U>(left) & static_cast<U>(right));
}

constexpr Poller::Mode operator^(Poller::Mode left, Poller::Mode right) noexcept {
    using U = std::underlying_type_t<Poller::Mode>;
    return static_cast<Poller::Mode>(static_cast<U>(left) ^ static_cast<U>(right));
}

constexpr Poller::Mode operator~(Poller::Mode value) noexcept {
    using U = std::underlying_type_t<Poller::Mode>;
    return static_cast<Poller::Mode>(~static_cast<U>(value));
}

inline Poller::Event &operator|=(Poller::Event &left, Poller::Event right) noexcept {
    left = left | right;
    return left;
}

inline Poller::Event &operator&=(Poller::Event &left, Poller::Event right) noexcept {
    left = left & right;
    return left;
}

inline Poller::Event &operator^=(Poller::Event &left, Poller::Event right) noexcept {
    left = left ^ right;
    return left;
}

inline Poller::Mode &operator|=(Poller::Mode &left, Poller::Mode right) noexcept {
    left = left | right;
    return left;
}

inline Poller::Mode &operator&=(Poller::Mode &left, Poller::Mode right) noexcept {
    left = left & right;
    return left;
}

inline Poller::Mode &operator^=(Poller::Mode &left, Poller::Mode right) noexcept {
    left = left ^ right;
    return left;
}

constexpr bool any(Poller::Event events) noexcept {
    using U = std::underlying_type_t<Poller::Event>;
    return static_cast<U>(events) != 0;
}

} // namespace fiber::event

#endif // FIBER_EVENT_POLLER_H
