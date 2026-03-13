#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <future>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "async/Sleep.h"
#include "async/Spawn.h"
#include "common/IoError.h"
#include "event/EventLoopGroup.h"
#include "http/Http2Connection.h"
#include "http/Http2Stream.h"

namespace {

using fiber::async::DetachedTask;

constexpr std::string_view kClientConnectionPreface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

class FakeHttpTransport final : public fiber::http::HttpTransport {
public:
    explicit FakeHttpTransport(std::vector<std::string> chunks, std::vector<size_t> write_steps = {}) :
        chunks_(std::move(chunks)), write_steps_(std::move(write_steps)) {}

    fiber::async::Task<fiber::common::IoResult<void>> handshake(std::chrono::milliseconds) override {
        co_return fiber::common::IoResult<void>{};
    }

    fiber::async::Task<fiber::common::IoResult<void>> shutdown(std::chrono::milliseconds) override {
        co_return fiber::common::IoResult<void>{};
    }

    fiber::async::Task<fiber::common::IoResult<size_t>> read(void *buf, size_t len, std::chrono::milliseconds) override {
        if (next_chunk_ >= chunks_.size()) {
            co_return static_cast<size_t>(0);
        }
        const std::string &chunk = chunks_[next_chunk_++];
        size_t take = std::min(len, chunk.size());
        std::memcpy(buf, chunk.data(), take);
        co_return take;
    }

    fiber::async::Task<fiber::common::IoResult<size_t>> read_into(fiber::mem::IoBuf &buf,
                                                                  std::chrono::milliseconds) override {
        if (next_chunk_ >= chunks_.size()) {
            co_return static_cast<size_t>(0);
        }
        const std::string &chunk = chunks_[next_chunk_++];
        size_t take = std::min(buf.writable(), chunk.size());
        std::memcpy(buf.writable_data(), chunk.data(), take);
        buf.commit(take);
        co_return take;
    }

    fiber::async::Task<fiber::common::IoResult<size_t>> readv_into(fiber::mem::IoBufChain &,
                                                                   std::chrono::milliseconds) override {
        co_return std::unexpected(fiber::common::IoErr::NotSupported);
    }

    fiber::async::Task<fiber::common::IoResult<size_t>> write(const void *buf, size_t len,
                                                              std::chrono::milliseconds) override {
        if (closed_) {
            co_return std::unexpected(fiber::common::IoErr::ConnReset);
        }
        size_t take = next_write_size(len);
        const auto *ptr = static_cast<const char *>(buf);
        written_.append(ptr, ptr + take);
        ++write_call_count_;
        co_return take;
    }

    fiber::async::Task<fiber::common::IoResult<size_t>> write(fiber::mem::IoBuf &buf, std::chrono::milliseconds timeout) override {
        auto result = co_await write(buf.readable_data(), buf.readable(), timeout);
        if (result) {
            buf.consume(*result);
        }
        co_return result;
    }

    fiber::async::Task<fiber::common::IoResult<size_t>> writev(fiber::mem::IoBufChain &buf,
                                                               std::chrono::milliseconds) override {
        if (closed_) {
            co_return std::unexpected(fiber::common::IoErr::ConnReset);
        }

        size_t take = next_write_size(buf.readable_bytes());
        std::array<iovec, 16> iov{};
        int count = buf.fill_write_iov(iov.data(), static_cast<int>(iov.size()));
        size_t remaining = take;
        for (int i = 0; i < count && remaining > 0; ++i) {
            size_t chunk = std::min<std::size_t>(iov[i].iov_len, remaining);
            const char *ptr = static_cast<const char *>(iov[i].iov_base);
            written_.append(ptr, ptr + chunk);
            remaining -= chunk;
        }
        buf.consume_and_compact(take);
        ++write_call_count_;
        co_return take;
    }

    void close() override { closed_ = true; }

    [[nodiscard]] bool valid() const noexcept override { return !closed_; }
    [[nodiscard]] int fd() const noexcept override { return -1; }
    [[nodiscard]] std::string negotiated_alpn() const noexcept override { return "h2"; }
    [[nodiscard]] const fiber::net::SocketAddress &remote_addr() const noexcept override { return remote_addr_; }
    [[nodiscard]] const std::string &written() const noexcept { return written_; }

private:
    size_t next_write_size(size_t available) noexcept {
        if (available == 0) {
            return 0;
        }
        if (write_call_count_ < write_steps_.size()) {
            return std::min(available, write_steps_[write_call_count_]);
        }
        return available;
    }

    std::vector<std::string> chunks_;
    std::vector<size_t> write_steps_;
    size_t next_chunk_ = 0;
    size_t write_call_count_ = 0;
    bool closed_ = false;
    std::string written_;
    fiber::net::SocketAddress remote_addr_{};
};

struct ObservedChunk {
    fiber::http::Http2Connection::FrameHeader header{};
    std::size_t offset = 0;
    std::size_t length = 0;
    fiber::mem::IoBuf payload{};
};

class RecordingHttp2Connection final : public fiber::http::Http2Connection {
public:
    RecordingHttp2Connection(std::unique_ptr<fiber::http::HttpTransport> transport, Options options) :
        fiber::http::Http2Connection(std::move(transport), options) {}

    const std::vector<ObservedChunk> &chunks() const noexcept { return chunks_; }

