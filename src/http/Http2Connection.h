#ifndef FIBER_HTTP_HTTP2_CONNECTION_H
#define FIBER_HTTP_HTTP2_CONNECTION_H

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>

#include <nghttp2/nghttp2.h>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../event/EventLoop.h"
#include "Http2ExchangeIo.h"
#include "HttpExchange.h"
#include "HttpTransport.h"

namespace fiber::http {

class Http2Connection : public common::NonCopyable, public common::NonMovable {
public:
    Http2Connection(event::EventLoop &loop,
                    std::unique_ptr<HttpTransport> transport,
                    HttpHandler handler,
                    HttpServerOptions options = {});
    ~Http2Connection();

    common::IoResult<void> init();
    fiber::async::Task<void> run();
    void close();

private:
    struct StreamContext {
        int32_t stream_id = 0;
        std::unique_ptr<HttpExchange> exchange;
        Http2ExchangeIo *io = nullptr;
        bool headers_complete = false;
        bool request_end_stream = false;
        bool handler_started = false;
        bool ready_queued = false;
    };

    static int on_begin_headers_cb(nghttp2_session *session,
                                   const nghttp2_frame *frame,
                                   void *user_data);
    static int on_header_cb(nghttp2_session *session,
                            const nghttp2_frame *frame,
                            const uint8_t *name,
                            size_t namelen,
                            const uint8_t *value,
                            size_t valuelen,
                            uint8_t flags,
                            void *user_data);
    static int on_data_chunk_recv_cb(nghttp2_session *session,
                                     uint8_t flags,
                                     int32_t stream_id,
                                     const uint8_t *data,
                                     size_t len,
                                     void *user_data);
    static int on_frame_recv_cb(nghttp2_session *session,
                                const nghttp2_frame *frame,
                                void *user_data);
    static int on_stream_close_cb(nghttp2_session *session,
                                  int32_t stream_id,
                                  uint32_t error_code,
                                  void *user_data);

    int on_begin_headers(const nghttp2_frame *frame);
    int on_header(const nghttp2_frame *frame,
                  std::string_view name,
                  std::string_view value);
    int on_data_chunk_recv(int32_t stream_id, std::string_view data);
    int on_frame_recv(const nghttp2_frame *frame);
    int on_stream_close(int32_t stream_id, uint32_t error_code);

    common::IoResult<StreamContext *> create_request_stream(int32_t stream_id);
    StreamContext *find_stream(int32_t stream_id) noexcept;
    void mark_stream_ready(StreamContext &stream);
    fiber::async::Task<void> dispatch_ready_streams();
    fiber::async::Task<void> drain_writes();
    common::IoResult<void> pump_session_output();
    void request_flush();
    static common::IoErr map_nghttp2_error(int rc) noexcept;
    int reset_stream(int32_t stream_id, uint32_t error_code);

    event::EventLoop &loop_;
    std::unique_ptr<HttpTransport> transport_;
    HttpHandler handler_;
    HttpServerOptions options_;

    nghttp2_session_callbacks *callbacks_ = nullptr;
    nghttp2_session *session_ = nullptr;
    std::unordered_map<int32_t, std::unique_ptr<StreamContext>> streams_;
    std::deque<int32_t> ready_streams_;

    std::string tx_buffer_;
    size_t tx_offset_ = 0;
    bool initialized_ = false;
    bool closed_ = false;
    bool write_pump_running_ = false;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_CONNECTION_H
