#ifndef FIBER_HTTP_HTTP_EXCHANGE_H
#define FIBER_HTTP_HTTP_EXCHANGE_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../common/mem/BufPool.h"
#include "../common/mem/IoBuf.h"
#include "HttpCommon.h"
#include "HttpHeaders.h"
#include "TlsOptions.h"

namespace fiber::http {

struct HttpServerOptions {
    std::chrono::seconds keep_alive_timeout{70};
    std::chrono::seconds header_timeout{10};
    std::chrono::seconds body_timeout{60};
    std::chrono::seconds write_timeout{30};
    std::size_t header_init_size = 8 * 1024;
    std::size_t header_large_size = 32 * 1024;
    std::size_t header_large_num = 4;
    bool drain_unread_body = false;
    TlsOptions tls{};
};

struct BodyChunk {
    bool last = false;
    mem::IoBufChain data_chain;
};

using ReadBodyChunk = BodyChunk;

enum class ResponseBodyMode : std::uint8_t {
    Auto,
    ContentLength,
    Chunked,
};

enum class ResponseConnectionMode : std::uint8_t {
    Auto,
    Close,
};

class Http1Connection;
class Http1ExchangeIo;
class HttpExchangeIo;
class HttpTransport;
class RequestLineParser;
class HeaderLineParser;


class HttpExchange : public common::NonCopyable, public common::NonMovable {
public:
    explicit HttpExchange(const HttpServerOptions &options);
    ~HttpExchange();

    [[nodiscard]] HttpMethod method() const noexcept { return method_; }
    [[nodiscard]] HttpVersion version() const noexcept { return version_; }
    [[nodiscard]] const HttpUri &uri() const noexcept { return uri_; }
    std::string_view version_view() const noexcept { return version_view_; }
    std::string_view method_view() const noexcept { return method_view_; }
    std::string_view header(std::string_view name) const noexcept;
    const HttpHeaders &request_headers() const noexcept { return request_headers_; };
    const HttpHeaders &request_trailers() const noexcept { return request_trailers_; };
    bool request_trailers_complete() const noexcept { return request_trailers_complete_; }
    HttpHeaders &response_headers() noexcept { return response_headers_; };
    HttpHeaders &response_trailers() noexcept { return response_trailers_; }
    mem::BufPool &pool() noexcept { return pool_; }

    fiber::async::Task<common::IoResult<BodyChunk>> read_body(std::size_t max_bytes) noexcept;
    fiber::async::Task<common::IoResult<void>> discard_body() noexcept;

    void set_response_header(std::string_view name, std::string_view value);
    void set_response_content_length(size_t len);
    void set_response_chunked();
    void set_response_close();
    void set_response_trailer(std::string_view name, std::string_view value);

    fiber::async::Task<common::IoResult<void>> send_response_header(int status, std::string_view reason = {});
    fiber::async::Task<common::IoResult<void>> finish_response() noexcept;
    fiber::async::Task<common::IoResult<size_t>> write_body(BodyChunk chunk) noexcept;
    fiber::async::Task<common::IoResult<size_t>> write_body(const uint8_t *buf, size_t len, bool end) noexcept;


private:
    void set_io(HttpExchangeIo *io) noexcept;

    friend class RequestLineParser;
    friend class HeaderLineParser;
    friend class Http1Connection;
    friend class Http1ExchangeIo;

    fiber::mem::BufPool pool_;
    fiber::mem::IoBufChain header_bufs_;
    fiber::mem::IoBufChain trailer_bufs_;
    HttpMethod method_{};
    HttpVersion version_{};
    HttpUri uri_;
    std::string_view method_view_;
    std::string_view version_view_;
    HttpHeaders request_headers_;
    HttpHeaders request_trailers_;
    bool request_trailers_complete_ = false;

    HttpHeaders response_headers_;
    HttpHeaders response_trailers_;
    ResponseBodyMode response_body_mode_ = ResponseBodyMode::Auto;
    ResponseConnectionMode response_connection_mode_ = ResponseConnectionMode::Auto;
    size_t response_content_length_ = 0;
    bool request_chunked_ = false;
    bool request_content_length_set_ = false;
    size_t request_content_length_ = 0;
    bool request_close_ = false;
    bool request_keep_alive_ = false;
    HttpExchangeIo *io_ = nullptr;
};

using HttpHandler = std::function<fiber::async::Task<void>(HttpExchange &)>;

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP_EXCHANGE_H