    void set_payload_error(fiber::common::IoErr err) noexcept { payload_error_ = err; }

protected:
    fiber::common::IoErr on_frame_payload(const FrameHeader &fhr, const fiber::mem::IoBuf &buf, std::size_t offset,
                                          std::size_t length) noexcept override {
        ObservedChunk chunk;
        chunk.header = fhr;
        chunk.offset = offset;
        chunk.length = length;
        if (length != 0) {
            chunk.payload = buf.retain_slice(0, length);
            if (!chunk.payload) {
                return fiber::common::IoErr::NoMem;
            }
        }
        chunks_.push_back(std::move(chunk));
        return payload_error_;
    }

private:
    std::vector<ObservedChunk> chunks_;
    fiber::common::IoErr payload_error_ = fiber::common::IoErr::None;
};

struct RunOutcome {
    fiber::common::IoResult<void> result;
    std::vector<ObservedChunk> chunks;
};

struct CompletedSend {
    std::size_t total_bytes = 0;
    std::size_t written_bytes = 0;
    fiber::common::IoErr result = fiber::common::IoErr::None;
};

struct PendingEvent {
    fiber::http::Http2PendingChange::Kind kind = fiber::http::Http2PendingChange::Kind::Scheduled;
    std::size_t delta_bytes = 0;
    std::size_t total_bytes = 0;
    std::size_t written_bytes = 0;
    fiber::common::IoErr result = fiber::common::IoErr::None;
};

struct SendOutcome {
    fiber::common::IoErr submit_error = fiber::common::IoErr::None;
    fiber::http::Http2Connection::WriterState writer_state = fiber::http::Http2Connection::WriterState::WaitingForData;
    std::vector<CompletedSend> completions;
    std::string written;
};

struct PendingOutcome {
    fiber::common::IoErr submit_error = fiber::common::IoErr::None;
    fiber::http::Http2Connection::WriterState writer_state = fiber::http::Http2Connection::WriterState::WaitingForData;
    std::vector<PendingEvent> events;
    std::string written;
};

struct ControlRunOutcome {
    fiber::common::IoResult<void> result;
    std::string written;
    fiber::http::Http2Connection::WriterState writer_state = fiber::http::Http2Connection::WriterState::WaitingForData;
    std::int32_t conn_send_window = 0;
    std::uint32_t peer_max_frame_size = 0;
    std::uint32_t peer_max_concurrent_streams = 0;
    bool peer_enable_push = true;
    std::int32_t stream1_send_window = 0;
    fiber::http::Http2Stream::State stream1_state = fiber::http::Http2Stream::State::Idle;
    bool stream1_registered = false;
};

std::string iobuf_to_string(const fiber::mem::IoBuf &buf) {
    return std::string(reinterpret_cast<const char *>(buf.readable_data()), buf.readable());
}

std::string make_frame(std::uint32_t length, std::uint8_t type, std::uint8_t flags, std::uint32_t stream_id,
                       std::string_view payload) {
    EXPECT_EQ(length, payload.size());

    std::string out;
    out.resize(9 + payload.size());
    out[0] = static_cast<char>((length >> 16) & 0xffU);
    out[1] = static_cast<char>((length >> 8) & 0xffU);
    out[2] = static_cast<char>(length & 0xffU);
    out[3] = static_cast<char>(type);
    out[4] = static_cast<char>(flags);
    out[5] = static_cast<char>((stream_id >> 24) & 0x7fU);
    out[6] = static_cast<char>((stream_id >> 16) & 0xffU);
    out[7] = static_cast<char>((stream_id >> 8) & 0xffU);
    out[8] = static_cast<char>(stream_id & 0xffU);
    std::memcpy(out.data() + 9, payload.data(), payload.size());
    return out;
}

struct EncodedFrame {
    std::uint32_t length = 0;
    std::uint8_t type = 0;
    std::uint8_t flags = 0;
    std::uint32_t stream_id = 0;
    std::string payload;
};

std::vector<EncodedFrame> parse_frames(std::string_view data) {
    std::vector<EncodedFrame> frames;
    std::size_t pos = 0;
    while (pos + 9 <= data.size()) {
        EncodedFrame frame;
        frame.length = (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[pos])) << 16) |
                       (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[pos + 1])) << 8) |
                       static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[pos + 2]));
        frame.type = static_cast<std::uint8_t>(data[pos + 3]);
        frame.flags = static_cast<std::uint8_t>(data[pos + 4]);
        frame.stream_id = ((static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[pos + 5])) & 0x7fU) << 24) |
                          (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[pos + 6])) << 16) |
                          (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[pos + 7])) << 8) |
                          static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[pos + 8]));
        pos += 9;
        if (pos + frame.length > data.size()) {
            break;
        }
        frame.payload.assign(data.data() + pos, frame.length);
        pos += frame.length;
        frames.push_back(std::move(frame));
    }
    return frames;
}

std::string describe_frames(const std::vector<EncodedFrame> &frames) {
    std::ostringstream out;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        if (i != 0) {
            out << " | ";
        }
        out << "#" << i << " type=" << static_cast<int>(frames[i].type) << " sid=" << frames[i].stream_id
            << " len=" << frames[i].length << " payload=" << frames[i].payload;
    }
    return out.str();
}

DetachedTask run_http2_connection(std::shared_ptr<std::promise<RunOutcome>> promise, std::vector<std::string> chunks,
                                  fiber::http::Http2Connection::Options options,
                                  fiber::common::IoErr payload_error = fiber::common::IoErr::None) {
    options.auto_start_connection_preface = false;
    auto transport = std::make_unique<FakeHttpTransport>(std::move(chunks));
    RecordingHttp2Connection connection(std::move(transport), options);
    connection.set_payload_error(payload_error);

    RunOutcome outcome;
    outcome.result = co_await connection.run();
    outcome.chunks = connection.chunks();
    promise->set_value(std::move(outcome));

    fiber::event::EventLoop::current().stop();
    co_return;
}

RunOutcome execute_connection(std::vector<std::string> chunks, fiber::http::Http2Connection::Options options = {},
                              fiber::common::IoErr payload_error = fiber::common::IoErr::None) {
    fiber::event::EventLoopGroup group(1);
    auto promise = std::make_shared<std::promise<RunOutcome>>();
    auto future = promise->get_future();

    group.start();
    fiber::async::spawn(group.at(0), [promise = std::move(promise), chunks = std::move(chunks), options,
                                      payload_error]() mutable {
        return run_http2_connection(std::move(promise), std::move(chunks), options, payload_error);
    });

    auto status = future.wait_for(std::chrono::seconds(2));
    if (status != std::future_status::ready) {
        group.stop();
        group.join();
        ADD_FAILURE() << "Timed out waiting for http2 connection task";
        return {};
    }

    RunOutcome outcome = future.get();
    group.join();
    return outcome;
}

