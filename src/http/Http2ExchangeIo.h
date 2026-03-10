#ifndef FIBER_HTTP_HTTP2_EXCHANGE_IO_H
#define FIBER_HTTP_HTTP2_EXCHANGE_IO_H

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <string_view>

#include <nghttp2/nghttp2.h>

#include "HttpExchangeIo.h"

namespace fiber::http {

class HttpExchange;

class Http2ExchangeIo final : public HttpExchangeIo {
public:
    using FlushCallback = std::function<void()>;

    Http2ExchangeIo(nghttp2_session *session, int32_t stream_id, FlushCallback on_flush = {});

    common::IoResult<void> on_request_header(HttpExchange &exchange, std::string_view name, std::string_view value);
    common::IoResult<void> on_request_headers_complete(HttpExchange &exchange);
    void append_request_body(std::string_view data);
    void mark_request_body_end();

    fiber::async::Task<common::IoResult<ReadBodyChunk>> read_body(HttpExchange &exchange,
                                                                  size_t max_bytes) noexcept override;
    fiber::async::Task<common::IoResult<void>> send_response_header(HttpExchange &exchange, int status,
                                                                    std::string_view reason) override;
    fiber::async::Task<common::IoResult<size_t>> write_body(HttpExchange &exchange, const uint8_t *buf, size_t len,
                                                            bool end) noexcept override;

private:
    static nghttp2_ssize data_source_read_callback(nghttp2_session *session, int32_t stream_id, uint8_t *buf,
                                                   size_t length, uint32_t *data_flags, nghttp2_data_source *source,
                                                   void *user_data);
    nghttp2_ssize read_response_data(uint8_t *buf, size_t length, uint32_t *data_flags);
    static common::IoErr map_nghttp2_error(int rc) noexcept;

    nghttp2_session *session_ = nullptr;
    int32_t stream_id_ = -1;
    FlushCallback on_flush_;

    std::string method_text_;
    std::string path_text_;
    std::string scheme_text_;
    std::string authority_text_;
    std::string version_text_{"HTTP/2.0"};

    std::string request_body_;
    size_t request_body_read_ = 0;
    bool request_body_end_ = false;

    nghttp2_data_provider2 data_provider_{};
    std::deque<std::string> response_chunks_;
    size_t response_chunk_offset_ = 0;
    bool response_header_sent_ = false;
    bool response_complete_ = false;
    bool response_end_stream_ = false;
    bool response_data_deferred_ = false;
    size_t response_body_sent_ = 0;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_EXCHANGE_IO_H
