# HTTP/1 Parsing Design

## Goals
- Keep HTTP/1 request parsing inside `Http1Connection`.
- Run `HttpServer` as a pure HTTP/1 server; TLS ALPN is normalized to avoid
  advertising `h2`.
- Use `IoBuf` / `IoBufChain` for header input, request body buffering, and
  keep-alive carry-over bytes.
- Preserve zero-copy header views by letting `HttpExchange` retain header
  backing storage.

## Ownership and Lifetime
- `Http1Connection` owns:
  - `HttpTransport`
  - connection-scoped `IoBufChain inbound_bufs_`
  - the request parsing loop
- `HttpExchange` owns:
  - request metadata in `BufPool`
  - `header_bufs_`, an `IoBufChain` retaining raw header bytes referenced by
    header name/value views
- `Http1ExchangeIo` reads request body data from `Http1Connection::inbound_bufs_`
  and returns zero-copy `IoBufChain` chunks.

## Header Input Buffer Strategy
- Allocate an initial parse buffer with `header_init_size`.
- If request line or headers outgrow the current buffer, allocate a larger
  `IoBuf` by adding `header_large_size`.
- Allow at most `header_large_num` growth steps.
- If growth is exhausted, return `ParseCode::HeaderTooLarge`.

## Parser Roles
- `RequestLineParser`
  - parses method, URI, and version offsets inside the current `IoBuf`
- `HeaderLineParser`
  - parses one header line at a time
  - exposes name/value/hash for `HttpExchange::request_headers_`
- `Http1Connection`
  - owns the parsing loop
  - reads more bytes from transport when parsers return `Again`
  - appends trailing bytes after header completion back into `inbound_bufs_`

## Parsing Flow
1. Construct a fresh `HttpExchange` for the request.
2. Allocate a temporary header parse `IoBuf`.
3. Parse the request line incrementally.
4. Parse header lines incrementally.
5. On `Again`, first drain bytes already present in `inbound_bufs_`, then read
   more from transport.
6. When headers finish:
   - retain the header portion into `exchange.header_bufs_`
   - push any post-header trailing bytes back into `inbound_bufs_`

## Keep-Alive and Body Handoff
- `inbound_bufs_` is connection-scoped, so extra bytes belonging to the next
  request survive across requests.
- `Http1ExchangeIo::read_body(max_bytes)` slices only the current request body
  bytes from the front of `inbound_bufs_`.
- If an `IoBuf` contains both the tail of the current body and the head of the
  next request, body reads retain only the body subrange and leave the next
  request bytes in place.

## Error Handling
- malformed request line or header syntax returns the corresponding
  `ParseCode::Invalid*`
- transport read errors propagate as `IoErr`
- EOF during parsing is treated as connection reset