class SendingHttp2Connection final : public fiber::http::Http2Connection {
public:
    SendingHttp2Connection(std::unique_ptr<fiber::http::HttpTransport> transport, FakeHttpTransport *fake_transport,
                           std::size_t expected_done, Options options = {}) :
        fiber::http::Http2Connection(std::move(transport), options), fake_transport_(fake_transport),
        expected_done_(expected_done) {}

    fiber::common::IoErr submit_stable_span(std::string_view data) noexcept {
        return enqueue_send_stable_span(reinterpret_cast<const std::uint8_t *>(data.data()), data.size(),
                                        &SendingHttp2Connection::handle_done, this);
    }

    fiber::common::IoErr submit_buf(fiber::mem::IoBuf &&buf) noexcept {
        return enqueue_send_buf(std::move(buf), &SendingHttp2Connection::handle_done, this);
    }

    fiber::common::IoErr submit_chain(fiber::mem::IoBufChain &&bufs) noexcept {
        return enqueue_send_chain(std::move(bufs), &SendingHttp2Connection::handle_done, this);
    }

    void request_stop(fiber::common::IoErr reason = fiber::common::IoErr::Canceled) noexcept { stop_sending(reason); }

    [[nodiscard]] bool done() const noexcept { return done_; }

    SendOutcome snapshot() const {
        SendOutcome outcome;
        outcome.writer_state = writer_state();
        outcome.completions = completions_;
        if (fake_transport_) {
            outcome.written = fake_transport_->written();
        }
        return outcome;
    }

private:
    static void handle_done(void *user_data, std::size_t total_bytes, std::size_t written_bytes, std::size_t,
                            std::size_t, fiber::common::IoErr result) noexcept {
        auto *self = static_cast<SendingHttp2Connection *>(user_data);
        self->record_done(total_bytes, written_bytes, result);
    }

    void record_done(std::size_t total_bytes, std::size_t written_bytes, fiber::common::IoErr result) noexcept {
        completions_.push_back({total_bytes, written_bytes, result});
        if (completions_.size() >= expected_done_) {
            done_ = true;
        }
    }

    FakeHttpTransport *fake_transport_ = nullptr;
    std::size_t expected_done_ = 0;
    bool done_ = false;
    std::vector<CompletedSend> completions_;
};

class PendingHttp2Connection final : public fiber::http::Http2Connection {
public:
    PendingHttp2Connection(std::unique_ptr<fiber::http::HttpTransport> transport, FakeHttpTransport *fake_transport,
                           std::size_t expected_terminal_events, Options options = {}) :
        fiber::http::Http2Connection(std::move(transport), options), fake_transport_(fake_transport),
        expected_terminal_events_(expected_terminal_events) {}

    fiber::common::IoErr submit_header(fiber::http::Http2Stream &stream, std::string_view data, std::uint8_t first_flags = 0,
                                       std::uint8_t last_flags = 0) noexcept {
        SendPayload payload;
        payload.set_stable_span(reinterpret_cast<const std::uint8_t *>(data.data()), data.size());
        return stream.enqueue_pending(*this, PendingKind::Header, std::move(payload), first_flags, last_flags,
                                      &PendingHttp2Connection::handle_change, this);
    }

    fiber::common::IoErr submit_data(fiber::http::Http2Stream &stream, std::string_view data,
                                     std::uint8_t last_flags = 0) noexcept {
        SendPayload payload;
        payload.set_stable_span(reinterpret_cast<const std::uint8_t *>(data.data()), data.size());
        return stream.enqueue_pending(*this, PendingKind::Data, std::move(payload), 0, last_flags,
                                      &PendingHttp2Connection::handle_change, this);
    }

    void add_connection_credit(std::int32_t delta) noexcept { update_connection_send_window(delta); }
    void add_stream_credit(fiber::http::Http2Stream &stream, std::int32_t delta) noexcept { update_stream_send_window(stream, delta); }

    [[nodiscard]] bool done() const noexcept { return done_; }

    PendingOutcome snapshot() const {
        PendingOutcome outcome;
        outcome.writer_state = writer_state();
        outcome.events = events_;
        if (fake_transport_) {
            outcome.written = fake_transport_->written();
        }
        return outcome;
    }

private:
    static void handle_change(PendingEntry *entry, const PendingChange &change) noexcept {
        auto *self = static_cast<PendingHttp2Connection *>(entry->user_ctx_);
        self->record_change(change);
    }

    void record_change(const PendingChange &change) noexcept {
        events_.push_back({change.kind, change.delta_bytes, change.total_bytes, change.written_bytes, change.result});
        if (change.kind == PendingChange::Kind::Completed || change.kind == PendingChange::Kind::Failed ||
            change.kind == PendingChange::Kind::Canceled) {
            ++terminal_events_;
            if (terminal_events_ >= expected_terminal_events_) {
                done_ = true;
            }
        }
    }

    FakeHttpTransport *fake_transport_ = nullptr;
    std::size_t expected_terminal_events_ = 0;
    std::size_t terminal_events_ = 0;
    bool done_ = false;
    std::vector<PendingEvent> events_;
};

class ControlHttp2Connection final : public fiber::http::Http2Connection {
public:
    ControlHttp2Connection(std::unique_ptr<fiber::http::HttpTransport> transport, FakeHttpTransport *fake_transport,
                           Options options = {}) :
        fiber::http::Http2Connection(std::move(transport), options), fake_transport_(fake_transport) {}

    fiber::common::IoErr attach_stream(fiber::http::Http2Stream &stream) noexcept { return bind_stream(stream); }
    fiber::common::IoErr submit_data(fiber::http::Http2Stream &stream, std::string_view data,
                                     std::uint8_t last_flags = 0) noexcept {
        SendPayload payload;
        payload.set_stable_span(reinterpret_cast<const std::uint8_t *>(data.data()), data.size());
        return stream.enqueue_pending(*this, PendingKind::Data, std::move(payload), 0, last_flags, nullptr, nullptr);
    }
    [[nodiscard]] std::int32_t current_connection_send_window() const noexcept { return connection_send_window(); }
    [[nodiscard]] std::uint32_t current_peer_max_frame_size() const noexcept { return peer_max_outbound_frame_size(); }
    [[nodiscard]] std::uint32_t current_peer_max_concurrent_streams() const noexcept {
        return peer_max_concurrent_streams();
    }
    [[nodiscard]] bool current_peer_enable_push() const noexcept { return peer_enable_push(); }
    [[nodiscard]] bool current_has_stream(std::uint32_t stream_id) const noexcept { return has_stream(stream_id); }
    [[nodiscard]] WriterState current_writer_state() const noexcept { return writer_state(); }
    [[nodiscard]] bool send_idle() const noexcept { return send_queue_idle(); }
    [[nodiscard]] const std::string &written() const noexcept { return fake_transport_->written(); }

private:
    FakeHttpTransport *fake_transport_ = nullptr;
};

