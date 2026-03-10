#include "Http2Connection.h"

#include <array>
#include <utility>

#include "../async/Spawn.h"
#include "../common/Assert.h"

namespace fiber::http {

namespace {

constexpr size_t kReadBufferSize = 16 * 1024;
constexpr uint32_t kMaxConcurrentStreams = 100;

} // namespace

Http2Connection::Http2Connection(event::EventLoop &loop,
                                 std::unique_ptr<HttpTransport> transport,
                                 HttpHandler handler,
                                 HttpServerOptions options)
    : loop_(loop),
      transport_(std::move(transport)),
      handler_(std::move(handler)),
      options_(std::move(options)) {
}

Http2Connection::~Http2Connection() {
    close();
}

common::IoResult<void> Http2Connection::init() {
    if (initialized_) {
        return {};
    }
    if (finalized_ || !transport_ || !transport_->valid()) {
        return std::unexpected(common::IoErr::Invalid);
    }

    int rc = nghttp2_session_callbacks_new(&callbacks_);
    if (rc != 0) {
        return std::unexpected(map_nghttp2_error(rc));
    }

    nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks_, &Http2Connection::on_begin_headers_cb);
    nghttp2_session_callbacks_set_on_header_callback(callbacks_, &Http2Connection::on_header_cb);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks_, &Http2Connection::on_data_chunk_recv_cb);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks_, &Http2Connection::on_frame_recv_cb);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks_, &Http2Connection::on_stream_close_cb);

    rc = nghttp2_session_server_new2(&session_, callbacks_, this, nullptr);
    if (rc != 0) {
        nghttp2_session_callbacks_del(callbacks_);
        callbacks_ = nullptr;
        return std::unexpected(map_nghttp2_error(rc));
    }

    nghttp2_settings_entry iv[] = {
        {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, kMaxConcurrentStreams},
    };
    rc = nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, iv, 1);
    if (rc != 0) {
        request_stop();
        finalize_close();
        return std::unexpected(map_nghttp2_error(rc));
    }

    initialized_ = true;
    request_flush();
    return {};
}

fiber::async::Task<void> Http2Connection::run() {
    auto init_result = init();
    if (!init_result) {
        request_stop();
        finalize_close();
        co_return;
    }

    running_ = true;
    workers_.add(2);

    fiber::async::spawn(loop_, [this]() -> fiber::async::DetachedTask {
        co_await read_loop();
        workers_.done();
        co_return;
    });
    fiber::async::spawn(loop_, [this]() -> fiber::async::DetachedTask {
        co_await write_loop();
        workers_.done();
        co_return;
    });

    co_await workers_.join();
    running_ = false;
    finalize_close();
    co_return;
}

void Http2Connection::close() {
    request_stop();
    if (!running_) {
        finalize_close();
    }
}

bool Http2Connection::WriteWakeAwaiter::await_ready() const noexcept {
    return !conn_ || conn_->stop_requested_ || conn_->write_wakeup_;
}

bool Http2Connection::WriteWakeAwaiter::await_suspend(std::coroutine_handle<> handle) noexcept {
    if (!conn_ || conn_->stop_requested_ || conn_->write_wakeup_) {
        return false;
    }
    FIBER_ASSERT(conn_->write_waiter_ == nullptr);
    conn_->write_waiter_ = handle;
    return true;
}

void Http2Connection::WriteWakeAwaiter::await_resume() noexcept {
    if (!conn_) {
        return;
    }
    conn_->write_wakeup_ = false;
}

fiber::async::Task<void> Http2Connection::read_loop() {
    std::array<uint8_t, kReadBufferSize> buf{};
    while (!stop_requested_ && transport_ && transport_->valid()) {
        auto read_result = co_await transport_->read(buf.data(), buf.size(), options_.keep_alive_timeout);
        if (!read_result || *read_result == 0) {
            request_stop();
            break;
        }
        if (!session_) {
            request_stop();
            break;
        }

        nghttp2_ssize nproc = nghttp2_session_mem_recv2(session_, buf.data(), *read_result);
        if (nproc < 0) {
            request_stop();
            break;
        }
        request_flush();

        if (session_ &&
            nghttp2_session_want_read(session_) == 0 &&
            nghttp2_session_want_write(session_) == 0 &&
            streams_.empty()) {
            request_stop();
            break;
        }
    }

    request_stop();
    co_return;
}

