#include "SignalService.h"

#include <cerrno>
#include <pthread.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include "../common/Assert.h"

namespace fiber::event {

thread_local SignalService *SignalService::current_ = nullptr;

namespace {

fiber::async::SignalInfo to_signal_info(const signalfd_siginfo &info) {
    fiber::async::SignalInfo out{};
    out.signum = static_cast<int>(info.ssi_signo);
    out.code = static_cast<int>(info.ssi_code);
    out.pid = static_cast<pid_t>(info.ssi_pid);
    out.uid = static_cast<uid_t>(info.ssi_uid);
    out.status = static_cast<int>(info.ssi_status);
    out.errno_ = static_cast<int>(info.ssi_errno);
    out.value = static_cast<std::intptr_t>(info.ssi_ptr);
    return out;
}

} // namespace

SignalService::SignalService(EventLoop &loop) : loop_(loop) {
    item_.service = this;
    item_.callback = &SignalService::on_signalfd;
}

SignalService::~SignalService() {
    if (attached_.load(std::memory_order_acquire)) {
        if (loop_.in_loop()) {
            detach();
        } else {
            FIBER_ASSERT_MSG(false, "SignalService must be detached on loop thread before destruction");
        }
    }
    if (current_ == this) {
        current_ = nullptr;
    }
}

bool SignalService::attach(const fiber::async::SignalSet &mask) {
    FIBER_ASSERT(loop_.in_loop());
    if (attached_.load(std::memory_order_acquire)) {
        return false;
    }
    FIBER_ASSERT(current_ == nullptr);
    mask_ = mask;
    pthread_sigmask(SIG_BLOCK, &mask_.native(), nullptr);
    signalfd_ = ::signalfd(-1, &mask_.native(), SFD_NONBLOCK | SFD_CLOEXEC);
    if (signalfd_ < 0) {
        return false;
    }
    if (loop_.poller().add(signalfd_, IoEvent::Read, &item_) != fiber::common::IoErr::None) {
        ::close(signalfd_);
        signalfd_ = -1;
        return false;
    }
    attached_.store(true, std::memory_order_release);
    current_ = this;
    return true;
}

void SignalService::detach() {
    FIBER_ASSERT(loop_.in_loop());
    if (!attached_.load(std::memory_order_acquire)) {
        return;
    }
    attached_.store(false, std::memory_order_release);
    if (signalfd_ >= 0) {
        loop_.poller().del(signalfd_);
        ::close(signalfd_);
        signalfd_ = -1;
    }
    for (int signum = 0; signum < NSIG; ++signum) {
        auto &queue = waiters_[signum];
        FIBER_ASSERT(queue.head == nullptr);
        queue.tail = nullptr;
        pending_[signum].clear();
    }
    if (current_ == this) {
        current_ = nullptr;
    }
}

SignalService &SignalService::current() {
    FIBER_ASSERT(current_ != nullptr);
    return *current_;
}

SignalService *SignalService::current_or_null() noexcept {
    return current_;
}

bool SignalService::valid_signum(int signum) noexcept {
    return signum > 0 && signum < NSIG;
}

void SignalService::enqueue_waiter(int signum, fiber::async::detail::SignalWaiter *waiter) {
    FIBER_ASSERT(loop_.in_loop());
    FIBER_ASSERT(attached_.load(std::memory_order_acquire));
    FIBER_ASSERT(waiter != nullptr);
    FIBER_ASSERT(valid_signum(signum));
    FIBER_ASSERT(waiter->signum == signum);
    FIBER_ASSERT(waiter->loop == &loop_);
    FIBER_ASSERT(!waiter->queued);

    auto &queue = waiters_[signum];
    waiter->prev = queue.tail;
    waiter->next = nullptr;
    if (queue.tail) {
        queue.tail->next = waiter;
    } else {
        queue.head = waiter;
    }
    queue.tail = waiter;
    waiter->queued = true;
}

void SignalService::cancel_waiter(fiber::async::detail::SignalWaiter *waiter) {
    if (!waiter) {
        return;
    }
    FIBER_ASSERT(loop_.in_loop());
    FIBER_ASSERT(valid_signum(waiter->signum));
    auto state = waiter->state.load(std::memory_order_acquire);
    if (state == fiber::async::detail::SignalWaiterState::Waiting) {
        if (waiter->queued) {
            if (waiter->prev) {
                waiter->prev->next = waiter->next;
            } else {
                waiters_[waiter->signum].head = waiter->next;
            }
            if (waiter->next) {
                waiter->next->prev = waiter->prev;
            } else {
                waiters_[waiter->signum].tail = waiter->prev;
            }
            waiter->prev = nullptr;
            waiter->next = nullptr;
            waiter->queued = false;
        }
        waiter->state.store(fiber::async::detail::SignalWaiterState::Canceled, std::memory_order_release);
        waiter->handle = {};
        delete waiter;
        return;
    }
    if (state == fiber::async::detail::SignalWaiterState::Notified) {
        waiter->state.store(fiber::async::detail::SignalWaiterState::Canceled, std::memory_order_release);
        waiter->handle = {};
    }
}

bool SignalService::try_pop_pending(int signum, fiber::async::SignalInfo &out) {
    FIBER_ASSERT(loop_.in_loop());
    FIBER_ASSERT(attached_.load(std::memory_order_acquire));
    if (!valid_signum(signum)) {
        return false;
    }
    auto &queue = pending_[signum];
    if (queue.empty()) {
        return false;
    }
    out = queue.front();
    queue.pop_front();
    return true;
}

fiber::async::detail::SignalWaiter *SignalService::pop_next_waiter(int signum) {
    auto &queue = waiters_[signum];
    while (queue.head) {
        auto *waiter = queue.head;
        queue.head = waiter->next;
        if (queue.head) {
            queue.head->prev = nullptr;
        } else {
            queue.tail = nullptr;
        }
        waiter->prev = nullptr;
        waiter->next = nullptr;
        waiter->queued = false;
        auto state = waiter->state.load(std::memory_order_acquire);
        if (state != fiber::async::detail::SignalWaiterState::Waiting) {
            continue;
        }
        waiter->state.store(fiber::async::detail::SignalWaiterState::Notified, std::memory_order_release);
        return waiter;
    }
    return nullptr;
}

void SignalService::on_delivery(const fiber::async::SignalInfo &info) {
    if (!attached_.load(std::memory_order_acquire)) {
        return;
    }
    if (!valid_signum(info.signum)) {
        return;
    }
    auto *waiter = pop_next_waiter(info.signum);
    if (!waiter) {
        pending_[info.signum].push_back(info);
        return;
    }
    waiter->info = info;
    loop_.post<fiber::async::detail::SignalWaiter,
               &fiber::async::detail::SignalWaiter::defer_entry,
               &fiber::async::detail::SignalWaiter::on_run,
               &fiber::async::detail::SignalWaiter::on_cancel>(*waiter);
}

void SignalService::on_signalfd(Poller::Item *item, int fd, IoEvent events) {
    (void) fd;
    if (!item || !any(events)) {
        return;
    }
    auto *signal_item = static_cast<SignalItem *>(item);
    if (!signal_item->service) {
        return;
    }
    signal_item->service->drain_signalfd();
}

void SignalService::drain_signalfd() {
    FIBER_ASSERT(loop_.in_loop());
    if (!attached_.load(std::memory_order_acquire) || signalfd_ < 0) {
        return;
    }
    for (;;) {
        signalfd_siginfo info{};
        ssize_t rc = ::read(signalfd_, &info, sizeof(info));
        if (rc == static_cast<ssize_t>(sizeof(info))) {
            on_delivery(to_signal_info(info));
            continue;
        }
        if (rc < 0 && (errno == EINTR)) {
            continue;
        }
        break;
    }
}

} // namespace fiber::event