using SendScript = std::function<fiber::common::IoErr(SendingHttp2Connection &)>;
using PendingScript = std::function<fiber::common::IoErr(PendingHttp2Connection &, fiber::http::Http2Stream &,
                                                         fiber::http::Http2Stream &)>;
using ControlScript = std::function<void(ControlHttp2Connection &, fiber::http::Http2Stream &, fiber::http::Http2Stream &)>;

DetachedTask run_send_connection(std::shared_ptr<std::promise<SendOutcome>> promise, std::vector<size_t> write_steps,
                                 std::size_t expected_done, SendScript submit,
                                 fiber::http::Http2Connection::Options options = {}) {
    auto transport = std::make_unique<FakeHttpTransport>(std::vector<std::string>{}, std::move(write_steps));
    auto *fake_transport = transport.get();
    SendingHttp2Connection connection(std::move(transport), fake_transport, expected_done, options);

    SendOutcome outcome;
    outcome.submit_error = submit(connection);
    if (outcome.submit_error != fiber::common::IoErr::None) {
        promise->set_value(std::move(outcome));
        fiber::event::EventLoop::current().stop();
        co_return;
    }

    while (!connection.done()) {
        co_await fiber::async::sleep(std::chrono::milliseconds(1));
    }

    co_await fiber::async::sleep(std::chrono::milliseconds(1));
    outcome = connection.snapshot();
    promise->set_value(std::move(outcome));
    fiber::event::EventLoop::current().stop();
    co_return;
}

SendOutcome execute_send_connection(SendScript submit, std::size_t expected_done, std::vector<size_t> write_steps = {},
                                    fiber::http::Http2Connection::Options options = {}) {
    fiber::event::EventLoopGroup group(1);
    auto promise = std::make_shared<std::promise<SendOutcome>>();
    auto future = promise->get_future();

    group.start();
    fiber::async::spawn(group.at(0), [promise = std::move(promise), write_steps = std::move(write_steps), expected_done,
                                      submit = std::move(submit), options]() mutable {
        return run_send_connection(std::move(promise), std::move(write_steps), expected_done, std::move(submit), options);
    });

    auto status = future.wait_for(std::chrono::seconds(2));
    if (status != std::future_status::ready) {
        group.stop();
        group.join();
        ADD_FAILURE() << "Timed out waiting for http2 send task";
        return {};
    }

    SendOutcome outcome = future.get();
    group.join();
    return outcome;
}

DetachedTask run_pending_connection(std::shared_ptr<std::promise<PendingOutcome>> promise, std::vector<size_t> write_steps,
                                    std::size_t expected_terminal_events, PendingScript submit,
                                    fiber::http::Http2Connection::Options options = {}) {
    auto transport = std::make_unique<FakeHttpTransport>(std::vector<std::string>{}, std::move(write_steps));
    auto *fake_transport = transport.get();
    PendingHttp2Connection connection(std::move(transport), fake_transport, expected_terminal_events, options);
    fiber::http::Http2Stream stream1(1);
    fiber::http::Http2Stream stream3(3);

    PendingOutcome outcome;
    outcome.submit_error = submit(connection, stream1, stream3);
    if (outcome.submit_error != fiber::common::IoErr::None) {
        promise->set_value(std::move(outcome));
        fiber::event::EventLoop::current().stop();
        co_return;
    }

    while (!connection.done()) {
        co_await fiber::async::sleep(std::chrono::milliseconds(1));
    }

    co_await fiber::async::sleep(std::chrono::milliseconds(1));
    outcome = connection.snapshot();
    promise->set_value(std::move(outcome));
    fiber::event::EventLoop::current().stop();
    co_return;
}

PendingOutcome execute_pending_connection(PendingScript submit, std::size_t expected_terminal_events,
                                          std::vector<size_t> write_steps = {},
                                          fiber::http::Http2Connection::Options options = {}) {
    fiber::event::EventLoopGroup group(1);
    auto promise = std::make_shared<std::promise<PendingOutcome>>();
    auto future = promise->get_future();

    group.start();
    fiber::async::spawn(group.at(0), [promise = std::move(promise), write_steps = std::move(write_steps),
                                      expected_terminal_events, submit = std::move(submit), options]() mutable {
        return run_pending_connection(std::move(promise), std::move(write_steps), expected_terminal_events,
                                      std::move(submit), options);
    });

    auto status = future.wait_for(std::chrono::seconds(2));
    if (status != std::future_status::ready) {
        group.stop();
        group.join();
        ADD_FAILURE() << "Timed out waiting for http2 pending task";
        return {};
    }

    PendingOutcome outcome = future.get();
    group.join();
    return outcome;
}

DetachedTask run_control_connection(std::shared_ptr<std::promise<ControlRunOutcome>> promise, std::vector<std::string> chunks,
                                    ControlScript setup, fiber::http::Http2Connection::Options options = {},
                                    bool auto_start_connection_preface = false) {
    options.auto_start_connection_preface = auto_start_connection_preface;
    auto transport = std::make_unique<FakeHttpTransport>(std::move(chunks));
    auto *fake_transport = transport.get();
    ControlHttp2Connection connection(std::move(transport), fake_transport, options);
    fiber::http::Http2Stream stream1(1);
    fiber::http::Http2Stream stream3(3);

    if (setup) {
        setup(connection, stream1, stream3);
    }

    ControlRunOutcome outcome;
    outcome.result = co_await connection.run();
    for (int i = 0; i < 20 && !connection.send_idle(); ++i) {
        co_await fiber::async::sleep(std::chrono::milliseconds(1));
    }
    co_await fiber::async::sleep(std::chrono::milliseconds(1));
    outcome.written = connection.written();
    outcome.writer_state = connection.current_writer_state();
    outcome.conn_send_window = connection.current_connection_send_window();
    outcome.peer_max_frame_size = connection.current_peer_max_frame_size();
    outcome.peer_max_concurrent_streams = connection.current_peer_max_concurrent_streams();
    outcome.peer_enable_push = connection.current_peer_enable_push();
    outcome.stream1_send_window = stream1.send_window();
    outcome.stream1_state = stream1.state();
    outcome.stream1_registered = connection.current_has_stream(stream1.stream_id());
    promise->set_value(std::move(outcome));
    fiber::event::EventLoop::current().stop();
    co_return;
}

