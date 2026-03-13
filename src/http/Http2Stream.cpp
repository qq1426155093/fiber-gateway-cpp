#include "Http2Stream.h"

#include <algorithm>
#include <array>
#include <new>

#include "../common/Assert.h"
#include "Http2Connection.h"

namespace fiber::http {

namespace {

constexpr std::uint8_t kFlagEndStream = 0x1;
constexpr std::uint8_t kFlagEndHeaders = 0x4;

constexpr std::uint16_t bit(Http2Stream::StreamOp op) noexcept {
    return static_cast<std::uint16_t>(1u << static_cast<std::uint8_t>(op));
}

constexpr std::array<std::uint16_t, 7> kValidOpBits = {
    bit(Http2Stream::StreamOp::RecvHeaders) | bit(Http2Stream::StreamOp::RecvHeadersEndStream) |
        bit(Http2Stream::StreamOp::SendHeaders) | bit(Http2Stream::StreamOp::SendHeadersEndStream),
    0,
    0,
    bit(Http2Stream::StreamOp::RecvHeaders) | bit(Http2Stream::StreamOp::RecvHeadersEndStream) |
        bit(Http2Stream::StreamOp::RecvData) | bit(Http2Stream::StreamOp::RecvDataEndStream) |
        bit(Http2Stream::StreamOp::SendHeaders) | bit(Http2Stream::StreamOp::SendHeadersEndStream) |
        bit(Http2Stream::StreamOp::SendData) | bit(Http2Stream::StreamOp::SendDataEndStream),
    bit(Http2Stream::StreamOp::RecvHeaders) | bit(Http2Stream::StreamOp::RecvHeadersEndStream) |
        bit(Http2Stream::StreamOp::RecvData) | bit(Http2Stream::StreamOp::RecvDataEndStream),
    bit(Http2Stream::StreamOp::SendHeaders) | bit(Http2Stream::StreamOp::SendHeadersEndStream) |
        bit(Http2Stream::StreamOp::SendData) | bit(Http2Stream::StreamOp::SendDataEndStream),
    0,
};

} // namespace

common::IoErr Http2Stream::enqueue_pending(Http2PendingKind kind, Http2SendPayload &&payload, std::uint8_t first_frame_flags,
                                           std::uint8_t last_frame_flags, Http2PendingEntry::ChangeFn on_change,
                                           void *user_ctx) noexcept {
    if (!conn_ || !active_ || state_ == State::Closed) {
        return common::IoErr::Invalid;
    }

    Http2PendingEntry *entry = conn_->pending_pool_.create(*this, stream_id_, kind, std::move(payload), first_frame_flags,
                                                           last_frame_flags, on_change, user_ctx);
    if (!entry) {
        return common::IoErr::NoMem;
    }

    append_active_pending(*entry);
    if (pending_tail_) {
        pending_tail_->next = entry;
    } else {
        pending_head_ = entry;
    }
    pending_tail_ = entry;
    try_schedule_pending();
    return common::IoErr::None;
}

common::IoErr Http2Stream::on_header_recv(const mem::IoBuf &payload, std::size_t block_offset, std::size_t length,
                                          bool end_headers, bool end_stream) noexcept {
    common::IoErr err = transition_on_recv_headers(end_stream);
    if (err != common::IoErr::None) {
        return err;
    }

    (void)payload;
    (void)block_offset;
    (void)length;
    (void)end_headers;
    // TODO: decode/process received header block fragments.
    return common::IoErr::None;
}

common::IoErr Http2Stream::on_data_recv(const mem::IoBuf &payload, std::size_t data_offset, std::size_t length,
                                        bool end_stream) noexcept {
    common::IoErr err = transition_on_recv_data(end_stream);
    if (err != common::IoErr::None) {
        return err;
    }

    (void)payload;
    (void)data_offset;
    (void)length;
    // TODO: consume/process received DATA payload.
    return common::IoErr::None;
}

void Http2Stream::on_remote_rst(Http2ErrorCode, common::IoErr result) noexcept { close(result); }

common::IoErr Http2Stream::send_header(Http2SendPayload &&payload, bool end_stream) noexcept {
    if (!conn_) {
        return common::IoErr::Invalid;
    }

    common::IoErr err = transition_on_send_headers(end_stream);
    if (err != common::IoErr::None) {
        return err;
    }

    std::uint8_t last_flags = kFlagEndHeaders;
    if (end_stream) {
        last_flags |= kFlagEndStream;
    }
    // TODO: split encoded header block into HEADERS/CONTINUATION fragments as needed.
    return enqueue_pending(Http2PendingKind::Header, std::move(payload), 0, last_flags, nullptr, nullptr);
}

common::IoErr Http2Stream::send_data(Http2SendPayload &&payload, bool end_stream) noexcept {
    if (!conn_) {
        return common::IoErr::Invalid;
    }

    common::IoErr err = transition_on_send_data(end_stream);
    if (err != common::IoErr::None) {
        return err;
    }

    return enqueue_pending(Http2PendingKind::Data, std::move(payload), 0, end_stream ? kFlagEndStream : 0,
                           nullptr, nullptr);
}

common::IoErr Http2Stream::close_rst(Http2ErrorCode code, common::IoErr result) noexcept {
    if (!conn_) {
        return common::IoErr::Invalid;
    }

    common::IoErr err = conn_->send_rst_stream(stream_id_, code);
    if (err != common::IoErr::None) {
        return err;
    }

    close(result);
    conn_->maybe_destroy_stream(*this);
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
    if (!conn_) {
        return ScheduleResult::NoPending;
    }
    if (!pending_head_) {
        sync_conn_window_wait_membership();
        return ScheduleResult::NoPending;
    }

    Http2PendingEntry *entry = pending_head_;
    bool scheduled_any = false;
    auto finish = [this](ScheduleResult result) noexcept {
        sync_conn_window_wait_membership();
        return result;
    };

    for (;;) {
        if (entry->kind == Http2PendingKind::Data) {
            if (send_window_ <= 0) {
                return finish(scheduled_any ? ScheduleResult::Scheduled : ScheduleResult::BlockedByStreamWindow);
            }
            if (conn_->conn_send_window_ <= 0) {
                return finish(scheduled_any ? ScheduleResult::Scheduled : ScheduleResult::BlockedByConnWindow);
            }
        }

        std::size_t remaining = entry->remaining_bytes();
        if (remaining == 0) {
            pop_pending_head();
            maybe_finish_pending(*entry);
            return finish(scheduled_any ? ScheduleResult::Scheduled : ScheduleResult::NoPending);
        }

        bool first_fragment = remaining == entry->total_bytes;
        std::size_t payload_bytes = std::min<std::size_t>(remaining, conn_->peer_max_outbound_frame_size_);
        if (entry->kind == Http2PendingKind::Data) {
            payload_bytes = std::min<std::size_t>(payload_bytes, static_cast<std::size_t>(conn_->conn_send_window_));
            payload_bytes = std::min<std::size_t>(payload_bytes, static_cast<std::size_t>(send_window_));
        }
        if (payload_bytes == 0) {
            return finish(scheduled_any ? ScheduleResult::Scheduled
                                        : (entry->kind == Http2PendingKind::Data ? ScheduleResult::BlockedByConnWindow
                                                                                 : ScheduleResult::NoPending));
        }

        Http2Connection::SendEntry *send = conn_->acquire_send_entry();
        if (!send) {
            conn_->stop_sending(common::IoErr::NoMem);
            return finish(ScheduleResult::NoPending);
        }

        Http2SendPayload chunk;
        if (!entry->payload.split_prefix_to(payload_bytes, chunk)) {
            conn_->release_send_entry(send);
            conn_->stop_sending(common::IoErr::NoMem);
            return finish(ScheduleResult::NoPending);
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

        encode_http2_frame_header(send->frame_header_, static_cast<std::uint32_t>(payload_bytes), frame_type, flags,
                                  stream_id_);
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
            return finish(ScheduleResult::NoPending);
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
                    return finish(ScheduleResult::Scheduled);
                }
                continue;
            }
            return finish(ScheduleResult::Scheduled);
        }