fiber::async::Task<void> Http2Connection::write_loop() {
    while (!stop_requested_) {
        if (!transport_ || !transport_->valid()) {
            request_stop();
            break;
        }

        if (tx_offset_ >= tx_buffer_.size()) {
            tx_buffer_.clear();
            tx_offset_ = 0;
            if (!tx_pending_buffer_.empty()) {
                tx_buffer_.swap(tx_pending_buffer_);
                tx_pending_buffer_.clear();
            }

            if (tx_buffer_.empty()) {
                auto flush_result = pump_session_output();
                if (!flush_result) {
                    request_stop();
                    break;
                }
                if (!tx_pending_buffer_.empty()) {
                    tx_buffer_.swap(tx_pending_buffer_);
                    tx_pending_buffer_.clear();
                }
                if (tx_buffer_.empty()) {
                    co_await wait_write_ready();
                    continue;
                }
            }
        }

        auto write_result = co_await transport_->write(tx_buffer_.data() + tx_offset_,
                                                       tx_buffer_.size() - tx_offset_,
                                                       options_.write_timeout);
        if (!write_result || *write_result == 0) {
            request_stop();
            break;
        }
        tx_offset_ += *write_result;
    }
    co_return;
}

fiber::async::Task<void> Http2Connection::run_stream(int32_t stream_id) {
    StreamContext *stream = find_stream(stream_id);
    if (!stream || !stream->exchange) {
        co_return;
    }

    co_await handler_(*stream->exchange);
    request_flush();
    on_stream_handler_done(stream_id);
    co_return;
}

void Http2Connection::request_stop() {
    if (stop_requested_) {
        return;
    }
    stop_requested_ = true;
    notify_write_loop();
    if (transport_) {
        transport_->close();
    }
}

void Http2Connection::finalize_close() {
    if (finalized_) {
        return;
    }
    finalized_ = true;
    initialized_ = false;
    write_waiter_ = nullptr;
    write_wakeup_ = false;
    write_resume_posted_ = false;

    streams_.clear();
    tx_buffer_.clear();
    tx_pending_buffer_.clear();
    tx_offset_ = 0;

    if (session_) {
        nghttp2_session_del(session_);
        session_ = nullptr;
    }
    if (callbacks_) {
        nghttp2_session_callbacks_del(callbacks_);
        callbacks_ = nullptr;
    }
    if (transport_) {
        transport_->close();
        transport_.reset();
    }
}

Http2Connection::WriteWakeAwaiter Http2Connection::wait_write_ready() noexcept {
    return WriteWakeAwaiter(*this);
}

void Http2Connection::notify_write_loop() {
    write_wakeup_ = true;
    if (!write_waiter_ || write_resume_posted_) {
        return;
    }
    write_resume_posted_ = true;
    loop_.post<Http2Connection, &Http2Connection::write_resume_entry_, &Http2Connection::on_write_resume>(*this);
}

void Http2Connection::on_write_resume(Http2Connection *conn) {
    if (!conn) {
        return;
    }
    conn->write_resume_posted_ = false;
    if (!conn->write_waiter_ || !conn->write_wakeup_) {
        return;
    }
    auto handle = conn->write_waiter_;
    conn->write_waiter_ = nullptr;
    if (handle) {
        handle.resume();
    }
}

int Http2Connection::on_begin_headers_cb(nghttp2_session *session,
                                         const nghttp2_frame *frame,
                                         void *user_data) {
    (void) session;
    if (!user_data) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    return static_cast<Http2Connection *>(user_data)->on_begin_headers(frame);
}

int Http2Connection::on_header_cb(nghttp2_session *session,
                                  const nghttp2_frame *frame,
                                  const uint8_t *name,
                                  size_t namelen,
                                  const uint8_t *value,
                                  size_t valuelen,
                                  uint8_t flags,
                                  void *user_data) {
    (void) session;
    (void) flags;
    if (!user_data || (namelen > 0 && !name) || (valuelen > 0 && !value)) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    auto *conn = static_cast<Http2Connection *>(user_data);
    std::string_view name_view = namelen > 0
        ? std::string_view(reinterpret_cast<const char *>(name), namelen)
        : std::string_view{};
    std::string_view value_view = valuelen > 0
        ? std::string_view(reinterpret_cast<const char *>(value), valuelen)
        : std::string_view{};
    return conn->on_header(frame,
                           name_view,
                           value_view);
}