ControlRunOutcome execute_control_connection(std::vector<std::string> chunks, ControlScript setup = {},
                                             fiber::http::Http2Connection::Options options = {},
                                             bool auto_start_connection_preface = false) {
    fiber::event::EventLoopGroup group(1);
    auto promise = std::make_shared<std::promise<ControlRunOutcome>>();
    auto future = promise->get_future();

    group.start();
    fiber::async::spawn(group.at(0), [promise = std::move(promise), chunks = std::move(chunks), setup = std::move(setup),
                                      options, auto_start_connection_preface]() mutable {
        return run_control_connection(std::move(promise), std::move(chunks), std::move(setup), options,
                                      auto_start_connection_preface);
    });

    auto status = future.wait_for(std::chrono::seconds(2));
    if (status != std::future_status::ready) {
        group.stop();
        group.join();
        ADD_FAILURE() << "Timed out waiting for http2 control task";
        return {};
    }

    ControlRunOutcome outcome = future.get();
    group.join();
    return outcome;
}

} // namespace

TEST(Http2ConnectionTest, ReportsPayloadChunksWithFrameOffsets) {
    constexpr std::string_view preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    std::string data = make_frame(11, 0x0, 0x1, 1, "hello world");

    std::vector<std::string> chunks = {
            std::string(preface),
            data.substr(0, 12),
            data.substr(12, 4),
            data.substr(16),
    };

    RunOutcome outcome = execute_connection(std::move(chunks));

    ASSERT_TRUE(outcome.result.has_value());
    ASSERT_EQ(outcome.chunks.size(), 3U);
    EXPECT_EQ(outcome.chunks[0].header.length, 11U);
    EXPECT_EQ(outcome.chunks[0].offset, 0U);
    EXPECT_EQ(outcome.chunks[0].length, 3U);
    EXPECT_EQ(iobuf_to_string(outcome.chunks[0].payload), "hel");
    EXPECT_EQ(outcome.chunks[1].offset, 3U);
    EXPECT_EQ(outcome.chunks[1].length, 4U);
    EXPECT_EQ(iobuf_to_string(outcome.chunks[1].payload), "lo w");
    EXPECT_EQ(outcome.chunks[2].offset, 7U);
    EXPECT_EQ(outcome.chunks[2].length, 4U);
    EXPECT_EQ(iobuf_to_string(outcome.chunks[2].payload), "orld");
}

TEST(Http2ConnectionTest, AllowsClientsToParseFramesWithoutPeerPreface) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    options.expect_peer_preface = false;

    std::string data = make_frame(4, 0x0, 0x1, 1, "pong");
    RunOutcome outcome = execute_connection({data}, options);

    ASSERT_TRUE(outcome.result.has_value());
    ASSERT_EQ(outcome.chunks.size(), 1U);
    EXPECT_EQ(outcome.chunks[0].header.type, fiber::http::Http2FrameType::Data);
    EXPECT_EQ(outcome.chunks[0].offset, 0U);
    EXPECT_EQ(outcome.chunks[0].length, 4U);
    EXPECT_EQ(iobuf_to_string(outcome.chunks[0].payload), "pong");
}

TEST(Http2ConnectionTest, ReportsZeroLengthFrames) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    options.expect_peer_preface = false;

    std::string data = make_frame(0, 0x0, 0x0, 1, "");
    RunOutcome outcome = execute_connection({data}, options);

    ASSERT_TRUE(outcome.result.has_value());
    ASSERT_EQ(outcome.chunks.size(), 1U);
    EXPECT_EQ(outcome.chunks[0].header.type, fiber::http::Http2FrameType::Data);
    EXPECT_EQ(outcome.chunks[0].offset, 0U);
    EXPECT_EQ(outcome.chunks[0].length, 0U);
    EXPECT_EQ(outcome.chunks[0].payload.readable(), 0U);
}

TEST(Http2ConnectionTest, ReallocatesReadBufferWhenPayloadIsRetained) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    options.expect_peer_preface = false;

    std::string first = make_frame(3, 0x0, 0x0, 1, "abc");
    std::string second = make_frame(4, 0x0, 0x0, 1, "wxyz");

    RunOutcome outcome = execute_connection({first, second}, options);

    ASSERT_TRUE(outcome.result.has_value());
    ASSERT_EQ(outcome.chunks.size(), 2U);
    EXPECT_EQ(iobuf_to_string(outcome.chunks[0].payload), "abc");
    EXPECT_EQ(iobuf_to_string(outcome.chunks[1].payload), "wxyz");
}

TEST(Http2ConnectionTest, RejectsPrefaceMismatch) {
    std::vector<std::string> chunks = {
            "PRI * HTTP/1.1\r\n\r\nSM\r\n\r\n",
    };

    RunOutcome outcome = execute_connection(std::move(chunks));

    ASSERT_FALSE(outcome.result.has_value());
    EXPECT_EQ(outcome.result.error(), fiber::common::IoErr::Invalid);
    EXPECT_TRUE(outcome.chunks.empty());
}

TEST(Http2ConnectionTest, RejectsFramesLargerThanConfiguredMax) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    options.expect_peer_preface = false;
    options.max_frame_size = 3;

    std::string frame = make_frame(4, 0x0, 0x0, 1, "data");
    RunOutcome outcome = execute_connection({frame}, options);

    ASSERT_FALSE(outcome.result.has_value());
    EXPECT_EQ(outcome.result.error(), fiber::common::IoErr::Invalid);
    EXPECT_TRUE(outcome.chunks.empty());
}

