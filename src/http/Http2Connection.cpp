#include "Http2Connection.h"

#include <array>
#include <utility>

#include "../async/Spawn.h"

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
    if (!transport_ || !transport_->valid()) {
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
        close();
        return std::unexpected(map_nghttp2_error(rc));
    }

    initialized_ = true;
    request_flush();
    return {};
}

fiber::async::Task<void> Http2Connection::run() {
    auto init_result = init();
    if (!init_result) {
        close();
        co_return;
    }

    std::array<uint8_t, kReadBufferSize> buf{};
    while (!closed_ && transport_ && transport_->valid()) {
        auto read_result = co_await transport_->read(buf.data(), buf.size(), options_.keep_alive_timeout);
        if (!read_result) {
            break;
        }
        if (*read_result == 0) {
            break;
        }

        nghttp2_ssize nproc = nghttp2_session_mem_recv2(session_, buf.data(), *read_result);
        if (nproc < 0) {
            break;
        }
        request_flush();
        co_await dispatch_ready_streams();

        if (session_ &&
            nghttp2_session_want_read(session_) == 0 &&
            nghttp2_session_want_write(session_) == 0 &&
            streams_.empty()) {
            break;
        }
    }

    close();
    co_return;
}

void Http2Connection::close() {
    if (closed_) {
        return;
    }
    closed_ = true;
    initialized_ = false;
    streams_.clear();
    ready_streams_.clear();
    tx_buffer_.clear();
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
            mark_stream_ready(*stream);
        }
    }
    return 0;
}

int Http2Connection::on_stream_close(int32_t stream_id, uint32_t error_code) {
    (void) error_code;
    streams_.erase(stream_id);
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

    auto io = std::make_unique<Http2ExchangeIo>(session_, stream_id, [this]() { request_flush(); });
    stream->io = io.get();
    stream->exchange->set_io(std::move(io));

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

void Http2Connection::mark_stream_ready(StreamContext &stream) {
    if (!stream.headers_complete || !stream.request_end_stream || stream.handler_started || stream.ready_queued) {
        return;
    }
    stream.ready_queued = true;
    ready_streams_.push_back(stream.stream_id);
}

fiber::async::Task<void> Http2Connection::dispatch_ready_streams() {
    while (!ready_streams_.empty() && !closed_) {
        int32_t stream_id = ready_streams_.front();
        ready_streams_.pop_front();
        StreamContext *stream = find_stream(stream_id);
        if (!stream || !stream->exchange) {
            continue;
        }
        stream->ready_queued = false;
        if (stream->handler_started || !stream->headers_complete || !stream->request_end_stream) {
            continue;
        }
        stream->handler_started = true;
        co_await handler_(*stream->exchange);
        request_flush();
    }
    co_return;
}

fiber::async::Task<void> Http2Connection::drain_writes() {
    while (!closed_ && transport_ && transport_->valid()) {
        if (tx_offset_ >= tx_buffer_.size()) {
            tx_buffer_.clear();
            tx_offset_ = 0;
            break;
        }

        auto write_result = co_await transport_->write(tx_buffer_.data() + tx_offset_,
                                                       tx_buffer_.size() - tx_offset_,
                                                       options_.write_timeout);
        if (!write_result || *write_result == 0) {
            close();
            break;
        }
        tx_offset_ += *write_result;
    }

    write_pump_running_ = false;
    if (!closed_) {
        auto flush_result = pump_session_output();
        if (!flush_result) {
            close();
            co_return;
        }
        if (!tx_buffer_.empty() && !write_pump_running_) {
            write_pump_running_ = true;
            fiber::async::spawn(loop_, [this]() -> fiber::async::DetachedTask {
                co_await drain_writes();
                co_return;
            });
        }
    }
    co_return;
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
        tx_buffer_.append(reinterpret_cast<const char *>(data), static_cast<size_t>(nwrite));
    }
    return {};
}

void Http2Connection::request_flush() {
    if (closed_) {
        return;
    }
    auto result = pump_session_output();
    if (!result) {
        close();
        return;
    }
    if (tx_buffer_.empty() || write_pump_running_) {
        return;
    }
    write_pump_running_ = true;
    fiber::async::spawn(loop_, [this]() -> fiber::async::DetachedTask {
        co_await drain_writes();
        co_return;
    });
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
