# HTTP/1 Parsing Plan

## Goals
- Provide a reusable HTTP/1 parsing loop without `Http1Connection`.
- Keep parsing state in a lightweight `Http1Context` that owns header buffers and pool.
- Parse request line and headers incrementally: read a bit, parse a bit.

## Ownership and Lifetime
- `Http1Context` owns:
  - `header_pool_` for header storage.
  - `HeaderBuffers` for header input chains.
  - `HttpTransport*` for IO.
- `HttpExchange` is provided by the caller and filled during parsing.

## Header Input Buffer Strategy (nginx-style)
- Allocate initial header buffer: 8 KB.
- If request line / headers exceed current buffer, allocate a large buffer: 32 KB.
- Allow up to 4 large buffers.
- If `HeaderBuffers` is exhausted, parsing fails.

## Parser Roles
- `RequestLineParser`
  - Parses method/URI/version offsets and fills `HttpExchange` when complete.
- `HeaderLineParser`
  - Parses a single header line per call.
  - Exposes name/value/hash for the caller to store.
- `BodyParser`
  - Reserved for future body parsing; not used in header parsing.

## Parsing Flow
1. Reset `HeaderBuffers` and `HttpExchange`.
2. Read data into the current `BufChain`.
3. Parse request line until `Ok`.
4. Parse header lines until `HeaderDone`.
5. On `Again`, read more data and continue.

## Error Handling
- If `HeaderBuffers::alloc()` returns `nullptr`, fail the request (headers too large).
- Parser returns `Invalid*` codes on malformed input.
- IO errors propagate as `IoErr`.
