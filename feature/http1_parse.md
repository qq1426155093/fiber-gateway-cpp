# HTTP/1 Parsing Refactor Plan

## Goals
- Remove `Http1Parser` as a member from `Http1Connection` and `HttpExchange`.
- Split parsing into `RequestLineParser`, `HeaderLineParser`, and `BodyParser`.
- Keep request/response semantic fields in `HttpExchange` only.
- Store body parsing state in `HttpExchange` via `BodyParser`.
- Store header parsing buffers and allocator in `Http1Connection`.

## Ownership and Lifetime
- `Http1Connection` owns:
  - `header_pool_` for header parsing and request/response header storage.
  - Header input buffers (initial + large buffers).
- `HttpExchange` owns:
  - Request/response semantic fields.
  - `BodyParser` and body parsing buffers/state.
- Response header encoding uses a separate temporary buffer (heap `std::string`), not `header_pool_`.

## Header Input Buffer Strategy (nginx-style)
- Allocate initial header buffer: 8 KB.
- If request line / headers exceed the current buffer, allocate a large header buffer: 32 KB.
- Allow up to 4 large buffers.
- Total header buffer capacity: 8 KB + 4 * 32 KB = 136 KB.
- Enforce `options.max_header_bytes` separately. If parsed bytes exceed it, fail parsing.
- If large buffer limit is exceeded, fail parsing.

## Parser Roles
- `RequestLineParser`
  - Keeps its own `state` enum and request-line offsets.
  - Consumes an external buffer pointer and writes method/target/version into `HttpExchange`.
  - Populates `request_http_major_` and `request_http_minor_`.
- `HeaderLineParser`
  - Keeps its own `state` enum and header parsing offsets/hash/lowcase buffer.
  - Consumes the same external buffer pointer.
  - Populates request headers and updates content-length, chunked, keep-alive, expect-continue.
- `BodyParser`
  - Stored in `HttpExchange` for cross-call body parsing.
  - Keeps its own `state` enum and chunk/body counters.
  - Consumes external buffer pointer and writes to `body_buffer_` / `body_complete_`.

## HttpExchange Changes
- Add `request_http_major_` and `request_http_minor_`.
- Add `BodyParser body_parser_`.
- Construct `HttpHeaders` with `Http1Connection::header_pool_`.
- No `parse_state` added.

## Http1Connection Changes
- Add `mem::BufPool header_pool_`.
- Allocate and manage header input buffers per request.
- Use local `RequestLineParser` and `HeaderLineParser` during `run()`.
- Delegate body parsing to `exchange_.body_parser_` in `read_body()`.

## Error Handling
- Header oversize:
  - If parsed header bytes exceed `options.max_header_bytes`, fail (431/400).
- Buffer oversize:
  - If initial + large header buffer limit is exceeded, fail (431/400).
- Body oversize:
  - If `options.max_body_bytes` or `options.max_chunk_bytes` exceeded, fail (413).
