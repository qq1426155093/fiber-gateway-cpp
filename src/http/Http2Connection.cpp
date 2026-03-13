#include "Http2Connection.h"

#include <algorithm>
#include <cstring>
#include <string_view>

#include "../async/Spawn.h"
#include "../common/Assert.h"

namespace fiber::http {

namespace {

constexpr std::string_view kClientPreface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
constexpr std::size_t kFrameHeaderSize = 9;
constexpr std::size_t kSettingsParameterSize = 6;
constexpr std::size_t kPingPayloadSize = 8;
constexpr std::size_t kWindowUpdatePayloadSize = 4;
constexpr std::size_t kRstStreamPayloadSize = 4;
constexpr std::uint16_t kSettingsHeaderTableSize = 0x1;
constexpr std::uint16_t kSettingsEnablePush = 0x2;
constexpr std::uint16_t kSettingsMaxConcurrentStreams = 0x3;
constexpr std::uint16_t kSettingsInitialWindowSize = 0x4;
constexpr std::uint16_t kSettingsMaxFrameSize = 0x5;
constexpr std::uint16_t kSettingsMaxHeaderListSize = 0x6;
constexpr std::uint8_t kFlagAck = 0x1;
constexpr std::uint8_t kFlagSettingsAck = 0x1;
constexpr std::int32_t kDefaultInitialWindowSize = 65535;
constexpr std::uint32_t kDefaultHeaderTableSize = 4096;
constexpr std::uint32_t kDefaultMaxFrameSize = 16384;
constexpr std::uint32_t kMaxFrameSizeLimit = 16777215;
constexpr std::int64_t kMaxFlowControlWindow = 0x7fffffffLL;
constexpr std::int32_t kInitialFlowControlWindow = 65535;

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

std::uint32_t parse_u32(const std::uint8_t *pos) noexcept {
    return (static_cast<std::uint32_t>(pos[0]) << 24) | (static_cast<std::uint32_t>(pos[1]) << 16) |
           (static_cast<std::uint32_t>(pos[2]) << 8) | static_cast<std::uint32_t>(pos[3]);
}

std::uint16_t parse_u16(const std::uint8_t *pos) noexcept {
    return (static_cast<std::uint16_t>(pos[0]) << 8) | static_cast<std::uint16_t>(pos[1]);
}

std::uint8_t *append_u16(std::uint8_t *out, std::uint16_t value) noexcept {
    out[0] = static_cast<std::uint8_t>((value >> 8) & 0xffU);
    out[1] = static_cast<std::uint8_t>(value & 0xffU);
    return out + 2;
}

std::uint8_t *append_u32(std::uint8_t *out, std::uint32_t value) noexcept {
    out[0] = static_cast<std::uint8_t>((value >> 24) & 0xffU);
    out[1] = static_cast<std::uint8_t>((value >> 16) & 0xffU);
    out[2] = static_cast<std::uint8_t>((value >> 8) & 0xffU);
    out[3] = static_cast<std::uint8_t>(value & 0xffU);
    return out + 4;
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
    transport_(std::move(transport)), options_(std::move(options)), pending_pool_(options_.max_free_pending_entries) {
    options_.expect_peer_preface = options_.role == ConnectionRole::Server;
    peer_advertised_max_concurrent_streams_ = options_.max_peer_concurrent_streams;
    conn_send_window_ = options_.initial_connection_send_window;
    peer_initial_stream_send_window_ = options_.initial_stream_send_window;
    peer_header_table_size_ = kDefaultHeaderTableSize;
    peer_max_outbound_frame_size_ = options_.max_frame_size;
    FIBER_ASSERT(streams_.init(configured_max_active_streams()));
}

Http2Connection::~Http2Connection() {
    if (!writer_running_) {
        drain_send_queue(common::IoErr::Canceled);
    }
    drain_pending_entries(common::IoErr::Canceled);

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

    if (options_.role == ConnectionRole::Client && options_.auto_start_connection_preface &&
        !local_connection_preface_sent_) {
        common::IoErr err = send_connection_preface();
        if (err != common::IoErr::None) {
            co_return std::unexpected(err);
        }
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
                if (options_.auto_start_connection_preface && !local_connection_preface_sent_) {
                    common::IoErr err = send_connection_preface();
                    if (err != common::IoErr::None) {
                        co_return std::unexpected(err);
                    }
                }
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
                    common::IoErr err = consume_incoming_frame_payload(current_header, read_buf, 0, 0);
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
            common::IoErr err = consume_incoming_frame_payload(current_header, read_buf, payload_offset, chunk_len);
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

common::IoErr Http2Connection::consume_incoming_frame_payload(const FrameHeader &fhr, const mem::IoBuf &buf,
                                                              std::size_t offset, std::size_t length) noexcept {
    switch (fhr.type) {
        case Http2FrameType::Settings:
            return handle_settings_payload(fhr, buf, offset, length);
        case Http2FrameType::Ping:
            return handle_ping_payload(fhr, buf, offset, length);
        case Http2FrameType::WindowUpdate:
            return handle_window_update_payload(fhr, buf, offset, length);
        case Http2FrameType::RstStream:
            return handle_rst_stream_payload(fhr, buf, offset, length);
        default:
            return on_frame_payload(fhr, buf, offset, length);
    }
}

common::IoErr Http2Connection::handle_settings_payload(const FrameHeader &fhr, const mem::IoBuf &buf, std::size_t offset,
                                                       std::size_t length) noexcept {
    if (offset == 0) {
        settings_scratch_used_ = 0;
        if (fhr.stream_id != 0) {
            return common::IoErr::Invalid;
        }
        if ((fhr.flags & kFlagSettingsAck) != 0) {
            if (fhr.length != 0) {
                return common::IoErr::Invalid;
            }
            local_settings_acknowledged_ = true;
            return common::IoErr::None;
        }
        if ((fhr.length % kSettingsParameterSize) != 0) {
            return common::IoErr::Invalid;
        }
    }

    if ((fhr.flags & kFlagSettingsAck) != 0) {
        return common::IoErr::None;
    }

    const std::uint8_t *pos = buf.readable_data();
    std::size_t remaining = length;
    while (remaining != 0) {
        std::size_t take = std::min<std::size_t>(remaining, kSettingsParameterSize - settings_scratch_used_);
        std::memcpy(settings_scratch_.data() + settings_scratch_used_, pos, take);
        settings_scratch_used_ += take;
        pos += take;
        remaining -= take;

        if (settings_scratch_used_ == kSettingsParameterSize) {
            std::uint16_t id = parse_u16(settings_scratch_.data());
            std::uint32_t value = parse_u32(settings_scratch_.data() + 2);
            common::IoErr err = apply_settings_parameter(id, value);
            if (err != common::IoErr::None) {
                return err;
            }
            settings_scratch_used_ = 0;
        }
    }

    if (offset + length == fhr.length) {
        if (settings_scratch_used_ != 0) {
            return common::IoErr::Invalid;
        }
        return send_settings_ack();
    }

    return common::IoErr::None;
}

common::IoErr Http2Connection::handle_ping_payload(const FrameHeader &fhr, const mem::IoBuf &buf, std::size_t offset,
                                                   std::size_t length) noexcept {
    if (offset == 0) {
        control_payload_used_ = 0;
        if (fhr.stream_id != 0 || fhr.length != kPingPayloadSize) {
            return common::IoErr::Invalid;
        }
    }

    if (length != 0) {
        std::memcpy(control_payload_scratch_.data() + control_payload_used_, buf.readable_data(), length);
        control_payload_used_ += length;
    }

    if (offset + length != fhr.length) {
        return common::IoErr::None;
    }

    if ((fhr.flags & kFlagAck) != 0) {
        return common::IoErr::None;
    }

    return send_ping_ack(control_payload_scratch_.data());
}

common::IoErr Http2Connection::handle_window_update_payload(const FrameHeader &fhr, const mem::IoBuf &buf,
                                                            std::size_t offset, std::size_t length) noexcept {
    if (offset == 0) {
        control_payload_used_ = 0;
        if (fhr.length != kWindowUpdatePayloadSize) {
            return common::IoErr::Invalid;
        }
    }

    if (length != 0) {
        std::memcpy(control_payload_scratch_.data() + control_payload_used_, buf.readable_data(), length);
        control_payload_used_ += length;
    }

    if (offset + length != fhr.length) {
        return common::IoErr::None;
    }

    std::uint32_t increment = parse_u32(control_payload_scratch_.data()) & 0x7fffffffU;
    if (increment == 0) {
        if (fhr.stream_id == 0) {
            return common::IoErr::Invalid;
        }
        handle_stream_error(fhr.stream_id, Http2ErrorCode::ProtocolError, common::IoErr::Invalid);
        return common::IoErr::None;
    }

    if (fhr.stream_id == 0) {
        std::int64_t next_window = static_cast<std::int64_t>(conn_send_window_) + static_cast<std::int64_t>(increment);
        if (next_window > kMaxFlowControlWindow) {
            return common::IoErr::Invalid;
        }
        update_connection_send_window(static_cast<std::int32_t>(increment));
        return common::IoErr::None;
    }

    Http2Stream *stream = streams_.find(fhr.stream_id);
    if (!stream) {
        return is_idle_stream(fhr.stream_id) ? common::IoErr::Invalid : common::IoErr::None;
    }

    std::int64_t next_window = static_cast<std::int64_t>(stream->send_window_) + static_cast<std::int64_t>(increment);
    if (next_window > kMaxFlowControlWindow) {
        handle_stream_error(fhr.stream_id, Http2ErrorCode::FlowControlError, common::IoErr::Invalid);
        return common::IoErr::None;
    }

    update_stream_send_window(*stream, static_cast<std::int32_t>(increment));
    return common::IoErr::None;
}

common::IoErr Http2Connection::handle_rst_stream_payload(const FrameHeader &fhr, const mem::IoBuf &buf, std::size_t offset,
                                                         std::size_t length) noexcept {
    if (offset == 0) {
        control_payload_used_ = 0;
        if (fhr.stream_id == 0 || fhr.length != kRstStreamPayloadSize) {
            return common::IoErr::Invalid;
        }
    }

    if (length != 0) {
        std::memcpy(control_payload_scratch_.data() + control_payload_used_, buf.readable_data(), length);
        control_payload_used_ += length;
    }

    if (offset + length != fhr.length) {
        return common::IoErr::None;
    }

    Http2Stream *stream = streams_.find(fhr.stream_id);
    if (!stream) {
        return is_idle_stream(fhr.stream_id) ? common::IoErr::Invalid : common::IoErr::None;
    }

    stream->close(common::IoErr::Canceled);
    (void)streams_.erase(fhr.stream_id);
    return common::IoErr::None;
}

common::IoErr Http2Connection::apply_settings_parameter(std::uint16_t id, std::uint32_t value) noexcept {
    switch (id) {
        case kSettingsHeaderTableSize:
            peer_header_table_size_ = value;
            return common::IoErr::None;
        case kSettingsEnablePush:
            if (value > 1) {
                return common::IoErr::Invalid;
            }
            peer_enable_push_ = value != 0;
            return common::IoErr::None;
        case kSettingsMaxConcurrentStreams:
            peer_advertised_max_concurrent_streams_ = value;
            return common::IoErr::None;
        case kSettingsInitialWindowSize:
            return apply_peer_initial_stream_window(value);
        case kSettingsMaxFrameSize:
            if (value < kDefaultMaxFrameSize || value > kMaxFrameSizeLimit) {
                return common::IoErr::Invalid;
            }
            peer_max_outbound_frame_size_ = value;
            return common::IoErr::None;
        case kSettingsMaxHeaderListSize:
            peer_max_header_list_size_ = value;
            return common::IoErr::None;
        default:
            return common::IoErr::None;
    }
}

common::IoErr Http2Connection::apply_peer_initial_stream_window(std::uint32_t value) noexcept {
    if (value > static_cast<std::uint32_t>(kMaxFlowControlWindow)) {
        return common::IoErr::Invalid;
    }

    std::int64_t delta = static_cast<std::int64_t>(value) - static_cast<std::int64_t>(peer_initial_stream_send_window_);
    if (delta != 0) {
        common::IoErr err = common::IoErr::None;
        streams_.for_each([&](Http2Stream &stream) {
            if (err != common::IoErr::None) {
                return;
            }
            std::int64_t next_window = static_cast<std::int64_t>(stream.send_window()) + delta;
            if (next_window > kMaxFlowControlWindow || next_window < -kMaxFlowControlWindow - 1) {
                err = common::IoErr::Invalid;
            }
        });
        if (err != common::IoErr::None) {
            return err;
        }

        streams_.for_each([&](Http2Stream &stream) {
            stream.update_send_window(static_cast<std::int32_t>(delta));
        });
    }

    peer_initial_stream_send_window_ = static_cast<std::int32_t>(value);
    return common::IoErr::None;
}

common::IoErr Http2Connection::send_control_frame(Http2FrameType type, std::uint8_t flags, std::uint32_t stream_id,
                                                  const std::uint8_t *payload, std::size_t length) noexcept {
    mem::IoBuf buf = mem::IoBuf::allocate(kFrameHeaderSize + length);
    if (!buf) {
        return common::IoErr::NoMem;
    }

    encode_frame_header(buf.writable_data(), static_cast<std::uint32_t>(length), type, flags, stream_id);
    buf.commit(kFrameHeaderSize);
    if (length != 0) {
        std::memcpy(buf.writable_data(), payload, length);
        buf.commit(length);
    }
    return enqueue_send_buf(std::move(buf), nullptr);
}

common::IoErr Http2Connection::send_connection_preface() noexcept {
    constexpr std::size_t kSettingsCount = 3;
    constexpr std::size_t kSettingsPayloadSize = kSettingsCount * kSettingsParameterSize;
    bool send_client_preface = options_.role == ConnectionRole::Client;
    bool send_conn_window_update = options_.initial_connection_recv_window > static_cast<std::uint32_t>(kInitialFlowControlWindow);
    std::size_t total_size = kFrameHeaderSize + kSettingsPayloadSize;
    if (send_client_preface) {
        total_size += kClientPreface.size();
    }
    if (send_conn_window_update) {
        total_size += kFrameHeaderSize + kWindowUpdatePayloadSize;
    }

    mem::IoBuf buf = mem::IoBuf::allocate(total_size);
    if (!buf) {
        return common::IoErr::NoMem;
    }
    std::uint8_t *out = buf.writable_data();

    if (send_client_preface) {
        std::memcpy(out, kClientPreface.data(), kClientPreface.size());
        out += kClientPreface.size();
    }

    encode_frame_header(out, static_cast<std::uint32_t>(kSettingsPayloadSize), Http2FrameType::Settings, 0, 0);
    out += kFrameHeaderSize;
    out = append_u16(out, kSettingsMaxConcurrentStreams);
    out = append_u32(out, options_.local_max_concurrent_streams);
    out = append_u16(out, kSettingsInitialWindowSize);
    out = append_u32(out, static_cast<std::uint32_t>(options_.initial_stream_send_window));
    out = append_u16(out, kSettingsMaxFrameSize);
    out = append_u32(out, options_.max_frame_size);

    if (send_conn_window_update) {
        std::uint32_t increment = options_.initial_connection_recv_window - static_cast<std::uint32_t>(kInitialFlowControlWindow);
        encode_frame_header(out, kWindowUpdatePayloadSize, Http2FrameType::WindowUpdate, 0, 0);
        out += kFrameHeaderSize;
        out = append_u32(out, increment & 0x7fffffffU);
    }
    buf.commit(static_cast<std::size_t>(out - buf.writable_data()));

    common::IoErr err = enqueue_send_buf(std::move(buf), nullptr);
    if (err == common::IoErr::None) {
        local_connection_preface_sent_ = true;
        local_settings_acknowledged_ = false;
    }
    return err;
}

common::IoErr Http2Connection::send_settings_ack() noexcept {
    return send_control_frame(Http2FrameType::Settings, kFlagSettingsAck, 0, nullptr, 0);
}

common::IoErr Http2Connection::send_ping_ack(const std::uint8_t *opaque_data) noexcept {
    return send_control_frame(Http2FrameType::Ping, kFlagAck, 0, opaque_data, kPingPayloadSize);
}

common::IoErr Http2Connection::send_window_update(std::uint32_t stream_id, std::uint32_t increment) noexcept {
    if (increment == 0 || increment > static_cast<std::uint32_t>(kMaxFlowControlWindow)) {
        return common::IoErr::Invalid;
    }

    std::uint8_t payload[kWindowUpdatePayloadSize];
    payload[0] = static_cast<std::uint8_t>((increment >> 24) & 0x7fU);
    payload[1] = static_cast<std::uint8_t>((increment >> 16) & 0xffU);
    payload[2] = static_cast<std::uint8_t>((increment >> 8) & 0xffU);
    payload[3] = static_cast<std::uint8_t>(increment & 0xffU);
    return send_control_frame(Http2FrameType::WindowUpdate, 0, stream_id, payload, sizeof(payload));
}

common::IoErr Http2Connection::send_rst_stream(std::uint32_t stream_id, Http2ErrorCode error_code) noexcept {
    std::uint8_t payload[kRstStreamPayloadSize];
    std::uint32_t value = static_cast<std::uint32_t>(error_code);
    payload[0] = static_cast<std::uint8_t>((value >> 24) & 0xffU);
    payload[1] = static_cast<std::uint8_t>((value >> 16) & 0xffU);
    payload[2] = static_cast<std::uint8_t>((value >> 8) & 0xffU);
    payload[3] = static_cast<std::uint8_t>(value & 0xffU);
    return send_control_frame(Http2FrameType::RstStream, 0, stream_id, payload, sizeof(payload));
}

void Http2Connection::handle_stream_error(std::uint32_t stream_id, Http2ErrorCode error_code,
                                          common::IoErr pending_result) noexcept {
    (void)send_rst_stream(stream_id, error_code);

    Http2Stream *stream = streams_.find(stream_id);
    if (!stream) {
        return;
    }

    stream->close(pending_result);
    (void)streams_.erase(stream_id);
}

bool Http2Connection::is_idle_stream(std::uint32_t stream_id) const noexcept {
    if (stream_id == 0 || streams_.find(stream_id)) {
        return false;
    }
    if (is_local_stream_id(stream_id)) {
        return stream_id > last_local_stream_id_;
    }
    if (is_peer_stream_id(stream_id)) {
        return stream_id > last_peer_stream_id_;
    }
    return false;
}

bool Http2Connection::is_local_stream_id(std::uint32_t stream_id) const noexcept {
    if (stream_id == 0) {
        return false;
    }
    bool odd = (stream_id & 1U) != 0;
    if (options_.role == ConnectionRole::Client) {
        return odd;
    }
    return !odd;
}

bool Http2Connection::is_peer_stream_id(std::uint32_t stream_id) const noexcept {
    if (stream_id == 0) {
        return false;
    }
    return !is_local_stream_id(stream_id);
}

Http2Connection::SendPayload *Http2Connection::SendEntry::payload_ptr() noexcept {
    return std::launder(reinterpret_cast<SendPayload *>(payload_storage_));
}

const Http2Connection::SendPayload *Http2Connection::SendEntry::payload_ptr() const noexcept {
    return std::launder(reinterpret_cast<const SendPayload *>(payload_storage_));
}

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
        if (entry->frame_header_size == entry->written_bytes && entry->payload_ptr()->empty()) {
            finish_send_entry(entry, common::IoErr::None);
            continue;
        }

        writer_state_ = WriterState::Writing;
        common::IoResult<size_t> write_result = static_cast<size_t>(0);
        if (entry->written_bytes < entry->frame_header_size) {
            std::size_t header_offset = entry->written_bytes;
            write_result = co_await transport_->write(entry->frame_header_ + header_offset,
                                                      entry->frame_header_size - header_offset, options_.write_timeout);
        } else {
            write_result = co_await entry->payload_ptr()->write_once(*transport_, options_.write_timeout);
        }
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
        if (entry->written_bytes >= entry->frame_header_size && entry->payload_ptr()->empty()) {
            finish_send_entry(entry, common::IoErr::None);
        }
    }

    sending_ = nullptr;
    writer_running_ = false;
    if (stop_sending_requested_) {
        drain_pending_entries(stop_sending_reason_);
    }
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
    entry->frame_header_size = 0;
    std::memset(entry->frame_header_, 0, sizeof(entry->frame_header_));
    entry->logical_bytes = 0;
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
    entry->frame_header_size = 0;
    std::memset(entry->frame_header_, 0, sizeof(entry->frame_header_));
    entry->logical_bytes = 0;
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

common::IoErr Http2Connection::bind_stream(Http2Stream &stream) noexcept {
    if (stream.stream_id_ == 0 || !transport_ || !transport_->valid()) {
        return common::IoErr::Invalid;
    }
    if (stop_sending_requested_) {
        return stop_sending_reason_;
    }
    if (stream.conn_ && stream.conn_ != this) {
        return common::IoErr::Invalid;
    }

    if (!streams_.find(stream.stream_id_)) {
        if (!streams_.insert(stream)) {
            return common::IoErr::Busy;
        }
        stream.send_window_ = peer_initial_stream_send_window_;
    }

    stream.conn_ = this;
    stream.active_ = true;
    if (stream.state_ == Http2Stream::State::Idle) {
        stream.state_ = Http2Stream::State::Open;
    }
    if (stream.stream_id_ > last_local_stream_id_) {
        last_local_stream_id_ = stream.stream_id_;
    }
    return common::IoErr::None;
}

void Http2Connection::update_connection_send_window(std::int32_t delta) noexcept {
    conn_send_window_ += delta;
    drain_conn_blocked_streams();
}

void Http2Connection::update_stream_send_window(Http2Stream &stream, std::int32_t delta) noexcept {
    stream.update_send_window(delta);
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
    drain_pending_entries(reason);
}

Http2Connection::WriterState Http2Connection::writer_state() const noexcept { return writer_state_; }

std::size_t Http2Connection::configured_max_active_streams() const noexcept {
    return static_cast<std::size_t>(options_.max_peer_concurrent_streams) +
           static_cast<std::size_t>(options_.max_local_push_streams);
}

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
        entry->on_done(entry->user_data, entry->total_bytes, entry->written_bytes, entry->frame_header_size,
                       entry->logical_bytes, entry->result);
    }
}

void Http2Connection::drain_pending_entries(common::IoErr result) noexcept {
    streams_.for_each([&](Http2Stream &stream) { stream.drain_pending(result); });
}

void Http2Connection::drain_conn_blocked_streams() noexcept {
    if (stop_sending_requested_) {
        return;
    }

    bool progress = false;
    do {
        progress = false;
        Http2Stream *stream = conn_wait_head_;
        while (stream) {
            Http2Stream *next = stream->conn_wait_next_;
            remove_conn_wait_stream(*stream);
            Http2Stream::ScheduleResult result = stream->schedule_pending();
            if (result == Http2Stream::ScheduleResult::Scheduled) {
                progress = true;
            }
            if (stream->blocked_by_conn_window()) {
                append_conn_wait_stream(*stream);
            }
            stream = next;
            if (stop_sending_requested_ || conn_send_window_ <= 0) {
                break;
            }
        }
    } while (progress && conn_send_window_ > 0 && conn_wait_head_);
}

void Http2Connection::append_conn_wait_stream(Http2Stream &stream) noexcept {
    if (stream.in_conn_window_wait_list_) {
        return;
    }
    stream.conn_wait_prev_ = conn_wait_tail_;
    stream.conn_wait_next_ = nullptr;
    if (conn_wait_tail_) {
        conn_wait_tail_->conn_wait_next_ = &stream;
    } else {
        conn_wait_head_ = &stream;
    }
    conn_wait_tail_ = &stream;
    stream.in_conn_window_wait_list_ = true;
}

void Http2Connection::remove_conn_wait_stream(Http2Stream &stream) noexcept {
    if (!stream.in_conn_window_wait_list_) {
        return;
    }
    if (stream.conn_wait_prev_) {
        stream.conn_wait_prev_->conn_wait_next_ = stream.conn_wait_next_;
    } else {
        conn_wait_head_ = stream.conn_wait_next_;
    }
    if (stream.conn_wait_next_) {
        stream.conn_wait_next_->conn_wait_prev_ = stream.conn_wait_prev_;
    } else {
        conn_wait_tail_ = stream.conn_wait_prev_;
    }
    stream.conn_wait_prev_ = nullptr;
    stream.conn_wait_next_ = nullptr;
    stream.in_conn_window_wait_list_ = false;
}

void Http2Connection::encode_frame_header(std::uint8_t *out, std::uint32_t length, Http2FrameType type,
                                          std::uint8_t flags, std::uint32_t stream_id) noexcept {
    out[0] = static_cast<std::uint8_t>((length >> 16) & 0xffU);
    out[1] = static_cast<std::uint8_t>((length >> 8) & 0xffU);
    out[2] = static_cast<std::uint8_t>(length & 0xffU);
    out[3] = static_cast<std::uint8_t>(type);
    out[4] = flags;
    out[5] = static_cast<std::uint8_t>((stream_id >> 24) & 0x7fU);
    out[6] = static_cast<std::uint8_t>((stream_id >> 16) & 0xffU);
    out[7] = static_cast<std::uint8_t>((stream_id >> 8) & 0xffU);
    out[8] = static_cast<std::uint8_t>(stream_id & 0xffU);
}

} // namespace fiber::http