int Http2Connection::on_data_chunk_recv_cb(nghttp2_session *session,
                                           uint8_t flags,
                                           int32_t stream_id,
                                           const uint8_t *data,
                                           size_t len,
                                           void *user_data) {
    (void) session;
    (void) flags;
    if (!user_data || (len > 0 && !data)) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    auto *conn = static_cast<Http2Connection *>(user_data);
    std::string_view chunk = len > 0
        ? std::string_view(reinterpret_cast<const char *>(data), len)
        : std::string_view{};
    return conn->on_data_chunk_recv(stream_id, chunk);
}

int Http2Connection::on_frame_recv_cb(nghttp2_session *session,
                                      const nghttp2_frame *frame,
                                      void *user_data) {
    (void) session;
    if (!user_data) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    return static_cast<Http2Connection *>(user_data)->on_frame_recv(frame);
}

int Http2Connection::on_stream_close_cb(nghttp2_session *session,
                                        int32_t stream_id,
                                        uint32_t error_code,
                                        void *user_data) {
    (void) session;
    if (!user_data) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    return static_cast<Http2Connection *>(user_data)->on_stream_close(stream_id, error_code);
}

int Http2Connection::on_begin_headers(const nghttp2_frame *frame) {
    if (!frame) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
        return 0;
    }

    auto create_result = create_request_stream(frame->hd.stream_id);
    if (!create_result) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    return 0;
}

int Http2Connection::on_header(const nghttp2_frame *frame,
                               std::string_view name,
                               std::string_view value) {
    if (!frame) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
        return 0;
    }
    StreamContext *stream = find_stream(frame->hd.stream_id);
    if (!stream || !stream->io || !stream->exchange) {
        return reset_stream(frame->hd.stream_id, NGHTTP2_INTERNAL_ERROR);
    }
    auto result = stream->io->on_request_header(*stream->exchange, name, value);
    if (!result) {
        return reset_stream(frame->hd.stream_id, NGHTTP2_PROTOCOL_ERROR);
    }
    return 0;
}

int Http2Connection::on_data_chunk_recv(int32_t stream_id, std::string_view data) {
    StreamContext *stream = find_stream(stream_id);
    if (!stream || !stream->io) {
        return reset_stream(stream_id, NGHTTP2_INTERNAL_ERROR);
    }
    stream->io->append_request_body(data);
    return 0;
}

int Http2Connection::on_frame_recv(const nghttp2_frame *frame) {
    if (!frame) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }

    if (frame->hd.type == NGHTTP2_HEADERS && frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
        StreamContext *stream = find_stream(frame->hd.stream_id);
        if (!stream || !stream->io || !stream->exchange) {
            return reset_stream(frame->hd.stream_id, NGHTTP2_INTERNAL_ERROR);
        }
        if (!stream->headers_complete) {
            auto result = stream->io->on_request_headers_complete(*stream->exchange);
            if (!result) {
                return reset_stream(frame->hd.stream_id, NGHTTP2_PROTOCOL_ERROR);
            }
            stream->headers_complete = true;
        }
    }

    if (frame->hd.type == NGHTTP2_DATA ||
        (frame->hd.type == NGHTTP2_HEADERS && frame->headers.cat == NGHTTP2_HCAT_REQUEST) ||
        (frame->hd.type == NGHTTP2_HEADERS && frame->headers.cat == NGHTTP2_HCAT_HEADERS)) {
        if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
            StreamContext *stream = find_stream(frame->hd.stream_id);
            if (!stream || !stream->io) {
                return 0;
            }
            stream->io->mark_request_body_end();
            stream->request_end_stream = true;
            try_start_stream(*stream);
        }
    }
    return 0;
}

int Http2Connection::on_stream_close(int32_t stream_id, uint32_t error_code) {
    (void) error_code;
    StreamContext *stream = find_stream(stream_id);
    if (!stream) {
        return 0;
    }
    stream->remote_closed = true;
    maybe_reclaim_stream(stream_id);
    return 0;
}

