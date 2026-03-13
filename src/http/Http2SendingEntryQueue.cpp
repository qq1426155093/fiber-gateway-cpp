#include "Http2SendingEntryQueue.h"

#include <cstring>
#include <new>

#include "../common/Assert.h"

namespace fiber::http {

Http2SendPayload *Http2SendingEntry::payload_ptr() noexcept {
    return std::launder(reinterpret_cast<Http2SendPayload *>(payload_storage_));
}

const Http2SendPayload *Http2SendingEntry::payload_ptr() const noexcept {
    return std::launder(reinterpret_cast<const Http2SendPayload *>(payload_storage_));
}

Http2SendingEntryQueue::PollAwaiter::PollAwaiter(Http2SendingEntryQueue &queue, std::chrono::milliseconds timeout) noexcept
    : queue_(&queue),
      timeout_(timeout) {
}

Http2SendingEntryQueue::PollAwaiter::~PollAwaiter() {
    if (!queue_) {
        return;
    }
    queue_->cancel_waiter(this);
    if (loop_ && timer_entry_.is_in_heap()) {
        loop_->cancel<PollAwaiter, &PollAwaiter::timer_entry_>(*this);
    }
}

bool Http2SendingEntryQueue::PollAwaiter::await_ready() const noexcept {
    return queue_ == nullptr || queue_->ready_head_ != nullptr || queue_->closed_ || timeout_.count() == 0;
}

bool Http2SendingEntryQueue::PollAwaiter::await_suspend(std::coroutine_handle<> handle) {
    if (!queue_) {
        return false;
    }

    loop_ = fiber::event::EventLoop::current_or_null();
    FIBER_ASSERT(loop_ != nullptr);
    handle_ = handle;

    if (!queue_->arm_waiter(this)) {
        return false;
    }

    if (has_timer()) {
        loop_->post_at<PollAwaiter, &PollAwaiter::timer_entry_, &PollAwaiter::on_timeout>(loop_->now() + timeout_, *this);
    }
    return true;
}

Http2SendingEntryQueue::PollResult Http2SendingEntryQueue::PollAwaiter::await_resume() noexcept {
    if (queue_ == nullptr) {
        return {};
    }

    if (loop_ && timer_entry_.is_in_heap()) {
        loop_->cancel<PollAwaiter, &PollAwaiter::timer_entry_>(*this);
    }
    if (queue_->waiter_ == this) {
        queue_->waiter_ = nullptr;
    }

    PollResult result;
    if (Http2SendingEntry *entry = queue_->pop_ready()) {
        result.kind = PollResult::Kind::Entry;
        result.entry = entry;
    } else if (queue_->closed_) {
        result.kind = PollResult::Kind::Closed;
    } else {
        result.kind = PollResult::Kind::TimedOut;
    }

    queue_ = nullptr;
    handle_ = {};
    loop_ = nullptr;
    timed_out_ = false;
    resume_posted_ = false;
    return result;
}

void Http2SendingEntryQueue::PollAwaiter::on_notify(PollAwaiter *awaiter) {
    if (!awaiter) {
        return;
    }
    awaiter->resume_posted_ = false;
    awaiter->resume();
}

void Http2SendingEntryQueue::PollAwaiter::on_timeout(PollAwaiter *awaiter) {
    if (!awaiter) {
        return;
    }
    awaiter->timed_out_ = true;
    if (awaiter->queue_ && awaiter->queue_->waiter_ == awaiter) {
        awaiter->queue_->waiter_ = nullptr;
    }
    awaiter->resume();
}

void Http2SendingEntryQueue::PollAwaiter::resume() noexcept {
    auto handle = handle_;
    handle_ = {};
    if (handle) {
        handle.resume();
    }
}

bool Http2SendingEntryQueue::PollAwaiter::has_timer() const noexcept {
    return timeout_.count() > 0 && timeout_ != std::chrono::milliseconds::max();
}

Http2SendingEntryQueue::~Http2SendingEntryQueue() {
    while (ready_head_) {
        Http2SendingEntry *entry = pop_ready();
        destroy_entry(entry);
    }
    while (free_head_) {
        Http2SendingEntry *entry = free_head_;
        free_head_ = entry->next;
        delete entry;
    }
    free_count_ = 0;
}

Http2SendingEntry *Http2SendingEntryQueue::acquire() noexcept {
    Http2SendingEntry *entry = free_head_;
    if (entry) {
        free_head_ = entry->next;
        entry->next = nullptr;
        --free_count_;
    } else {
        entry = new (std::nothrow) Http2SendingEntry{};
        if (!entry) {
            return nullptr;
        }
    }

    new (entry->payload_ptr()) Http2SendPayload();
    entry->next = nullptr;
    entry->total_bytes = 0;
    entry->written_bytes = 0;
    entry->frame_header_size = 0;
    std::memset(entry->frame_header_, 0, sizeof(entry->frame_header_));
    entry->logical_bytes = 0;
    entry->result = common::IoErr::None;
    entry->done_notified = false;
    entry->on_done = nullptr;
    entry->user_data = nullptr;
    return entry;
}

void Http2SendingEntryQueue::release(Http2SendingEntry *entry) noexcept {
    if (!entry) {
        return;
    }

    reset_released_entry(entry);
    if (free_count_ < max_free_entries_) {
        entry->next = free_head_;
        free_head_ = entry;
        ++free_count_;
        return;
    }

    delete entry;
}

common::IoErr Http2SendingEntryQueue::enqueue(Http2SendingEntry *entry) noexcept {
    if (!entry) {
        return common::IoErr::Invalid;
    }
    if (closed_) {
        return common::IoErr::Canceled;
    }

    entry->next = nullptr;
    if (ready_tail_) {
        ready_tail_->next = entry;
    } else {
        ready_head_ = entry;
    }
    ready_tail_ = entry;
    notify_waiter();
    return common::IoErr::None;
}

Http2SendingEntryQueue::PollAwaiter Http2SendingEntryQueue::poll_to_send(std::chrono::milliseconds timeout) noexcept {
    return PollAwaiter(*this, timeout);
}

Http2SendingEntry *Http2SendingEntryQueue::pop_ready() noexcept {
    Http2SendingEntry *entry = ready_head_;
    if (!entry) {
        return nullptr;
    }

    ready_head_ = entry->next;
    if (!ready_head_) {
        ready_tail_ = nullptr;
    }
    entry->next = nullptr;
    return entry;
}

void Http2SendingEntryQueue::close() noexcept {
    if (closed_) {
        return;
    }
    closed_ = true;
    notify_waiter();
}

void Http2SendingEntryQueue::destroy_entry(Http2SendingEntry *entry) noexcept {
    if (!entry) {
        return;
    }
    entry->payload_ptr()->~Http2SendPayload();
    delete entry;
}

void Http2SendingEntryQueue::reset_released_entry(Http2SendingEntry *entry) noexcept {
    entry->payload_ptr()->~Http2SendPayload();
    entry->next = nullptr;
    entry->total_bytes = 0;
    entry->written_bytes = 0;
    entry->frame_header_size = 0;
    std::memset(entry->frame_header_, 0, sizeof(entry->frame_header_));
    entry->logical_bytes = 0;
    entry->result = common::IoErr::None;
    entry->done_notified = false;
    entry->on_done = nullptr;
    entry->user_data = nullptr;
}

bool Http2SendingEntryQueue::arm_waiter(PollAwaiter *awaiter) noexcept {
    if (!awaiter || ready_head_ || closed_) {
        return false;
    }

    FIBER_ASSERT(waiter_ == nullptr);
    waiter_ = awaiter;
    return true;
}

void Http2SendingEntryQueue::cancel_waiter(PollAwaiter *awaiter) noexcept {
    if (waiter_ == awaiter) {
        waiter_ = nullptr;
    }
}

void Http2SendingEntryQueue::notify_waiter() noexcept {
    if (!waiter_ || waiter_->resume_posted_ || waiter_->loop_ == nullptr) {
        return;
    }

    waiter_->resume_posted_ = true;
    waiter_->loop_->post<PollAwaiter, &PollAwaiter::notify_entry_, &PollAwaiter::on_notify>(*waiter_);
}

} // namespace fiber::http