TEST(Http2ConnectionTest, PropagatesPayloadCallbackErrors) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    options.expect_peer_preface = false;

    std::string frame = make_frame(4, 0x0, 0x0, 1, "data");
    RunOutcome outcome = execute_connection({frame}, options, fiber::common::IoErr::Busy);

    ASSERT_FALSE(outcome.result.has_value());
    EXPECT_EQ(outcome.result.error(), fiber::common::IoErr::Busy);
    ASSERT_EQ(outcome.chunks.size(), 1U);
    EXPECT_EQ(iobuf_to_string(outcome.chunks[0].payload), "data");
}

TEST(Http2ConnectionTest, SettingsFrameUpdatesPeerStateAndSendsAck) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    options.expect_peer_preface = false;

    std::string payload;
    payload.push_back('\0');
    payload.push_back('\x4');
    payload.push_back('\0');
    payload.push_back('\x01');
    payload.push_back('\x11');
    payload.push_back('\x70');
    payload.push_back('\0');
    payload.push_back('\x5');
    payload.push_back('\0');
    payload.push_back('\0');
    payload.push_back('\x80');
    payload.push_back('\0');
    std::string settings = make_frame(static_cast<std::uint32_t>(payload.size()), 0x4, 0x0, 0, payload);

    ControlRunOutcome outcome = execute_control_connection(
            {settings},
            [](ControlHttp2Connection &connection, fiber::http::Http2Stream &stream1, fiber::http::Http2Stream &) {
                ASSERT_EQ(connection.attach_stream(stream1), fiber::common::IoErr::None);
            },
            options);

    ASSERT_TRUE(outcome.result.has_value());
    EXPECT_EQ(outcome.peer_max_frame_size, 32768U);
    EXPECT_EQ(outcome.stream1_send_window, 70000);
    std::vector<EncodedFrame> frames = parse_frames(outcome.written);
    ASSERT_EQ(frames.size(), 1U) << describe_frames(frames);
    EXPECT_EQ(frames[0].type, 0x4);
    EXPECT_EQ(frames[0].flags, 0x1);
    EXPECT_EQ(frames[0].stream_id, 0U);
    EXPECT_TRUE(frames[0].payload.empty());
}

TEST(Http2ConnectionTest, LowerInitialWindowCanMakeStreamSendWindowNegative) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    options.expect_peer_preface = false;
    options.max_frame_size = 32768;
    options.initial_connection_send_window = 100000;

    std::string payload;
    payload.push_back('\0');
    payload.push_back('\x4');
    payload.push_back('\0');
    payload.push_back('\0');
    payload.push_back('\x04');
    payload.push_back('\0');
    std::string settings = make_frame(static_cast<std::uint32_t>(payload.size()), 0x4, 0x0, 0, payload);
    std::string body(70000, 'x');

    ControlRunOutcome outcome = execute_control_connection(
            {settings},
            [body](ControlHttp2Connection &connection, fiber::http::Http2Stream &stream1, fiber::http::Http2Stream &) {
                ASSERT_EQ(connection.submit_data(stream1, body), fiber::common::IoErr::None);
            },
            options);

    ASSERT_TRUE(outcome.result.has_value());
    EXPECT_LT(outcome.stream1_send_window, 0);

    std::vector<EncodedFrame> frames = parse_frames(outcome.written);
    ASSERT_EQ(frames.size(), 2U) << describe_frames(frames);
    EXPECT_EQ(frames[0].type, 0x0);
    EXPECT_EQ(frames[0].length, 32768U);
    EXPECT_EQ(frames[1].type, 0x4);
    EXPECT_EQ(frames[1].flags, 0x1);
    EXPECT_EQ(frames[1].stream_id, 0U);
}

TEST(Http2ConnectionTest, PingFrameRepliesWithAckAndSamePayload) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    options.expect_peer_preface = false;

    std::string ping = make_frame(8, 0x6, 0x0, 0, "12345678");
    ControlRunOutcome outcome = execute_control_connection({ping}, {}, options);

    ASSERT_TRUE(outcome.result.has_value());
    std::vector<EncodedFrame> frames = parse_frames(outcome.written);
    ASSERT_EQ(frames.size(), 1U) << describe_frames(frames);
    EXPECT_EQ(frames[0].type, 0x6);
    EXPECT_EQ(frames[0].flags, 0x1);
    EXPECT_EQ(frames[0].stream_id, 0U);
    EXPECT_EQ(frames[0].payload, "12345678");
}

TEST(Http2ConnectionTest, WindowUpdateIncreasesConnectionAndStreamSendWindow) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    options.expect_peer_preface = false;

    std::string stream_update_payload;
    stream_update_payload.push_back('\0');
    stream_update_payload.push_back('\0');
    stream_update_payload.push_back('\0');
    stream_update_payload.push_back('\x64');
    std::string conn_update_payload;
    conn_update_payload.push_back('\0');
    conn_update_payload.push_back('\0');
    conn_update_payload.push_back('\0');
    conn_update_payload.push_back('\x32');

    ControlRunOutcome outcome = execute_control_connection(
            {make_frame(4, 0x8, 0x0, 1, stream_update_payload), make_frame(4, 0x8, 0x0, 0, conn_update_payload)},
            [](ControlHttp2Connection &connection, fiber::http::Http2Stream &stream1, fiber::http::Http2Stream &) {
                ASSERT_EQ(connection.attach_stream(stream1), fiber::common::IoErr::None);
            },
            options);

    ASSERT_TRUE(outcome.result.has_value());
    EXPECT_EQ(outcome.stream1_send_window, 65635);
    EXPECT_EQ(outcome.conn_send_window, 65585);
}

TEST(Http2ConnectionTest, ZeroIncrementWindowUpdateOnStreamSendsRstStream) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    options.expect_peer_preface = false;

    std::string payload(4, '\0');
    ControlRunOutcome outcome = execute_control_connection({make_frame(4, 0x8, 0x0, 1, payload)}, {}, options);

    ASSERT_TRUE(outcome.result.has_value());
    std::vector<EncodedFrame> frames = parse_frames(outcome.written);
    ASSERT_EQ(frames.size(), 1U) << describe_frames(frames);
    EXPECT_EQ(frames[0].type, 0x3);
    EXPECT_EQ(frames[0].stream_id, 1U);
    ASSERT_EQ(frames[0].payload.size(), 4U);
    EXPECT_EQ(static_cast<unsigned char>(frames[0].payload[3]), 0x1U);
}