common::IoResult<Http2Connection::StreamContext *> Http2Connection::create_request_stream(int32_t stream_id) {
    if (stream_id <= 0 || !session_) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (streams_.find(stream_id) != streams_.end()) {
        return std::unexpected(common::IoErr::Already);
    }

    auto stream = std::make_unique<StreamContext>();
    stream->stream_id = stream_id;
    stream->exchange = std::make_unique<HttpExchange>(options_);

    stream->io = std::make_unique<Http2ExchangeIo>(session_, stream_id, [this]() { request_flush(); });
    stream->exchange->set_io(stream->io.get());

    StreamContext *ptr = stream.get();
    streams_.emplace(stream_id, std::move(stream));
    nghttp2_session_set_stream_user_data(session_, stream_id, ptr);
    return ptr;
}

Http2Connection::StreamContext *Http2Connection::find_stream(int32_t stream_id) noexcept {
    auto it = streams_.find(stream_id);
    if (it == streams_.end()) {
        return nullptr;
    }
    return it->second.get();
}

void Http2Connection::try_start_stream(StreamContext &stream) {
    if (stop_requested_) {
        return;
    }
    if (!stream.headers_complete || !stream.request_end_stream || stream.handler_started || !stream.exchange) {
        return;
    }
    if (stream.remote_closed) {
        maybe_reclaim_stream(stream.stream_id);
        return;
    }

    stream.handler_started = true;
    workers_.add(1);
    int32_t stream_id = stream.stream_id;
    fiber::async::spawn(loop_, [this, stream_id]() -> fiber::async::DetachedTask {
        co_await run_stream(stream_id);
        workers_.done();
        co_return;
    });
}

void Http2Connection::on_stream_handler_done(int32_t stream_id) {
    StreamContext *stream = find_stream(stream_id);
    if (!stream) {
        return;
    }
    stream->handler_done = true;
    maybe_reclaim_stream(stream_id);
}

void Http2Connection::maybe_reclaim_stream(int32_t stream_id) {
    auto it = streams_.find(stream_id);
    if (it == streams_.end()) {
        return;
    }
    StreamContext &stream = *it->second;
    if (!stream.remote_closed) {
        return;
    }
    if (stream.handler_started && !stream.handler_done) {
        return;
    }
    streams_.erase(it);
}

common::IoResult<void> Http2Connection::pump_session_output() {
    if (!session_) {
        return {};
    }
    for (;;) {
        const uint8_t *data = nullptr;
        nghttp2_ssize nwrite = nghttp2_session_mem_send2(session_, &data);
        if (nwrite < 0) {
            return std::unexpected(map_nghttp2_error(static_cast<int>(nwrite)));
        }
        if (nwrite == 0) {
            break;
        }
        tx_pending_buffer_.append(reinterpret_cast<const char *>(data), static_cast<size_t>(nwrite));
    }
    return {};
}

void Http2Connection::request_flush() {
    if (stop_requested_) {
        return;
    }
    auto result = pump_session_output();
    if (!result) {
        request_stop();
        return;
    }
    if (!tx_pending_buffer_.empty()) {
        notify_write_loop();
    }
}

common::IoErr Http2Connection::map_nghttp2_error(int rc) noexcept {
    switch (rc) {
        case 0:
            return common::IoErr::None;
        case NGHTTP2_ERR_NOMEM:
            return common::IoErr::NoMem;
        case NGHTTP2_ERR_INVALID_ARGUMENT:
        case NGHTTP2_ERR_PROTO:
            return common::IoErr::Invalid;
        case NGHTTP2_ERR_WOULDBLOCK:
            return common::IoErr::WouldBlock;
        case NGHTTP2_ERR_STREAM_CLOSED:
            return common::IoErr::ConnReset;
        case NGHTTP2_ERR_DATA_EXIST:
            return common::IoErr::Already;
        default:
            return common::IoErr::Unknown;
    }
}

int Http2Connection::reset_stream(int32_t stream_id, uint32_t error_code) {
    if (session_ && stream_id > 0) {
        nghttp2_submit_rst_stream(session_, NGHTTP2_FLAG_NONE, stream_id, error_code);
    }
    return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
}

} // namespace fiber::http
