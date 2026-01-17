#ifndef FIBER_HTTP_HTTP_EXCHANGE_H
#define FIBER_HTTP_HTTP_EXCHANGE_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../common/mem/BufPool.h"
#include "../async/Task.h"
#include "HttpHeaders.h"
#include "TlsOptions.h"

namespace fiber::http {

struct HttpServerOptions {
    std::chrono::seconds keep_alive_timeout{70};
    std::chrono::seconds header_timeout{10};
    std::chrono::seconds body_timeout{60};
    std::chrono::seconds write_timeout{30};
    size_t max_header_bytes = 16 * 1024;
    size_t max_body_bytes = 16 * 1024 * 1024;
    size_t max_chunk_bytes = 4 * 1024 * 1024;
    bool auto_100_continue = true;
    bool drain_unread_body = false;
    TlsOptions tls{};
};

struct ReadBodyResult {
    size_t size = 0;
    bool end = false;
};

class Http1Connection;
class Http1Parser;

class HttpExchange : public common::NonCopyable, public common::NonMovable {
public:
    std::string_view method() const noexcept;
    std::string_view target() const noexcept;
    std::string_view version() const noexcept;
    std::string_view header(std::string_view name) const noexcept;
    const HttpHeaders &request_headers() const noexcept;
    HttpHeaders &response_headers() noexcept;
    mem::BufPool &pool() noexcept;
    bool request_chunked() const noexcept;
    size_t request_content_length() const noexcept;

    fiber::async::Task<common::IoResult<ReadBodyResult>> read_body(void *buf, size_t len) noexcept;
    fiber::async::Task<common::IoResult<void>> discard_body() noexcept;

    void set_response_header(std::string_view name, std::string_view value);
    void set_response_content_length(size_t len);
    void set_response_chunked();
    void set_response_close();

    fiber::async::Task<common::IoResult<void>> send_response_header(int status,
                                                                    std::string_view reason = {});
    fiber::async::Task<common::IoResult<size_t>> write_body(const void *buf,
                                                            size_t len,
                                                            bool end) noexcept;

private:
    friend class Http1Connection;
    friend class Http1Parser;

    explicit HttpExchange(Http1Connection &connection, const HttpServerOptions &options);
    void reset();

    Http1Connection *connection_ = nullptr;
    const HttpServerOptions *options_ = nullptr;

    std::string method_;
    std::string target_;
    std::string version_;
    mem::BufPool pool_;
    HttpHeaders request_headers_;

    bool request_chunked_ = false;
    bool request_expect_continue_ = false;
    bool request_keep_alive_ = true;
    size_t request_content_length_ = 0;
    bool request_content_length_set_ = false;

    HttpHeaders response_headers_;
    bool response_chunked_ = false;
    bool response_header_sent_ = false;
    bool response_complete_ = false;
    bool response_close_ = false;
    bool response_content_length_set_ = false;
    size_t response_content_length_ = 0;
    size_t response_body_sent_ = 0;

    std::string body_buffer_;
    bool body_complete_ = false;
    bool continue_sent_ = false;
};

using HttpHandler = std::function<fiber::async::Task<void>(HttpExchange &)>;

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP_EXCHANGE_H