TEST(Http2ConnectionTest, RstStreamClosesActiveStream) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    options.expect_peer_preface = false;

    std::string payload(4, '\0');
    payload[3] = '\x8';
    ControlRunOutcome outcome = execute_control_connection(
            {make_frame(4, 0x3, 0x0, 1, payload)},
            [](ControlHttp2Connection &connection, fiber::http::Http2Stream &stream1, fiber::http::Http2Stream &) {
                ASSERT_EQ(connection.attach_stream(stream1), fiber::common::IoErr::None);
            },
            options);

    ASSERT_TRUE(outcome.result.has_value());
    EXPECT_EQ(outcome.stream1_state, fiber::http::Http2Stream::State::Closed);
    EXPECT_FALSE(outcome.stream1_registered);
}

TEST(Http2ConnectionTest, RejectsInvalidPingLengthAsConnectionError) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    options.expect_peer_preface = false;

    ControlRunOutcome outcome = execute_control_connection({make_frame(4, 0x6, 0x0, 0, "pong")}, {}, options);

    ASSERT_FALSE(outcome.result.has_value());
    EXPECT_EQ(outcome.result.error(), fiber::common::IoErr::Invalid);
}

TEST(Http2ConnectionTest, ClientConnectionPrefaceSendsPrefaceSettingsAndWindowUpdate) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Client;
    options.expect_peer_preface = false;
    options.max_frame_size = 0x00ffffffU;
    options.local_max_concurrent_streams = 128;
    options.initial_stream_send_window = 65535;
    options.initial_connection_recv_window = 0x7fffffffU;

    ControlRunOutcome outcome = execute_control_connection({}, {}, options, true);

    ASSERT_TRUE(outcome.result.has_value());
    ASSERT_GE(outcome.written.size(), kClientConnectionPreface.size());
    EXPECT_EQ(outcome.written.substr(0, kClientConnectionPreface.size()), kClientConnectionPreface);

    std::string_view frames_view(outcome.written.data() + kClientConnectionPreface.size(),
                                 outcome.written.size() - kClientConnectionPreface.size());
    std::vector<EncodedFrame> frames = parse_frames(frames_view);
    ASSERT_EQ(frames.size(), 2U) << describe_frames(frames);
    EXPECT_EQ(frames[0].type, 0x4);
    EXPECT_EQ(frames[0].flags, 0x0);
    EXPECT_EQ(frames[0].stream_id, 0U);
    EXPECT_EQ(frames[0].length, 18U);
    EXPECT_EQ(frames[1].type, 0x8);
    EXPECT_EQ(frames[1].stream_id, 0U);
    EXPECT_EQ(frames[1].length, 4U);

    ASSERT_EQ(frames[1].payload.size(), 4U);
    std::uint32_t increment = (static_cast<std::uint32_t>(static_cast<std::uint8_t>(frames[1].payload[0]) & 0x7fU) << 24) |
                              (static_cast<std::uint32_t>(static_cast<std::uint8_t>(frames[1].payload[1])) << 16) |
                              (static_cast<std::uint32_t>(static_cast<std::uint8_t>(frames[1].payload[2])) << 8) |
                              static_cast<std::uint32_t>(static_cast<std::uint8_t>(frames[1].payload[3]));
    EXPECT_EQ(increment, 0x7fffffffU - 65535U);
}

TEST(Http2ConnectionTest, ServerConnectionPrefaceSendsSettingsAndWindowUpdateAfterPeerPreface) {
    fiber::http::Http2Connection::Options options;
    options.role = fiber::http::Http2Connection::ConnectionRole::Server;
    options.expect_peer_preface = true;
    options.max_frame_size = 0x00ffffffU;
    options.local_max_concurrent_streams = 128;
    options.initial_stream_send_window = 65535;
    options.initial_connection_recv_window = 0x7fffffffU;

    ControlRunOutcome outcome = execute_control_connection({std::string(kClientConnectionPreface)}, {}, options, true);

    ASSERT_TRUE(outcome.result.has_value());
    std::vector<EncodedFrame> frames = parse_frames(outcome.written);
    ASSERT_EQ(frames.size(), 2U) << describe_frames(frames);
    EXPECT_EQ(frames[0].type, 0x4);
    EXPECT_EQ(frames[0].flags, 0x0);
    EXPECT_EQ(frames[0].stream_id, 0U);
    EXPECT_EQ(frames[0].length, 18U);
    EXPECT_EQ(frames[1].type, 0x8);
    EXPECT_EQ(frames[1].stream_id, 0U);
    EXPECT_EQ(frames[1].length, 4U);
}

TEST(Http2ConnectionTest, SendsStableSpanAcrossPartialWrites) {
    SendOutcome outcome = execute_send_connection(
            [](SendingHttp2Connection &connection) {
                return connection.submit_stable_span("hello world");
            },
            1, {3, 4, 4});

    ASSERT_EQ(outcome.submit_error, fiber::common::IoErr::None);
    ASSERT_EQ(outcome.completions.size(), 1U);
    EXPECT_EQ(outcome.completions[0].total_bytes, 11U);
    EXPECT_EQ(outcome.completions[0].written_bytes, 11U);
    EXPECT_EQ(outcome.completions[0].result, fiber::common::IoErr::None);
    EXPECT_EQ(outcome.writer_state, fiber::http::Http2Connection::WriterState::WaitingForData);
    EXPECT_EQ(outcome.written, "hello world");
}

