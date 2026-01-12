#ifndef FIBER_NET_UNIX_LISTENER_H
#define FIBER_NET_UNIX_LISTENER_H

#include <cstdint>

#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../common/IoError.h"
#include "../event/EventLoop.h"
#include "UnixAddress.h"
#include "detail/AcceptFd.h"

namespace fiber::net {

struct UnixListenOptions {
    int backlog = SOMAXCONN;
    bool unlink_existing = false;
};

struct UnixAcceptResult {
    int fd = -1;
    UnixAddress peer = UnixAddress::unnamed();
};

struct UnixTraits {
    using Address = UnixAddress;
    using ListenOptions = fiber::net::UnixListenOptions;
    using AcceptResult = fiber::net::UnixAcceptResult;

    static fiber::common::IoResult<int> bind(const Address &addr,
                                             const ListenOptions &options);
    static fiber::common::IoErr accept_once(int fd, AcceptResult &out);
};

class UnixListener : public common::NonCopyable, public common::NonMovable {
public:
    using AcceptAwaiter = detail::AcceptFd<UnixTraits>::AcceptAwaiter;

    explicit UnixListener(fiber::event::EventLoop &loop);
    ~UnixListener();

    fiber::common::IoResult<void> bind(const UnixAddress &addr,
                                       const UnixListenOptions &options);
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] int fd() const noexcept;
    void close();

    [[nodiscard]] AcceptAwaiter accept() noexcept;

private:
    detail::AcceptFd<UnixTraits> acceptor_;
};

} // namespace fiber::net

#endif // FIBER_NET_UNIX_LISTENER_H
