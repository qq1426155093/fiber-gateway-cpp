#include "Http2SendPayload.h"

#include "HttpTransport.h"

namespace fiber::http {

Http2SendPayload::~Http2SendPayload() { reset(); }

Http2SendPayload::Http2SendPayload(Http2SendPayload &&other) noexcept { move_from(std::move(other)); }

Http2SendPayload &Http2SendPayload::operator=(Http2SendPayload &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    reset();
    move_from(std::move(other));
    return *this;
}

Http2SendPayload::Kind Http2SendPayload::kind() const noexcept { return kind_; }

bool Http2SendPayload::empty() const noexcept { return readable_bytes() == 0; }

std::size_t Http2SendPayload::readable_bytes() const noexcept {
    switch (kind_) {
        case Kind::StableSpan:
            return span().length - span().offset;
        case Kind::IoBuf:
            return buf().readable();
        case Kind::IoBufChain:
            return chain().readable_bytes();
        case Kind::None:
        default:
            return 0;
    }
}

void Http2SendPayload::reset() noexcept {
    switch (kind_) {
        case Kind::StableSpan:
            span().~Http2StableSpan();
            break;
        case Kind::IoBuf:
            buf().~IoBuf();
            break;
        case Kind::IoBufChain:
            chain().~IoBufChain();
            break;
        case Kind::None:
        default:
            break;
    }
    kind_ = Kind::None;
}

void Http2SendPayload::set_stable_span(const std::uint8_t *data, std::size_t length) noexcept {
    reset();
    new (&storage_.span) Http2StableSpan{data, length, 0};
    kind_ = Kind::StableSpan;
}

void Http2SendPayload::set_buf(mem::IoBuf &&buf) noexcept {
    reset();
    new (&storage_.buf) mem::IoBuf(std::move(buf));
    kind_ = Kind::IoBuf;
}

void Http2SendPayload::set_chain(mem::IoBufChain &&bufs) noexcept {
    reset();
    new (&storage_.chain) mem::IoBufChain(std::move(bufs));
    kind_ = Kind::IoBufChain;
}

bool Http2SendPayload::split_prefix_to(std::size_t bytes, Http2SendPayload &out) noexcept {
    out.reset();

    switch (kind_) {
        case Kind::StableSpan: {
            Http2StableSpan &value = span();
            if (bytes > value.length - value.offset) {
                return false;
            }
            out.set_stable_span(value.data + value.offset, bytes);
            value.offset += bytes;
            return true;
        }
        case Kind::IoBuf: {
            if (bytes > buf().readable()) {
                return false;
            }
            mem::IoBuf slice = buf().retain_slice(0, bytes);
            if (!slice && bytes != 0) {
                return false;
            }
            buf().consume(bytes);
            out.set_buf(std::move(slice));
            return true;
        }
        case Kind::IoBufChain: {
            if (bytes > chain().readable_bytes()) {
                return false;
            }
            mem::IoBufChain prefix;
            if (!chain().retain_prefix(bytes, prefix)) {
                return false;
            }
            chain().consume_and_compact(bytes);
            out.set_chain(std::move(prefix));
            return true;
        }
        case Kind::None:
        default:
            return bytes == 0;
    }
}

fiber::async::Task<common::IoResult<size_t>>
Http2SendPayload::write_once(HttpTransport &transport, std::chrono::milliseconds timeout) noexcept {
    switch (kind_) {
        case Kind::StableSpan: {
            Http2StableSpan &value = span();
            std::size_t remaining = value.length - value.offset;
            auto result = co_await transport.write(value.data + value.offset, remaining, timeout);
            if (result) {
                value.offset += *result;
            }
            co_return result;
        }
        case Kind::IoBuf: {
            mem::IoBuf &value = buf();
            auto result = co_await transport.write(value.readable_data(), value.readable(), timeout);
            if (result) {
                value.consume(*result);
            }
            co_return result;
        }
        case Kind::IoBufChain:
            co_return co_await transport.writev(chain(), timeout);
        case Kind::None:
        default:
            co_return static_cast<size_t>(0);
    }
}

void Http2SendPayload::move_from(Http2SendPayload &&other) noexcept {
    switch (other.kind_) {
        case Kind::StableSpan:
            new (&storage_.span) Http2StableSpan(other.span());
            kind_ = Kind::StableSpan;
            break;
        case Kind::IoBuf:
            new (&storage_.buf) mem::IoBuf(std::move(other.buf()));
            kind_ = Kind::IoBuf;
            break;
        case Kind::IoBufChain:
            new (&storage_.chain) mem::IoBufChain(std::move(other.chain()));
            kind_ = Kind::IoBufChain;
            break;
        case Kind::None:
        default:
            kind_ = Kind::None;
            break;
    }
    other.reset();
}

Http2StableSpan &Http2SendPayload::span() noexcept { return storage_.span; }

const Http2StableSpan &Http2SendPayload::span() const noexcept { return storage_.span; }

mem::IoBuf &Http2SendPayload::buf() noexcept { return storage_.buf; }

const mem::IoBuf &Http2SendPayload::buf() const noexcept { return storage_.buf; }

mem::IoBufChain &Http2SendPayload::chain() noexcept { return storage_.chain; }

const mem::IoBufChain &Http2SendPayload::chain() const noexcept { return storage_.chain; }

} // namespace fiber::http
