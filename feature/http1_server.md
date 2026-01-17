# Http1 Server Design (llparse)

## Goals
- Provide coroutine-based HTTP/1.1 server with `HttpExchange` API.
- Parse request line/headers with llparse-generated parser.
- Support request bodies via `Content-Length` and `Transfer-Encoding: chunked`.
- Support response bodies via `Content-Length` and chunked encoding.
- Default keep-alive timeout: 70s, close on idle timeout.

## API Sketch
```cpp
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
};

struct ReadBodyResult {
    size_t size = 0;
    bool end = false;
};

class HttpExchange : public common::NonCopyable, public common::NonMovable {
public:
    std::string_view method() const;
    std::string_view target() const;
    std::string_view version() const;
    std::string_view header(std::string_view name) const;
    const HttpHeaders &request_headers() const;
    HttpHeaders &response_headers();
    BufPool &pool() noexcept;
    bool request_chunked() const;
    size_t request_content_length() const;

    HttpTask<fiber::common::IoResult<ReadBodyResult>> read_body(void *buf, size_t len) noexcept;
    HttpTask<fiber::common::IoResult<void>> discard_body() noexcept;

    void set_response_header(std::string_view name, std::string_view value);
    void set_response_content_length(size_t len);
    void set_response_chunked();
    void set_response_close();

    HttpTask<fiber::common::IoResult<void>> send_response_header(int status,
                                                                 std::string_view reason = {});
    HttpTask<fiber::common::IoResult<size_t>> write_body(const void *buf,
                                                         size_t len,
                                                         bool end) noexcept;
};

using HttpHandler = std::function<fiber::http::HttpTask<void>(HttpExchange &)>;

class Http1Server : public common::NonCopyable, public common::NonMovable {
public:
    Http1Server(event::EventLoop &loop, HttpHandler handler);
    fiber::common::IoResult<void> bind(const net::SocketAddress &addr,
                                       const net::ListenOptions &options);
    void close();
};

} // namespace fiber::http
```

## HttpHeaders & BufPool
- `BufPool` is a per-exchange arena (ngx_pool_t style). All request/response allocations use it
  and are released together on `HttpExchange::reset()` or destruction.
- `BufPool` provides aligned allocations and a `reset()` that frees all blocks (or keeps the first
  block for reuse). No individual frees.
- `HttpHeaders` stores header bytes in `BufPool` and indexes fields via a custom hash table.
  `HeaderField` only contains pointers/lengths into the pool.

### BufPool sketch
```cpp
class BufPool : public common::NonCopyable, public common::NonMovable {
public:
    explicit BufPool(size_t block_size = 4096);

    void *alloc(size_t size, size_t align = alignof(std::max_align_t));
    template <typename T> T *alloc(size_t n = 1);
    void reset();
};
```

### HttpHeaders sketch
```cpp
class HttpHeaders {
public:
    struct HeaderField {
        const char *name = nullptr;
        uint32_t name_len = 0;
        const char *value = nullptr;
        uint32_t value_len = 0;
        uint64_t name_hash = 0;
        uint32_t next = 0;
        std::string_view name_view() const noexcept;
        std::string_view value_view() const noexcept;
    };

    explicit HttpHeaders(BufPool &pool);

    bool add(std::string_view name, std::string_view value);
    bool set(std::string_view name, std::string_view value);
    std::string_view get(std::string_view name) const noexcept;
    size_t remove(std::string_view name) noexcept;

    void reserve_bytes(size_t bytes);
    void clear() noexcept;
    void release() noexcept;
    size_t size() const noexcept;

    std::vector<HeaderField>::const_iterator begin() const noexcept;
    std::vector<HeaderField>::const_iterator end() const noexcept;
};
```

### Hash table layout
- `fields_`: insertion order array of `HeaderField` with `next` for bucket chaining.
- `bucket_head_` + `bucket_tail_`: vectors sized to power-of-two; `kInvalid = 0xFFFFFFFF`.
- Hash: ASCII case-insensitive FNV-1a. Match uses hash + length + ASCII case-insensitive compare.
- `add` appends to `fields_` and chains via `bucket_tail_`.
- `remove` compacts `fields_` and rebuilds buckets in one pass (O(n) for small header counts).

### Reset order
- Because vectors allocate from `BufPool`, `HttpHeaders::release()` must be called before
  `BufPool::reset()` to avoid dangling pointers inside the vectors.

## Parsing with llparse
- Use llhttp (llparse-generated C) under `third_party/llparse/http1/generated/`.
- `third_party/llparse/http1/grammar.js` tracks the HTTP/1 request grammar:
  - request-line: `method SP target SP HTTP-version CRLF`
  - headers: `field-name ":" OWS field-value CRLF`
  - end-of-headers: `CRLF`
- llparse callbacks capture spans for method/target/version/header name/value.
- `Http1Parser` wraps generated C parser and exposes:
  - `ParseResult parse(const char *data, size_t len, size_t &consumed)`
  - `reset()` for next request
  - parsed fields stored in `HttpExchange`.

## Body Handling
- If `Transfer-Encoding: chunked` is present, ignore `Content-Length` per RFC.
- Chunked decoding is handled in `HttpExchange::read_body`:
  - parse hex size line, ignore extensions
  - read exact chunk bytes, then CRLF
  - size `0` terminates; trailers are consumed and ignored
- `Content-Length`: read exactly N bytes.
- No body for methods with neither CL nor chunked.

## Response Flow
- `send_response_header()` sends status line + headers.
- If no content length set:
  - HTTP/1.1 defaults to chunked
  - HTTP/1.0 forces `Connection: close`
- `write_body()`:
  - chunked: writes `<hex>\r\n<data>\r\n`, `end=true` writes `0\r\n\r\n`
  - content-length: writes raw bytes, validates total length

## Timeouts & Limits
- `keep_alive_timeout`: idle time between requests (default 70s).
- `header_timeout`: first line + headers deadline.
- `body_timeout`: applies to body reads.
- `write_timeout`: applies to response writes.
- `max_header_bytes`, `max_body_bytes`, `max_chunk_bytes` enforce limits.
- Over limits return 431/413 and close.

## Connection Rules
- HTTP/1.1: keep-alive by default, unless `Connection: close`.
- HTTP/1.0: close by default, unless `Connection: keep-alive`.
- If handler leaves body unread:
  - `drain_unread_body=false`: close connection after response.
  - `drain_unread_body=true`: discard remaining body before next request.