        if (entry->kind == Http2PendingKind::Header) {
            continue;
        }
        return finish(ScheduleResult::Scheduled);
    }
}

void Http2Stream::update_send_window(std::int32_t delta) noexcept {
    send_window_ += delta;
    try_schedule_pending();
}

bool Http2Stream::is_valid_transition(State state, StreamOp op) noexcept {
    const std::size_t state_index = static_cast<std::size_t>(state);
    FIBER_ASSERT(state_index < kValidOpBits.size());
    return (kValidOpBits[state_index] & bit(op)) != 0;
}

common::IoErr Http2Stream::transition_on_recv_headers(bool end_stream) noexcept {
    const StreamOp op = end_stream ? StreamOp::RecvHeadersEndStream : StreamOp::RecvHeaders;
    if (!is_valid_transition(state_, op)) {
        return common::IoErr::Invalid;
    }

    if (state_ == State::Idle) {
        state_ = end_stream ? State::HalfClosedRemote : State::Open;
    } else if (end_stream) {
        transition_on_remote_end_stream();
    }
    return common::IoErr::None;
}

common::IoErr Http2Stream::transition_on_recv_data(bool end_stream) noexcept {
    const StreamOp op = end_stream ? StreamOp::RecvDataEndStream : StreamOp::RecvData;
    if (!is_valid_transition(state_, op)) {
        return common::IoErr::Invalid;
    }

    if (end_stream) {
        transition_on_remote_end_stream();
    }
    return common::IoErr::None;
}