TEST(Http2ConnectionTest, SendsIoBufChainUsingWritev) {
    SendOutcome outcome = execute_send_connection(
            [](SendingHttp2Connection &connection) {
                fiber::mem::IoBuf first = fiber::mem::IoBuf::allocate(4);
                if (!first) {
                    return fiber::common::IoErr::NoMem;
                }
                std::memcpy(first.writable_data(), "ab", 2);
                first.commit(2);

                fiber::mem::IoBuf second = fiber::mem::IoBuf::allocate(8);
                if (!second) {
                    return fiber::common::IoErr::NoMem;
                }
                std::memcpy(second.writable_data(), "cdef", 4);
                second.commit(4);

                fiber::mem::IoBufChain chain;
                if (!chain.append(std::move(first))) {
                    return fiber::common::IoErr::NoMem;
                }
                if (!chain.append(std::move(second))) {
                    return fiber::common::IoErr::NoMem;
                }
                return connection.submit_chain(std::move(chain));
            },
            1, {4, 2});

    ASSERT_EQ(outcome.submit_error, fiber::common::IoErr::None);
    ASSERT_EQ(outcome.completions.size(), 1U);
    EXPECT_EQ(outcome.completions[0].total_bytes, 6U);
    EXPECT_EQ(outcome.completions[0].written_bytes, 6U);
    EXPECT_EQ(outcome.completions[0].result, fiber::common::IoErr::None);
    EXPECT_EQ(outcome.written, "abcdef");
}

TEST(Http2ConnectionTest, ClosingSendingNotifiesQueuedEntries) {
    SendOutcome outcome = execute_send_connection(
            [](SendingHttp2Connection &connection) {
                fiber::common::IoErr err = connection.submit_stable_span("first");
                if (err != fiber::common::IoErr::None) {
                    return err;
                }
                err = connection.submit_stable_span("second");
                if (err != fiber::common::IoErr::None) {
                    return err;
                }
                connection.request_stop(fiber::common::IoErr::Canceled);
                return fiber::common::IoErr::None;
            },
            2);

    ASSERT_EQ(outcome.submit_error, fiber::common::IoErr::None);
    ASSERT_EQ(outcome.completions.size(), 2U);
    EXPECT_EQ(outcome.completions[0].result, fiber::common::IoErr::Canceled);
    EXPECT_EQ(outcome.completions[0].written_bytes, 0U);
    EXPECT_EQ(outcome.completions[1].result, fiber::common::IoErr::Canceled);
    EXPECT_EQ(outcome.completions[1].written_bytes, 0U);
    EXPECT_TRUE(outcome.written.empty());
    EXPECT_EQ(outcome.writer_state, fiber::http::Http2Connection::WriterState::Stopping);
}

TEST(Http2ConnectionTest, PendingDataKeepsLaterHeadersInOrderWithinSameStream) {
    fiber::http::Http2Connection::Options options;
    options.max_frame_size = 2;
    options.initial_connection_send_window = 4;

    PendingOutcome outcome = execute_pending_connection(
            [](PendingHttp2Connection &connection, fiber::http::Http2Stream &stream1, fiber::http::Http2Stream &) {
                fiber::common::IoErr err = connection.submit_data(stream1, "abcd");
                if (err != fiber::common::IoErr::None) {
                    return err;
                }
                err = connection.submit_header(stream1, "xy", 0, 0x4);
                if (err != fiber::common::IoErr::None) {
                    return err;
                }
                return fiber::common::IoErr::None;
            },
            2, {}, options);

    ASSERT_EQ(outcome.submit_error, fiber::common::IoErr::None);
    std::vector<EncodedFrame> frames = parse_frames(outcome.written);
    ASSERT_EQ(frames.size(), 3U) << describe_frames(frames);
    EXPECT_EQ(frames[0].type, 0x0);
    EXPECT_EQ(frames[0].payload, "ab");
    EXPECT_EQ(frames[1].type, 0x0);
    EXPECT_EQ(frames[1].payload, "cd");
    EXPECT_EQ(frames[2].type, 0x1);
    EXPECT_EQ(frames[2].flags, 0x4);
    EXPECT_EQ(frames[2].payload, "xy");

    ASSERT_GE(outcome.events.size(), 6U);
    EXPECT_EQ(outcome.events[0].kind, fiber::http::Http2PendingChange::Kind::Scheduled);
    EXPECT_EQ(outcome.events[0].delta_bytes, 2U);
    EXPECT_EQ(outcome.events[1].kind, fiber::http::Http2PendingChange::Kind::Scheduled);
    EXPECT_EQ(outcome.events[1].delta_bytes, 2U);
    EXPECT_TRUE(std::any_of(outcome.events.begin(), outcome.events.end(), [](const PendingEvent &event) {
        return event.kind == fiber::http::Http2PendingChange::Kind::Written;
    }));
    EXPECT_EQ(outcome.events.back().kind, fiber::http::Http2PendingChange::Kind::Completed);
}

TEST(Http2ConnectionTest, PendingHeaderOnAnotherStreamCanBypassConnWindowBlockedData) {
    fiber::http::Http2Connection::Options options;
    options.max_frame_size = 4;
    options.initial_connection_send_window = 0;

    PendingOutcome outcome = execute_pending_connection(
            [](PendingHttp2Connection &connection, fiber::http::Http2Stream &stream1, fiber::http::Http2Stream &stream3) {
                fiber::common::IoErr err = connection.submit_data(stream1, "data");
                if (err != fiber::common::IoErr::None) {
                    return err;
                }
                err = connection.submit_header(stream3, "hh", 0, 0x4);
                if (err != fiber::common::IoErr::None) {
                    return err;
                }
                fiber::async::spawn([&connection]() -> DetachedTask {
                    co_await fiber::async::sleep(std::chrono::milliseconds(1));
                    connection.add_connection_credit(4);
                    co_return;
                });
                return fiber::common::IoErr::None;
            },
            2, {}, options);

    ASSERT_EQ(outcome.submit_error, fiber::common::IoErr::None);
    std::vector<EncodedFrame> frames = parse_frames(outcome.written);
    ASSERT_EQ(frames.size(), 2U) << describe_frames(frames);
    EXPECT_EQ(frames[0].type, 0x1);
    EXPECT_EQ(frames[0].stream_id, 3U);
    EXPECT_EQ(frames[0].payload, "hh");
    EXPECT_EQ(frames[1].type, 0x0);
    EXPECT_EQ(frames[1].stream_id, 1U);
    EXPECT_EQ(frames[1].payload, "data");
}
