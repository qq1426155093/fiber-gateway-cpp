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

struct ReadBodyResult {
    std::size_t size = 0;
    bool end = false;
};

class Http1Context;
class Http1ExchangeIo;
class Http2Connection;
class Http2ExchangeIo;
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
    HttpHeaders &response_headers() noexcept { return response_headers_; };
    mem::BufPool &pool() noexcept { return pool_; }

    fiber::async::Task<common::IoResult<ReadBodyResult>> read_body(void *buf, size_t len) noexcept;
    fiber::async::Task<common::IoResult<void>> discard_body() noexcept;

    void set_response_header(std::string_view name, std::string_view value);
    void set_response_content_length(size_t len);
    void set_response_chunked();
    void set_response_close();

    fiber::async::Task<common::IoResult<void>> send_response_header(int status, std::string_view reason = {});
    fiber::async::Task<common::IoResult<size_t>> write_body(const uint8_t *buf, size_t len, bool end) noexcept;


private:
    void set_io(std::unique_ptr<HttpExchangeIo> io) noexcept;

    friend class RequestLineParser;
    friend class HeaderLineParser;
    friend class Http1Context;
    friend class Http1ExchangeIo;
    friend class Http2Connection;
    friend class Http2ExchangeIo;

    fiber::mem::BufPool pool_;
    HttpMethod method_{};
    HttpVersion version_{};
    HttpUri uri_;
    std::string_view method_view_;
    std::string_view version_view_;
    HttpHeaders request_headers_;

    HttpHeaders response_headers_;
    bool response_chunked_ = false;
    bool response_close_ = false;
    bool response_content_length_set_ = false;
    size_t response_content_length_ = 0;
    bool request_chunked_ = false;
    bool request_content_length_set_ = false;
    size_t request_content_length_ = 0;
    bool request_close_ = false;
    bool request_keep_alive_ = false;
    std::unique_ptr<HttpExchangeIo> io_;
};

using HttpHandler = std::function<fiber::async::Task<void>(HttpExchange &)>;

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP_EXCHANGE_H
