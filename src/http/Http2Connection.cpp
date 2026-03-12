#include "Http2Connection.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <string_view>

#include "../async/Spawn.h"

namespace fiber::http {

namespace {

constexpr std::string_view kClientPreface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
constexpr std::size_t kFrameHeaderSize = 9;

enum class ParsePhase : std::uint8_t {
    Preface,
    FrameHeader,
    FramePayload,
};

std::uint32_t parse_frame_length(const std::uint8_t *pos) noexcept {
    return (static_cast<std::uint32_t>(pos[0]) << 16) | (static_cast<std::uint32_t>(pos[1]) << 8) |
           static_cast<std::uint32_t>(pos[2]);
}

std::uint32_t parse_stream_id(const std::uint8_t *pos) noexcept {
    return ((static_cast<std::uint32_t>(pos[0]) & 0x7fU) << 24) | (static_cast<std::uint32_t>(pos[1]) << 16) |
           (static_cast<std::uint32_t>(pos[2]) << 8) | static_cast<std::uint32_t>(pos[3]);
}

common::IoErr prepare_read_buffer(mem::IoBuf &read_buf, std::size_t capacity) noexcept {
    if (!read_buf) {
        read_buf = mem::IoBuf::allocate(capacity);
        return read_buf ? common::IoErr::None : common::IoErr::NoMem;
    }

    std::size_t unread = read_buf.readable();
    const std::uint8_t *unread_begin = read_buf.readable_data();

    if (!read_buf.unique()) {
        mem::IoBuf next = mem::IoBuf::allocate(capacity);
        if (!next) {
            return common::IoErr::NoMem;
        }
        if (unread != 0) {
            std::memcpy(next.writable_data(), unread_begin, unread);
            next.commit(unread);
        }
        read_buf = std::move(next);
        return common::IoErr::None;
    }

    if (unread == 0) {
        read_buf.clear();
        return common::IoErr::None;
    }

    if (unread_begin != read_buf.data()) {
        std::memmove(read_buf.data(), unread_begin, unread);
    }
    read_buf.clear();
    read_buf.commit(unread);
    return common::IoErr::None;
}

} // namespace

Http2Connection::Http2Connection(std::unique_ptr<HttpTransport> transport) :
    Http2Connection(std::move(transport), Options{}) {}

Http2Connection::Http2Connection(std::unique_ptr<HttpTransport> transport, Options options) :
    transport_(std::move(transport)), options_(std::move(options)) {}

Http2Connection::~Http2Connection() {
    if (!writer_running_) {
        drain_send_queue(common::IoErr::Canceled);
    }

    while (free_send_entries_) {
        SendEntry *entry = free_send_entries_;
        free_send_entries_ = entry->next;
        delete entry;
    }
    free_send_entry_count_ = 0;
}

fiber::async::Task<Http2Connection::RunResult> Http2Connection::run() noexcept {
    if (!transport_ || !transport_->valid()) {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    std::size_t read_buffer_capacity = std::max(options_.read_buffer_size, kClientPreface.size());
    mem::IoBuf read_buf = mem::IoBuf::allocate(read_buffer_capacity);
    if (!read_buf) {
        co_return std::unexpected(common::IoErr::NoMem);
    }

    ParsePhase phase = options_.expect_peer_preface ? ParsePhase::Preface : ParsePhase::FrameHeader;
    FrameHeader current_header{};
    std::uint32_t payload_remaining = 0;
    std::size_t payload_offset = 0;

    for (;;) {
        for (;;) {
            if (phase == ParsePhase::Preface) {
                if (read_buf.readable() < kClientPreface.size()) {
                    break;
                }
                if (std::memcmp(read_buf.readable_data(), kClientPreface.data(), kClientPreface.size()) != 0) {
                    co_return std::unexpected(common::IoErr::Invalid);
                }
                read_buf.consume(kClientPreface.size());
                phase = ParsePhase::FrameHeader;
                continue;
            }

            if (phase == ParsePhase::FrameHeader) {
                if (read_buf.readable() < kFrameHeaderSize) {
                    break;
                }

                const std::uint8_t *header = read_buf.readable_data();
                current_header.length = parse_frame_length(header);
                current_header.type = static_cast<Http2FrameType>(header[3]);
                current_header.flags = header[4];
                current_header.stream_id = parse_stream_id(header + 5);

                if (current_header.length > options_.max_frame_size) {
                    co_return std::unexpected(common::IoErr::Invalid);
                }

                read_buf.consume(kFrameHeaderSize);
                payload_remaining = current_header.length;
                payload_offset = 0;

                if (payload_remaining == 0) {
                    common::IoErr err = on_frame_payload(current_header, read_buf, 0, 0);
                    if (err != common::IoErr::None) {
                        co_return std::unexpected(err);
                    }
                    continue;
                }

                phase = ParsePhase::FramePayload;
                continue;
            }

            if (read_buf.readable() == 0) {
                break;
            }

            std::size_t chunk_len = std::min<std::size_t>(read_buf.readable(), payload_remaining);
            common::IoErr err = on_frame_payload(current_header, read_buf, payload_offset, chunk_len);
            if (err != common::IoErr::None) {
                co_return std::unexpected(err);
            }

            read_buf.consume(chunk_len);
            payload_remaining -= static_cast<std::uint32_t>(chunk_len);
            payload_offset += chunk_len;

            if (payload_remaining == 0) {
                phase = ParsePhase::FrameHeader;
            }
        }

        common::IoErr prepare_err = prepare_read_buffer(read_buf, read_buffer_capacity);
        if (prepare_err != common::IoErr::None) {
            co_return std::unexpected(prepare_err);
        }

        auto read_result = co_await transport_->read_into(read_buf, options_.read_timeout);
        if (!read_result) {
            co_return std::unexpected(read_result.error());
        }

        if (*read_result == 0) {
            if (phase == ParsePhase::FrameHeader && read_buf.readable() == 0) {
                co_return RunResult{};
            }
            co_return std::unexpected(common::IoErr::ConnReset);
        }
    }
}

common::IoErr Http2Connection::on_frame_payload(const FrameHeader &, const mem::IoBuf &, std::size_t,
                                                std::size_t) noexcept {
    return common::IoErr::None;
}

Http2Connection::SendPayload *Http2Connection::SendEntry::payload_ptr() noexcept {
    return std::launder(reinterpret_cast<SendPayload *>(payload_storage_));
}

const Http2Connection::SendPayload *Http2Connection::SendEntry::payload_ptr() const noexcept {
    return std::launder(reinterpret_cast<const SendPayload *>(payload_storage_));
}

Http2Connection::SendPayload::~SendPayload() { reset(); }

Http2Connection::SendPayload::SendPayload(SendPayload &&other) noexcept { move_from(std::move(other)); }

Http2Connection::SendPayload &Http2Connection::SendPayload::operator=(SendPayload &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    reset();
    move_from(std::move(other));
    return *this;
}

Http2Connection::SendPayload::Kind Http2Connection::SendPayload::kind() const noexcept { return kind_; }

bool Http2Connection::SendPayload::empty() const noexcept { return readable_bytes() == 0; }

std::size_t Http2Connection::SendPayload::readable_bytes() const noexcept {
    switch (kind_) {
        case Kind::StableSpan:
            return span().length - span().offset;
        case Kind::IoBuf:
            return buf().readable();
        case Kind::IoBufChain:
            return chain().readable_bytes();
        case Kind::None:
        default:
            return 0;
    }
}

void Http2Connection::SendPayload::reset() noexcept {
    switch (kind_) {
        case Kind::StableSpan:
            span().~StableSpan();
            break;
        case Kind::IoBuf:
            buf().~IoBuf();
            break;
        case Kind::IoBufChain:
            chain().~IoBufChain();
            break;
        case Kind::None:
        default:
            break;
    }
    kind_ = Kind::None;
}

void Http2Connection::SendPayload::set_stable_span(const std::uint8_t *data, std::size_t length) noexcept {
    reset();
    new (&storage_.span) StableSpan{data, length, 0};
    kind_ = Kind::StableSpan;
}

void Http2Connection::SendPayload::set_buf(mem::IoBuf &&buf) noexcept {
    reset();
    new (&storage_.buf) mem::IoBuf(std::move(buf));
    kind_ = Kind::IoBuf;
}

void Http2Connection::SendPayload::set_chain(mem::IoBufChain &&bufs) noexcept {
    reset();
    new (&storage_.chain) mem::IoBufChain(std::move(bufs));
    kind_ = Kind::IoBufChain;
}

fiber::async::Task<common::IoResult<size_t>>
Http2Connection::SendPayload::write_once(HttpTransport &transport, std::chrono::milliseconds timeout) noexcept {
    switch (kind_) {
        case Kind::StableSpan: {
            StableSpan &value = span();
            std::size_t remaining = value.length - value.offset;
            auto result = co_await transport.write(value.data + value.offset, remaining, timeout);
            if (result) {
                value.offset += *result;
            }
            co_return result;
        }
        case Kind::IoBuf: {
            mem::IoBuf &value = buf();
            auto result = co_await transport.write(value.readable_data(), value.readable(), timeout);
            if (result) {
                value.consume(*result);
            }
            co_return result;
        }
        case Kind::IoBufChain:
            co_return co_await transport.writev(chain(), timeout);
        case Kind::None:
        default:
            co_return static_cast<size_t>(0);
    }
}

void Http2Connection::SendPayload::move_from(SendPayload &&other) noexcept {
    switch (other.kind_) {
        case Kind::StableSpan:
            new (&storage_.span) StableSpan(other.span());
            kind_ = Kind::StableSpan;
            break;
        case Kind::IoBuf:
            new (&storage_.buf) mem::IoBuf(std::move(other.buf()));
            kind_ = Kind::IoBuf;
            break;
        case Kind::IoBufChain:
            new (&storage_.chain) mem::IoBufChain(std::move(other.chain()));
            kind_ = Kind::IoBufChain;
            break;
        case Kind::None:
        default:
            kind_ = Kind::None;
            break;
    }
    other.reset();
}

Http2Connection::StableSpan &Http2Connection::SendPayload::span() noexcept { return storage_.span; }

const Http2Connection::StableSpan &Http2Connection::SendPayload::span() const noexcept { return storage_.span; }

mem::IoBuf &Http2Connection::SendPayload::buf() noexcept { return storage_.buf; }

const mem::IoBuf &Http2Connection::SendPayload::buf() const noexcept { return storage_.buf; }

mem::IoBufChain &Http2Connection::SendPayload::chain() noexcept { return storage_.chain; }

const mem::IoBufChain &Http2Connection::SendPayload::chain() const noexcept { return storage_.chain; }

fiber::async::Task<void> Http2Connection::run_send_loop() noexcept {
    for (;;) {
        if (stop_sending_requested_) {
            writer_state_ = WriterState::Stopping;
            drain_send_queue(stop_sending_reason_);
            break;
        }

        SendEntry *entry = send_head_;
        if (!entry) {
            writer_state_ = WriterState::WaitingForData;
            break;
        }

        sending_ = entry;
        if (entry->payload_ptr()->empty()) {
            finish_send_entry(entry, common::IoErr::None);
            continue;
        }

        writer_state_ = WriterState::Writing;
        auto write_result = co_await entry->payload_ptr()->write_once(*transport_, options_.write_timeout);
        if (!write_result) {
            writer_state_ = WriterState::Stopping;
            finish_send_entry(entry, write_result.error());
            drain_send_queue(write_result.error());
            break;
        }
        if (*write_result == 0) {
            writer_state_ = WriterState::Stopping;
            finish_send_entry(entry, common::IoErr::ConnReset);
            drain_send_queue(common::IoErr::ConnReset);
            break;
        }

        entry->written_bytes += *write_result;
        if (entry->payload_ptr()->empty()) {
            finish_send_entry(entry, common::IoErr::None);
        }
    }

    sending_ = nullptr;
    writer_running_ = false;
    if (!stop_sending_requested_ && writer_state_ != WriterState::Stopping) {
        writer_state_ = WriterState::WaitingForData;
    }
}

void Http2Connection::start_send_loop() noexcept {
    if (writer_running_ || stop_sending_requested_ || !send_head_) {
        return;
    }
    writer_running_ = true;
    fiber::async::spawn([this]() -> fiber::async::DetachedTask {
        co_await run_send_loop();
    });
}

Http2Connection::SendEntry *Http2Connection::acquire_send_entry() noexcept {
    SendEntry *entry = free_send_entries_;
    if (entry) {
        free_send_entries_ = entry->next;
        entry->next = nullptr;
        --free_send_entry_count_;
    } else {
        entry = new (std::nothrow) SendEntry{};
        if (!entry) {
            return nullptr;
        }
    }

    new (entry->payload_ptr()) SendPayload();
    entry->next = nullptr;
    entry->total_bytes = 0;
    entry->written_bytes = 0;
    entry->result = common::IoErr::None;
    entry->done_notified = false;
    entry->on_done = nullptr;
    entry->user_data = nullptr;
    return entry;
}

void Http2Connection::release_send_entry(SendEntry *entry) noexcept {
    if (!entry) {
        return;
    }

    entry->payload_ptr()->~SendPayload();
    entry->next = nullptr;
    entry->total_bytes = 0;
    entry->written_bytes = 0;
    entry->result = common::IoErr::None;
    entry->done_notified = false;
    entry->on_done = nullptr;
    entry->user_data = nullptr;

    if (free_send_entry_count_ < options_.max_free_send_entries) {
        entry->next = free_send_entries_;
        free_send_entries_ = entry;
        ++free_send_entry_count_;
        return;
    }

    delete entry;
}

common::IoErr Http2Connection::enqueue_send_stable_span(const std::uint8_t *data, std::size_t length,
                                                        SendEntry::DoneFn on_done, void *user_data) noexcept {
    SendEntry *entry = acquire_send_entry();
    if (!entry) {
        return common::IoErr::NoMem;
    }

    entry->payload_ptr()->set_stable_span(data, length);
    entry->total_bytes = length;
    entry->on_done = on_done;
    entry->user_data = user_data;
    common::IoErr result = enqueue_send_entry(entry);
    if (result != common::IoErr::None) {
        release_send_entry(entry);
    }
    return result;
}

common::IoErr Http2Connection::enqueue_send_buf(mem::IoBuf &&buf, SendEntry::DoneFn on_done, void *user_data) noexcept {
    SendEntry *entry = acquire_send_entry();
    if (!entry) {
        return common::IoErr::NoMem;
    }

    entry->payload_ptr()->set_buf(std::move(buf));
    entry->total_bytes = entry->payload_ptr()->readable_bytes();
    entry->on_done = on_done;
    entry->user_data = user_data;
    common::IoErr result = enqueue_send_entry(entry);
    if (result != common::IoErr::None) {
        release_send_entry(entry);
    }
    return result;
}

common::IoErr Http2Connection::enqueue_send_chain(mem::IoBufChain &&bufs, SendEntry::DoneFn on_done,
                                                  void *user_data) noexcept {
    SendEntry *entry = acquire_send_entry();
    if (!entry) {
        return common::IoErr::NoMem;
    }

    entry->payload_ptr()->set_chain(std::move(bufs));
    entry->total_bytes = entry->payload_ptr()->readable_bytes();
    entry->on_done = on_done;
    entry->user_data = user_data;
    common::IoErr result = enqueue_send_entry(entry);
    if (result != common::IoErr::None) {
        release_send_entry(entry);
    }
    return result;
}

void Http2Connection::stop_sending(common::IoErr reason) noexcept {
    stop_sending_requested_ = true;
    stop_sending_reason_ = reason;
    writer_state_ = WriterState::Stopping;

    if (transport_) {
        transport_->close();
    }

    if (!writer_running_) {
        drain_send_queue(reason);
    }
}

Http2Connection::WriterState Http2Connection::writer_state() const noexcept { return writer_state_; }

common::IoErr Http2Connection::enqueue_send_entry(SendEntry *entry) noexcept {
    if (!entry || !transport_ || !transport_->valid()) {
        return common::IoErr::Invalid;
    }
    if (stop_sending_requested_) {
        return stop_sending_reason_;
    }

    entry->next = nullptr;
    if (send_tail_) {
        send_tail_->next = entry;
    } else {
        send_head_ = entry;
    }
    send_tail_ = entry;

    start_send_loop();
    return common::IoErr::None;
}

void Http2Connection::finish_send_entry(SendEntry *entry, common::IoErr result) noexcept {
    if (!entry) {
        return;
    }

    entry->result = result;
    if (send_head_ == entry) {
        send_head_ = entry->next;
        if (!send_head_) {
            send_tail_ = nullptr;
        }
    } else {
        SendEntry *prev = send_head_;
        while (prev && prev->next != entry) {
            prev = prev->next;
        }
        if (prev) {
            prev->next = entry->next;
            if (send_tail_ == entry) {
                send_tail_ = prev;
            }
        }
    }

    if (sending_ == entry) {
        sending_ = nullptr;
    }

    entry->next = nullptr;
    notify_send_done(entry);
    release_send_entry(entry);
}

void Http2Connection::drain_send_queue(common::IoErr result) noexcept {
    while (send_head_) {
        finish_send_entry(send_head_, result);
    }
}

void Http2Connection::notify_send_done(SendEntry *entry) noexcept {
    if (!entry || entry->done_notified) {
        return;
    }

    entry->done_notified = true;
    if (entry->on_done) {
        entry->on_done(entry);
    }
}

} // namespace fiber::http
