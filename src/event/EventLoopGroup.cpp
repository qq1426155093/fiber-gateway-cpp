#include "EventLoopGroup.h"

#include <pthread.h>
#include <signal.h>

#include "../async/Signal.h"
#include "../common/Assert.h"

namespace fiber::event {

EventLoopGroup::EventLoopGroup(std::size_t size)
    : threads_(size) {
    FIBER_ASSERT(size > 0);
    loops_.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
        loops_.push_back(std::make_unique<EventLoop>(this));
    }
}

EventLoopGroup::~EventLoopGroup() {
    stop();
    join();
}

void EventLoopGroup::start() {
    start_with_mask(nullptr);
}

void EventLoopGroup::start(const fiber::async::SignalSet &mask) {
    start_with_mask(&mask);
}

void EventLoopGroup::start_with_mask(const fiber::async::SignalSet *mask) {
    if (mask) {
        fiber::async::SignalSet copy = *mask;
        threads_.start([this, copy](fiber::async::ThreadGroup::Thread &thread) {
            pthread_sigmask(SIG_BLOCK, &copy.native(), nullptr);
            const auto index = thread.index();
            EventLoop &loop = *loops_[index];
            loop.run();
        });
        return;
    }
    threads_.start([this](fiber::async::ThreadGroup::Thread &thread) {
        const auto index = thread.index();
        EventLoop &loop = *loops_[index];
        loop.run();
    });
}

void EventLoopGroup::stop() {
    for (auto &loop : loops_) {
        loop->stop();
    }
}

void EventLoopGroup::join() {
    threads_.join();
}

EventLoop &EventLoopGroup::at(std::size_t index) {
    FIBER_ASSERT(index < loops_.size());
    return *loops_[index];
}

const EventLoop &EventLoopGroup::at(std::size_t index) const {
    FIBER_ASSERT(index < loops_.size());
    return *loops_[index];
}

} // namespace fiber::event
