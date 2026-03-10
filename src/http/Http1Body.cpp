//
// Created by dear on 2026/2/1.
//

#include "Http1Body.h"
#include <limits>
namespace fiber::http {
void ChunkParser::reset() noexcept {
    state_ = State::ChunkStart;
    size_ = 0;
    length_ = 0;
}

void ChunkParser::consume(off_t n) noexcept {
    if (n <= 0) {
        return;
    }
    if (n >= size_) {
        size_ = 0;
        return;
    }
    size_ -= n;
}

ChunkParser::ParseCode ChunkParser::execute(mem::IoBuf *chain) noexcept {
    if (!chain) {
        return ParseCode::Error;
    }

    auto *begin = chain->readable_data();
    auto *pos = begin;
    auto *end = chain->writable_data();
    State state = state_;

    if (state == State::ChunkData && size_ == 0) {
        state = State::AfterData;
    }

    ParseCode rc = ParseCode::Again;

    for (; pos < end; ++pos) {
        unsigned char ch = *pos;
        unsigned char c;

        switch (state) {
            case State::ChunkStart:
                if (ch >= '0' && ch <= '9') {
                    state = State::ChunkSize;
                    size_ = ch - '0';
                    break;
                }
                c = static_cast<unsigned char>(ch | 0x20);
                if (c >= 'a' && c <= 'f') {
                    state = State::ChunkSize;
                    size_ = c - 'a' + 10;
                    break;
                }
                goto invalid;

            case State::ChunkSize:
                if (size_ > std::numeric_limits<off_t>::max() / 16) {
                    goto invalid;
                }
                if (ch >= '0' && ch <= '9') {
                    size_ = size_ * 16 + (ch - '0');
                    break;
                }
                c = static_cast<unsigned char>(ch | 0x20);
                if (c >= 'a' && c <= 'f') {
                    size_ = size_ * 16 + (c - 'a' + 10);
                    break;
                }
                if (size_ == 0) {
                    switch (ch) {
                        case '\r':
                            state = State::LastChunkExtensionAlmostDone;
                            break;
                        case '\n':
                            state = State::Trailer;
                            break;
                        case ';':
                        case ' ':
                        case '\t':
                            state = State::LastChunkExtension;
                            break;
                        default:
                            goto invalid;
                    }
                    break;
                }
                switch (ch) {
                    case '\r':
                        state = State::ChunkExtensionAlmostDone;
                        break;
                    case '\n':
                        state = State::ChunkData;
                        break;
                    case ';':
                    case ' ':
                    case '\t':
                        state = State::ChunkExtension;
                        break;
                    default:
                        goto invalid;
                }
                break;

            case State::ChunkExtension:
                switch (ch) {
                    case '\r':
                        state = State::ChunkExtensionAlmostDone;
                        break;
                    case '\n':
                        state = State::ChunkData;
                }
                break;

            case State::ChunkExtensionAlmostDone:
                if (ch == '\n') {
                    state = State::ChunkData;
                    break;
                }
                goto invalid;

            case State::ChunkData:
                rc = ParseCode::Ok;
                goto data;

            case State::AfterData:
                switch (ch) {
                    case '\r':
                        state = State::AfterDataAlmostDone;
                        break;
                    case '\n':
                        state = State::ChunkStart;
                        break;
                    default:
                        goto invalid;
                }
                break;

            case State::AfterDataAlmostDone:
                if (ch == '\n') {
                    state = State::ChunkStart;
                    break;
                }
                goto invalid;

            case State::LastChunkExtension:
                switch (ch) {
                    case '\r':
                        state = State::LastChunkExtensionAlmostDone;
                        break;
                    case '\n':
                        state = State::Trailer;
                }
                break;

            case State::LastChunkExtensionAlmostDone:
                if (ch == '\n') {
                    state = State::Trailer;
                    break;
                }
                goto invalid;

            case State::Trailer:
                switch (ch) {
                    case '\r':
                        state = State::TrailerAlmostDone;
                        break;
                    case '\n':
                        goto done;
                    default:
                        state = State::TrailerHeader;
                }
                break;

            case State::TrailerAlmostDone:
                if (ch == '\n') {
                    goto done;
                }
                goto invalid;

            case State::TrailerHeader:
                switch (ch) {
                    case '\r':
                        state = State::TrailerHeaderAlmostDone;
                        break;
                    case '\n':
                        state = State::Trailer;
                }
                break;

            case State::TrailerHeaderAlmostDone:
                if (ch == '\n') {
                    state = State::Trailer;
                    break;
                }
                goto invalid;
        }
    }

data:
    state_ = state;
    chain->consume(static_cast<std::size_t>(pos - begin));

    if (size_ > std::numeric_limits<off_t>::max() - 5) {
        return ParseCode::Error;
    }

    switch (state) {
        case State::ChunkStart:
            length_ = 3;
            break;
        case State::ChunkSize:
            length_ = 1 + (size_ ? size_ + 4 : 1);
            break;
        case State::ChunkExtension:
        case State::ChunkExtensionAlmostDone:
            length_ = 1 + size_ + 4;
            break;
        case State::ChunkData:
            length_ = size_ + 4;
            break;
        case State::AfterData:
        case State::AfterDataAlmostDone:
            length_ = 4;
            break;
        case State::LastChunkExtension:
        case State::LastChunkExtensionAlmostDone:
            length_ = 2;
            break;
        case State::Trailer:
        case State::TrailerAlmostDone:
            length_ = 1;
            break;
        case State::TrailerHeader:
        case State::TrailerHeaderAlmostDone:
            length_ = 2;
            break;
    }

    return rc;

done:
    state_ = State::ChunkStart;
    chain->consume(static_cast<std::size_t>((pos + 1) - begin));
    return ParseCode::Done;

invalid:
    return ParseCode::Error;
}
} // namespace fiber::http
