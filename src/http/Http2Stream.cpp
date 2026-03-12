#include "Http2Stream.h"

#include <algorithm>
#include <new>

#include "../common/Assert.h"
#include "Http2Connection.h"

namespace fiber::http {

common::IoErr Http2Stream::enqueue_pending(Http2Connection &conn, Http2PendingKind kind, Http2SendPayload &&payload,
                                           std::uint8_t first_frame_flags,
                                           std::uint8_t last_frame_flags, Http2PendingEntry::ChangeFn on_change,
                                           void *user_ctx) noexcept {
    if (stream_id_ == 0 || !conn.transport_ || !conn.transport_->valid()) {
        return common::IoErr::Invalid;
    }
    if (conn.stop_sending_requested_) {
        return conn.stop_sending_reason_;
    }
    if (conn_ && conn_ != &conn) {
        return common::IoErr::Invalid;
    }

    Http2PendingEntry *entry = conn.pending_pool_.create(*this, stream_id_, kind, std::move(payload), first_frame_flags,
                                                         last_frame_flags, on_change, user_ctx);
    if (!entry) {
        return common::IoErr::NoMem;
    }

    conn_ = &conn;
    append_active_pending(*entry);
    if (pending_tail_) {
        pending_tail_->next = entry;
    } else {
        pending_head_ = entry;
    }
    pending_tail_ = entry;
    refresh_connection_membership();
    conn.reevaluate_stream(*this);
    conn.start_dispatch_loop();
    return common::IoErr::None;
}

Http2PendingKind Http2Stream::pending_kind() const noexcept {
    return pending_head_ ? pending_head_->kind : Http2PendingKind::Data;
}

bool Http2Stream::blocked_by_stream_window() const noexcept {
    return pending_head_ && pending_head_->kind == Http2PendingKind::Data && send_window_ <= 0;
}

bool Http2Stream::blocked_by_conn_window() const noexcept {
    return conn_ && pending_head_ && pending_head_->kind == Http2PendingKind::Data && send_window_ > 0 &&
           conn_->conn_send_window_ <= 0;
}

Http2Stream::ScheduleResult Http2Stream::schedule_pending() noexcept {
    if (!conn_ || !pending_head_) {
        return ScheduleResult::NoPending;
    }

    Http2PendingEntry *entry = pending_head_;
    bool scheduled_any = false;

    for (;;) {
        if (entry->kind == Http2PendingKind::Data) {
            if (send_window_ <= 0) {
                return scheduled_any ? ScheduleResult::Scheduled : ScheduleResult::BlockedByStreamWindow;
            }
            if (conn_->conn_send_window_ <= 0) {
                return scheduled_any ? ScheduleResult::Scheduled : ScheduleResult::BlockedByConnWindow;
            }
        }

        std::size_t remaining = entry->remaining_bytes();
        if (remaining == 0) {
            pop_pending_head();
            maybe_finish_pending(*entry);
            return scheduled_any ? ScheduleResult::Scheduled : ScheduleResult::NoPending;
        }

        bool first_fragment = remaining == entry->total_bytes;
        std::size_t payload_bytes = std::min<std::size_t>(remaining, conn_->options_.max_frame_size);
        if (entry->kind == Http2PendingKind::Data) {
            payload_bytes = std::min<std::size_t>(payload_bytes, static_cast<std::size_t>(conn_->conn_send_window_));
            payload_bytes = std::min<std::size_t>(payload_bytes, static_cast<std::size_t>(send_window_));
        }
        if (payload_bytes == 0) {
            return scheduled_any ? ScheduleResult::Scheduled
                                 : (entry->kind == Http2PendingKind::Data ? ScheduleResult::BlockedByConnWindow
                                                                          : ScheduleResult::NoPending);
        }

        Http2Connection::SendEntry *send = conn_->acquire_send_entry();
        if (!send) {
            conn_->stop_sending(common::IoErr::NoMem);
            return ScheduleResult::NoPending;
        }

        Http2SendPayload chunk;
        if (!entry->payload.split_prefix_to(payload_bytes, chunk)) {
            conn_->release_send_entry(send);
            conn_->stop_sending(common::IoErr::NoMem);
            return ScheduleResult::NoPending;
        }

        Http2FrameType frame_type = Http2FrameType::Data;
        std::uint8_t flags = 0;
        bool last_fragment = payload_bytes == remaining;
        if (entry->kind == Http2PendingKind::Header) {
            frame_type = first_fragment ? Http2FrameType::Headers : Http2FrameType::Continuation;
            if (first_fragment) {
                flags |= entry->first_frame_flags;
            }
            if (last_fragment) {
                flags |= entry->last_frame_flags;
            }
        } else if (last_fragment) {
            flags |= entry->last_frame_flags;
        }

        Http2Connection::encode_frame_header(send->frame_header_, static_cast<std::uint32_t>(payload_bytes), frame_type,
                                             flags, stream_id_);
        send->frame_header_size = 9;
        send->payload_ptr()->operator=(std::move(chunk));
        send->logical_bytes = payload_bytes;
        send->total_bytes = send->frame_header_size + payload_bytes;
        send->on_done = &Http2Stream::handle_send_done;
        send->user_data = entry;

        common::IoErr err = conn_->enqueue_send_entry(send);
        if (err != common::IoErr::None) {
            send->payload_ptr()->reset();
            send->on_done = nullptr;
            send->user_data = nullptr;
            send->logical_bytes = 0;
            conn_->release_send_entry(send);
            conn_->stop_sending(err);
            return ScheduleResult::NoPending;
        }

        if (entry->kind == Http2PendingKind::Data) {
            conn_->conn_send_window_ -= static_cast<std::int32_t>(payload_bytes);
            send_window_ -= static_cast<std::int32_t>(payload_bytes);
        }

        ++entry->inflight_sends;
        entry->notify_change(Http2PendingChange::Kind::Scheduled, payload_bytes, common::IoErr::None);
        scheduled_any = true;

        if (entry->payload.empty()) {
            pop_pending_head();
            if (entry->kind == Http2PendingKind::Header) {
                entry = pending_head_;
                if (!entry) {
                    return ScheduleResult::Scheduled;
                }
                continue;
            }
            return ScheduleResult::Scheduled;
        }

        if (entry->kind == Http2PendingKind::Header) {
            continue;
        }
        return ScheduleResult::Scheduled;
    }
}

void Http2Stream::update_send_window(std::int32_t delta) noexcept {
    send_window_ += delta;
    if (conn_) {
        conn_->reevaluate_stream(*this);
        conn_->start_dispatch_loop();
    }
}

void Http2Stream::drain_pending(common::IoErr result) noexcept {
    Http2PendingEntry *entry = active_pending_head_;
    while (entry) {
        Http2PendingEntry *next = entry->active_next;
        if (entry->inflight_sends == 0) {
            finish_pending(*entry, result);
        } else if (entry->result == common::IoErr::None) {
            entry->result = result;
        }
        entry = next;
    }
}

void Http2Stream::append_active_pending(Http2PendingEntry &entry) noexcept {
    entry.active_prev = nullptr;
    entry.active_next = active_pending_head_;
    if (active_pending_head_) {
        active_pending_head_->active_prev = &entry;
    }
    active_pending_head_ = &entry;
}

void Http2Stream::remove_active_pending(Http2PendingEntry &entry) noexcept {
    if (entry.active_prev) {
        entry.active_prev->active_next = entry.active_next;
    } else if (active_pending_head_ == &entry) {
        active_pending_head_ = entry.active_next;
    }
    if (entry.active_next) {
        entry.active_next->active_prev = entry.active_prev;
    }
    entry.active_prev = nullptr;
    entry.active_next = nullptr;
}

void Http2Stream::pop_pending_head() noexcept {
    Http2PendingEntry *entry = pending_head_;
    if (!entry) {
        return;
    }
    pending_head_ = entry->next;
    if (!pending_head_) {
        pending_tail_ = nullptr;
    }
    entry->next = nullptr;
    if (conn_) {
        conn_->reevaluate_stream(*this);
    }
    refresh_connection_membership();
}

void Http2Stream::maybe_finish_pending(Http2PendingEntry &entry) noexcept {
    if (entry.terminal_notified || entry.inflight_sends != 0) {
        return;
    }

    if (entry.result != common::IoErr::None) {
        finish_pending(entry, entry.result);
        return;
    }

    if (entry.payload.empty() && entry.written_bytes == entry.total_bytes) {
        finish_pending(entry, common::IoErr::None);
    }
}

void Http2Stream::finish_pending(Http2PendingEntry &entry, common::IoErr result) noexcept {
    if (entry.terminal_notified || !conn_) {
        return;
    }

    entry.result = result;
    entry.terminal_notified = true;
    entry.notify_change(result == common::IoErr::None
                                ? Http2PendingChange::Kind::Completed
                                : (result == common::IoErr::Canceled ? Http2PendingChange::Kind::Canceled
                                                                     : Http2PendingChange::Kind::Failed),
                        0, result);
    remove_active_pending(entry);
    conn_->pending_pool_.destroy(&entry);
    if (!pending_head_) {
        conn_->reevaluate_stream(*this);
    }
    refresh_connection_membership();
}

void Http2Stream::refresh_connection_membership() noexcept {
    if (!conn_) {
        return;
    }

    bool needs_registry = active_pending_head_ != nullptr;
    if (needs_registry && !in_pending_registry_) {
        conn_->register_pending_stream(*this);
        return;
    }

    if (!needs_registry && in_pending_registry_) {
        conn_->unregister_pending_stream(*this);
    }
}

void Http2Stream::handle_send_done(void *user_data, std::size_t, std::size_t written_bytes, std::size_t frame_header_size,
                                   std::size_t logical_bytes, common::IoErr result) noexcept {
    auto *pending = static_cast<Http2PendingEntry *>(user_data);
    if (!pending || !pending->stream) {
        return;
    }

    Http2Stream &stream = *pending->stream;
    std::size_t payload_written =
            written_bytes > frame_header_size ? std::min<std::size_t>(logical_bytes, written_bytes - frame_header_size) : 0;
    if (payload_written != 0) {
        pending->written_bytes += payload_written;
        pending->notify_change(Http2PendingChange::Kind::Written, payload_written, common::IoErr::None);
    }

    if (result != common::IoErr::None && pending->result == common::IoErr::None) {
        pending->result = result;
    }
    FIBER_ASSERT(pending->inflight_sends > 0);
    --pending->inflight_sends;
    stream.maybe_finish_pending(*pending);
    if (stream.conn_ && !stream.conn_->stop_sending_requested_) {
        stream.conn_->reevaluate_stream(stream);
        stream.conn_->start_dispatch_loop();
    }
}

} // namespace fiber::http