common::IoErr Http2Stream::transition_on_send_headers(bool end_stream) noexcept {
    const StreamOp op = end_stream ? StreamOp::SendHeadersEndStream : StreamOp::SendHeaders;
    if (!is_valid_transition(state_, op)) {
        return common::IoErr::Invalid;
    }

    if (state_ == State::Idle) {
        state_ = end_stream ? State::HalfClosedLocal : State::Open;
    } else if (end_stream) {
        transition_on_local_end_stream();
    }
    return common::IoErr::None;
}

common::IoErr Http2Stream::transition_on_send_data(bool end_stream) noexcept {
    const StreamOp op = end_stream ? StreamOp::SendDataEndStream : StreamOp::SendData;
    if (!is_valid_transition(state_, op)) {
        return common::IoErr::Invalid;
    }

    if (end_stream) {
        transition_on_local_end_stream();
    }
    return common::IoErr::None;
}

void Http2Stream::transition_on_remote_end_stream() noexcept {
    switch (state_) {
        case State::Open:
            state_ = State::HalfClosedRemote;
            break;
        case State::HalfClosedLocal:
            state_ = State::Closed;
            break;
        default:
            break;
    }
}

void Http2Stream::transition_on_local_end_stream() noexcept {
    switch (state_) {
        case State::Open:
            state_ = State::HalfClosedLocal;
            break;
        case State::HalfClosedRemote:
            state_ = State::Closed;
            break;
        default:
            break;
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

void Http2Stream::close(common::IoErr result) noexcept {
    state_ = State::Closed;
    active_ = false;
    remove_from_conn_window_wait_list();
    pending_head_ = nullptr;
    pending_tail_ = nullptr;
    drain_pending(result);
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
    conn_->maybe_destroy_stream(*this);
}

void Http2Stream::sync_conn_window_wait_membership() noexcept {
    if (!conn_) {
        return;
    }
    if (blocked_by_conn_window()) {
        conn_->conn_wait_streams_.push_back(*this);
        return;
    }
    conn_->conn_wait_streams_.erase(*this);
}

void Http2Stream::remove_from_conn_window_wait_list() noexcept {
    if (!conn_) {
        return;
    }
    conn_->conn_wait_streams_.erase(*this);
}

void Http2Stream::try_schedule_pending() noexcept {
    if (!conn_ || conn_->stop_sending_requested_ || state_ == State::Closed) {
        return;
    }
    (void)schedule_pending();
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
    if (stream.state_ != State::Closed) {
        stream.try_schedule_pending();
    }
}

} // namespace fiber::http
