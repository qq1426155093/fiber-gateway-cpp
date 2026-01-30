#include "Http1Parser.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <string_view>

#include "HttpExchange.h"

namespace fiber::http {

namespace {

constexpr int kChunkDataState = 4;
constexpr size_t kMaxOffT = std::numeric_limits<size_t>::max();
constexpr size_t kInvalidPos = std::numeric_limits<size_t>::max();

static const uint32_t usual[] = {
        0x00000000, 0x7fff37d6,
#if defined(_WIN32)
        0xefffffff,
#else
        0xffffffff,
#endif
        0x7fffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
};

constexpr uint64_t pack_u64(char c0, char c1, char c2, char c3, char c4, char c5, char c6, char c7) {
    return static_cast<uint64_t>(static_cast<unsigned char>(c0)) |
           (static_cast<uint64_t>(static_cast<unsigned char>(c1)) << 8) |
           (static_cast<uint64_t>(static_cast<unsigned char>(c2)) << 16) |
           (static_cast<uint64_t>(static_cast<unsigned char>(c3)) << 24) |
           (static_cast<uint64_t>(static_cast<unsigned char>(c4)) << 32) |
           (static_cast<uint64_t>(static_cast<unsigned char>(c5)) << 40) |
           (static_cast<uint64_t>(static_cast<unsigned char>(c6)) << 48) |
           (static_cast<uint64_t>(static_cast<unsigned char>(c7)) << 56);
}

constexpr uint32_t pack_u32(char c0, char c1, char c2, char c3) {
    return static_cast<uint32_t>(static_cast<unsigned char>(c0)) |
           (static_cast<uint32_t>(static_cast<unsigned char>(c1)) << 8) |
           (static_cast<uint32_t>(static_cast<unsigned char>(c2)) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(c3)) << 24);
}

inline uint64_t load_u64_le(const char *p, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) {
        v |= static_cast<uint64_t>(static_cast<unsigned char>(p[i])) << (i * 8);
    }
    return v;
}

inline uint32_t load_u32_le(const char *p, size_t n) {
    uint32_t v = 0;
    for (size_t i = 0; i < n; ++i) {
        v |= static_cast<uint32_t>(static_cast<unsigned char>(p[i])) << (i * 8);
    }
    return v;
}

inline bool str3_cmp(const char *m, char c0, char c1, char c2, char) {
    return load_u32_le(m, 3) == pack_u32(c0, c1, c2, 0);
}

inline bool str3Ocmp(const char *m, char c0, char, char c2, char c3) { return m[0] == c0 && m[2] == c2 && m[3] == c3; }

inline bool str4cmp(const char *m, char c0, char c1, char c2, char c3) {
    return load_u32_le(m, 4) == pack_u32(c0, c1, c2, c3);
}

inline bool str5cmp(const char *m, char c0, char c1, char c2, char c3, char c4) {
    return load_u64_le(m, 5) == pack_u64(c0, c1, c2, c3, c4, 0, 0, 0);
}

inline bool str6cmp(const char *m, char c0, char c1, char c2, char c3, char c4, char c5) {
    return load_u64_le(m, 6) == pack_u64(c0, c1, c2, c3, c4, c5, 0, 0);
}

inline bool str7cmp(const char *m, char c0, char c1, char c2, char c3, char c4, char c5, char c6) {
    return load_u64_le(m, 7) == pack_u64(c0, c1, c2, c3, c4, c5, c6, 0);
}

inline bool str8cmp(const char *m, char c0, char c1, char c2, char c3, char c4, char c5, char c6, char c7) {
    return load_u64_le(m, 8) == pack_u64(c0, c1, c2, c3, c4, c5, c6, c7);
}

inline bool str9cmp(const char *m, char c0, char c1, char c2, char c3, char c4, char c5, char c6, char c7, char c8) {
    return load_u64_le(m, 8) == pack_u64(c0, c1, c2, c3, c4, c5, c6, c7) &&
           static_cast<unsigned char>(m[8]) == static_cast<unsigned char>(c8);
}

inline uint32_t ngx_hash(uint32_t key, unsigned char c) { return key * 31u + static_cast<uint32_t>(c); }

constexpr uint32_t ngx_hash_lower(const char *s, size_t n) {
    uint32_t h = 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<unsigned char>(c - 'A' + 'a');
        }
        h = h * 31u + c;
    }
    return h;
}

inline bool match_content_length(uint32_t hash, size_t len, const char *lc) {
    constexpr uint32_t content_length_hash = ngx_hash_lower("content-length", 14);
    return len == 14 && hash == content_length_hash && std::memcmp(lc, "content-length", 14) == 0;
}

inline bool match_transfer_encoding(uint32_t hash, size_t len, const char *lc) {
    constexpr uint32_t transfer_encoding_hash = ngx_hash_lower("transfer-encoding", 17);
    return len == 17 && hash == transfer_encoding_hash && std::memcmp(lc, "transfer-encoding", 17) == 0;
}

inline bool match_expect(uint32_t hash, size_t len, const char *lc) {
    constexpr uint32_t expect_hash = ngx_hash_lower("expect", 6);
    return len == 6 && hash == expect_hash && std::memcmp(lc, "expect", 6) == 0;
}

inline bool match_connection(uint32_t hash, size_t len, const char *lc) {
    constexpr uint32_t connection_hash = ngx_hash_lower("connection", 10);
    return len == 10 && hash == connection_hash && std::memcmp(lc, "connection", 10) == 0;
}

bool has_token(std::string_view value, std::string_view token) {
    size_t pos = 0;
    for (;;) {
        pos = value.find_first_not_of(" \t", pos);
        if (pos == std::string_view::npos) {
            return false;
        }
        size_t end = value.find_first_of(",", pos);
        size_t len = (end == std::string_view::npos) ? value.size() - pos : end - pos;
        size_t tlen = token.size();
        if (len >= tlen && value.compare(pos, tlen, token) == 0) {
            return true;
        }
        if (end == std::string_view::npos) {
            return false;
        }
        pos = end + 1;
    }
}

bool parse_transfer_encoding(std::string_view value, bool &chunked) {
    size_t pos = 0;
    for (;;) {
        pos = value.find_first_not_of(" \t", pos);
        if (pos == std::string_view::npos) {
            return true;
        }
        size_t end = value.find_first_of(",", pos);
        size_t len = (end == std::string_view::npos) ? value.size() - pos : end - pos;
        std::string_view token = value.substr(pos, len);
        size_t trim_end = token.find_last_not_of(" \t");
        if (trim_end != std::string_view::npos) {
            token = token.substr(0, trim_end + 1);
        }
        if (token == "chunked") {
            chunked = true;
        } else if (token != "identity") {
            return false;
        }
        if (end == std::string_view::npos) {
            return true;
        }
        pos = end + 1;
    }
}

size_t buffer_offset(const ParseBuffer &buffer, const char *p) { return static_cast<size_t>(p - buffer.start); }

ParseCode parse_request_line(RequestLineParser::RequestLineState &r, RequestLineParser::State &state, ParseBuffer &b) {
    unsigned char c = 0;
    unsigned char ch = 0;
    const char *p = nullptr;
    const char *m = nullptr;

    for (p = b.pos; p < b.last; p++) {
        ch = static_cast<unsigned char>(*p);

        switch (state) {

            case RequestLineParser::State::Start:
                r.request_start = buffer_offset(b, p);

                if (ch == '\r' || ch == '\n') {
                    break;
                }

                if ((ch < 'A' || ch > 'Z') && ch != '_' && ch != '-') {
                    return ParseCode::InvalidMethod;
                }

                state = RequestLineParser::State::Method;
                break;

            case RequestLineParser::State::Method:
                if (ch == ' ') {
                    r.method_end = buffer_offset(b, p) - 1;
                    m = b.start + r.request_start;

                    switch (p - m) {
                        case 3:
                            if (str3_cmp(m, 'G', 'E', 'T', ' ')) {
                                r.method = HttpMethod::Get;
                                break;
                            }
                            if (str3_cmp(m, 'P', 'U', 'T', ' ')) {
                                r.method = HttpMethod::Put;
                                break;
                            }
                            break;
                        case 4:
                            if (m[1] == 'O') {
                                if (str3Ocmp(m, 'P', 'O', 'S', 'T')) {
                                    r.method = HttpMethod::Post;
                                    break;
                                }
                                if (str3Ocmp(m, 'C', 'O', 'P', 'Y')) {
                                    r.method = HttpMethod::Copy;
                                    break;
                                }
                                if (str3Ocmp(m, 'M', 'O', 'V', 'E')) {
                                    r.method = HttpMethod::Move;
                                    break;
                                }
                                if (str3Ocmp(m, 'L', 'O', 'C', 'K')) {
                                    r.method = HttpMethod::Lock;
                                    break;
                                }
                            } else {
                                if (str4cmp(m, 'H', 'E', 'A', 'D')) {
                                    r.method = HttpMethod::Head;
                                    break;
                                }
                            }
                            break;
                        case 5:
                            if (str5cmp(m, 'M', 'K', 'C', 'O', 'L')) {
                                r.method = HttpMethod::MKCOL;
                                break;
                            }
                            if (str5cmp(m, 'P', 'A', 'T', 'C', 'H')) {
                                r.method = HttpMethod::Patch;
                                break;
                            }
                            if (str5cmp(m, 'T', 'R', 'A', 'C', 'E')) {
                                r.method = HttpMethod::Trace;
                                break;
                            }
                            break;
                        case 6:
                            if (str6cmp(m, 'D', 'E', 'L', 'E', 'T', 'E')) {
                                r.method = HttpMethod::Delete;
                                break;
                            }
                            if (str6cmp(m, 'U', 'N', 'L', 'O', 'C', 'K')) {
                                r.method = HttpMethod::Unlock;

                                break;
                            }
                            break;
                        case 7:
                            if (str7cmp(m, 'O', 'P', 'T', 'I', 'O', 'N', 'S')) {
                                r.method = HttpMethod::Options;
                                break;
                            }
                            if (str7cmp(m, 'C', 'O', 'N', 'N', 'E', 'C', 'T')) {
                                r.method = HttpMethod::Connect;
                                break;
                            }
                            break;
                        case 8:
                            if (str8cmp(m, 'P', 'R', 'O', 'P', 'F', 'I', 'N', 'D')) {
                                r.method = HttpMethod::PropFind;
                                break;
                            }
                            break;
                        case 9:
                            if (str9cmp(m, 'P', 'R', 'O', 'P', 'P', 'A', 'T', 'C', 'H')) {
                                r.method = HttpMethod::PropPatch;
                                break;
                            }
                            break;
                    }

                    state = RequestLineParser::State::SpacesBeforeUri;
                    break;
                }

                if ((ch < 'A' || ch > 'Z') && ch != '_' && ch != '-') {
                    return ParseCode::InvalidMethod;
                }

                break;

            case RequestLineParser::State::SpacesBeforeUri:
                if (ch == '/') {
                    r.uri_start = buffer_offset(b, p);
                    state = RequestLineParser::State::AfterSlashInUri;
                    break;
                }

                c = static_cast<unsigned char>(ch | 0x20u);
                if (c >= 'a' && c <= 'z') {
                    r.schema_start = buffer_offset(b, p);
                    state = RequestLineParser::State::Schema;
                    break;
                }

                switch (ch) {
                    case ' ':
                        break;
                    default:
                        return ParseCode::InvalidRequest;
                }
                break;

            case RequestLineParser::State::Schema:
                c = static_cast<unsigned char>(ch | 0x20u);
                if (c >= 'a' && c <= 'z') {
                    break;
                }
                if ((ch >= '0' && ch <= '9') || ch == '+' || ch == '-' || ch == '.') {
                    break;
                }
                switch (ch) {
                    case ':':
                        r.schema_end = buffer_offset(b, p);
                        state = RequestLineParser::State::SchemaSlash;
                        break;
                    default:
                        return ParseCode::InvalidRequest;
                }
                break;

            case RequestLineParser::State::SchemaSlash:
                switch (ch) {
                    case '/':
                        state = RequestLineParser::State::SchemaSlashSlash;
                        break;
                    default:
                        return ParseCode::InvalidRequest;
                }
                break;

            case RequestLineParser::State::SchemaSlashSlash:
                switch (ch) {
                    case '/':
                        state = RequestLineParser::State::HostStart;
                        break;
                    default:
                        return ParseCode::InvalidRequest;
                }
                break;

            case RequestLineParser::State::HostStart:
                r.host_start = buffer_offset(b, p);
                if (ch == '[') {
                    state = RequestLineParser::State::HostIpLiteral;
                    break;
                }
                state = RequestLineParser::State::Host;
                [[fallthrough]];

            case RequestLineParser::State::Host:
                c = static_cast<unsigned char>(ch | 0x20u);
                if (c >= 'a' && c <= 'z') {
                    break;
                }
                if ((ch >= '0' && ch <= '9') || ch == '.' || ch == '-') {
                    break;
                }
                [[fallthrough]];

            case RequestLineParser::State::HostEnd:
                r.host_end = buffer_offset(b, p);
                switch (ch) {
                    case ':':
                        state = RequestLineParser::State::Port;
                        break;
                    case '/':
                        r.uri_start = buffer_offset(b, p);
                        state = RequestLineParser::State::AfterSlashInUri;
                        break;
                    case '?':
                        r.uri_start = buffer_offset(b, p);
                        r.args_start = buffer_offset(b, p) + 1;
                        r.empty_path_in_uri = true;
                        state = RequestLineParser::State::Uri;
                        break;
                    case ' ':
                        if (r.schema_end != kInvalidPos) {
                            r.uri_start = r.schema_end + 1;
                            r.uri_end = r.schema_end + 2;
                        }
                        state = RequestLineParser::State::Http09;
                        break;
                    default:
                        return ParseCode::InvalidRequest;
                }
                break;

            case RequestLineParser::State::HostIpLiteral:
                if (ch >= '0' && ch <= '9') {
                    break;
                }
                c = static_cast<unsigned char>(ch | 0x20u);
                if (c >= 'a' && c <= 'z') {
                    break;
                }
                switch (ch) {
                    case ':':
                        break;
                    case ']':
                        state = RequestLineParser::State::HostEnd;
                        break;
                    case '-':
                    case '.':
                    case '_':
                    case '~':
                        break;
                    case '!':
                    case '$':
                    case '&':
                    case '\'':
                    case '(':
                    case ')':
                    case '*':
                    case '+':
                    case ',':
                    case ';':
                    case '=':
                        break;
                    default:
                        return ParseCode::InvalidRequest;
                }
                break;

            case RequestLineParser::State::Port:
                if (ch >= '0' && ch <= '9') {
                    break;
                }
                switch (ch) {
                    case '/':
                        r.uri_start = buffer_offset(b, p);
                        state = RequestLineParser::State::AfterSlashInUri;
                        break;
                    case '?':
                        r.uri_start = buffer_offset(b, p);
                        r.args_start = buffer_offset(b, p) + 1;
                        r.empty_path_in_uri = true;
                        state = RequestLineParser::State::Uri;
                        break;
                    case ' ':
                        if (r.schema_end != kInvalidPos) {
                            r.uri_start = r.schema_end + 1;
                            r.uri_end = r.schema_end + 2;
                        }
                        state = RequestLineParser::State::Http09;
                        break;
                    default:
                        return ParseCode::InvalidRequest;
                }
                break;

            case RequestLineParser::State::AfterSlashInUri:
                if (usual[ch >> 5] & (1U << (ch & 0x1f))) {
                    state = RequestLineParser::State::CheckUri;
                    break;
                }
                switch (ch) {
                    case ' ':
                        r.uri_end = buffer_offset(b, p);
                        state = RequestLineParser::State::Http09;
                        break;
                    case '\r':
                        r.uri_end = buffer_offset(b, p);
                        r.http_minor = 9;
                        state = RequestLineParser::State::AlmostDone;
                        break;
                    case '\n':
                        r.uri_end = buffer_offset(b, p);
                        r.http_minor = 9;
                        goto done;
                    case '.':
                        r.complex_uri = true;
                        state = RequestLineParser::State::Uri;
                        break;
                    case '%':
                        r.quoted_uri = true;
                        state = RequestLineParser::State::Uri;
                        break;
                    case '/':
                        r.complex_uri = true;
                        state = RequestLineParser::State::Uri;
                        break;
#if defined(_WIN32)
                    case '\\':
                        r.complex_uri = true;
                        state = RequestLineParser::State::Uri;
                        break;
#endif
                    case '?':
                        r.args_start = buffer_offset(b, p) + 1;
                        state = RequestLineParser::State::Uri;
                        break;
                    case '#':
                        r.complex_uri = true;
                        state = RequestLineParser::State::Uri;
                        break;
                    case '+':
                        r.plus_in_uri = true;
                        break;
                    default:
                        if (ch < 0x20 || ch == 0x7f) {
                            return ParseCode::InvalidRequest;
                        }
                        state = RequestLineParser::State::CheckUri;
                        break;
                }
                break;

            case RequestLineParser::State::CheckUri:
                if (usual[ch >> 5] & (1U << (ch & 0x1f))) {
                    break;
                }
                switch (ch) {
                    case '/':
#if defined(_WIN32)
                        if (r.uri_ext != kInvalidPos && r.uri_ext == buffer_offset(b, p)) {
                            r.complex_uri = true;
                            state = RequestLineParser::State::Uri;
                            break;
                        }
#endif
                        r.uri_ext = kInvalidPos;
                        state = RequestLineParser::State::AfterSlashInUri;
                        break;
                    case '.':
                        r.uri_ext = buffer_offset(b, p) + 1;
                        break;
                    case ' ':
                        r.uri_end = buffer_offset(b, p);
                        state = RequestLineParser::State::Http09;
                        break;
                    case '\r':
                        r.uri_end = buffer_offset(b, p);
                        r.http_minor = 9;
                        state = RequestLineParser::State::AlmostDone;
                        break;
                    case '\n':
                        r.uri_end = buffer_offset(b, p);
                        r.http_minor = 9;
                        goto done;
                    case '%':
                        r.quoted_uri = true;
                        state = RequestLineParser::State::Uri;
                        break;
                    case '?':
                        r.args_start = buffer_offset(b, p) + 1;
                        state = RequestLineParser::State::Uri;
                        break;
                    case '#':
                        r.complex_uri = true;
                        state = RequestLineParser::State::Uri;
                        break;
                    case '+':
                        r.plus_in_uri = true;
                        break;
                    case '\0':
                        return ParseCode::InvalidRequest;
                    default:
                        if (ch < 0x20 || ch == 0x7f) {
                            return ParseCode::InvalidRequest;
                        }
                        state = RequestLineParser::State::Uri;
                        break;
                }
                break;

            case RequestLineParser::State::Uri:
                if (usual[ch >> 5] & (1U << (ch & 0x1f))) {
                    break;
                }
                switch (ch) {
                    case ' ':
                        r.uri_end = buffer_offset(b, p);
                        state = RequestLineParser::State::Http09;
                        break;
                    case '\r':
                        r.uri_end = buffer_offset(b, p);
                        r.http_minor = 9;
                        state = RequestLineParser::State::AlmostDone;
                        break;
                    case '\n':
                        r.uri_end = buffer_offset(b, p);
                        r.http_minor = 9;
                        goto done;
                    case '%':
                        r.quoted_uri = true;
                        break;
                    case '#':
                        r.complex_uri = true;
                        break;
                    case '\0':
                        return ParseCode::InvalidRequest;
                    default:
                        if (ch < 0x20 || ch == 0x7f) {
                            return ParseCode::InvalidRequest;
                        }
                        break;
                }
                break;

            case RequestLineParser::State::Http09:
                switch (ch) {
                    case ' ':
                        break;
                    case '\r':
                        state = RequestLineParser::State::AlmostDone;
                        break;
                    case '\n':
                        goto done;
                    case 'H':
                        r.http_protocol_start = buffer_offset(b, p);
                        state = RequestLineParser::State::HttpH;
                        break;
                    default:
                        return ParseCode::InvalidRequest;
                }
                break;

            case RequestLineParser::State::HttpH:
                switch (ch) {
                    case 'T':
                        state = RequestLineParser::State::HttpHT;
                        break;
                    default:
                        return ParseCode::InvalidRequest;
                }
                break;

            case RequestLineParser::State::HttpHT:
                switch (ch) {
                    case 'T':
                        state = RequestLineParser::State::HttpHTT;
                        break;
                    default:
                        return ParseCode::InvalidRequest;
                }
                break;

            case RequestLineParser::State::HttpHTT:
                switch (ch) {
                    case 'P':
                        state = RequestLineParser::State::HttpHTTP;
                        break;
                    default:
                        return ParseCode::InvalidRequest;
                }
                break;

            case RequestLineParser::State::HttpHTTP:
                switch (ch) {
                    case '/':
                        state = RequestLineParser::State::FirstMajorDigit;
                        break;
                    default:
                        return ParseCode::InvalidRequest;
                }
                break;

            case RequestLineParser::State::FirstMajorDigit:
                if (ch < '1' || ch > '9') {
                    return ParseCode::InvalidVersion;
                }
                r.http_major = ch - '0';
                state = RequestLineParser::State::MajorDigit;
                break;

            case RequestLineParser::State::MajorDigit:
                if (ch == '.') {
                    state = RequestLineParser::State::FirstMinorDigit;
                    break;
                }
                if (ch < '0' || ch > '9') {
                    return ParseCode::InvalidVersion;
                }
                r.http_major = r.http_major * 10 + (ch - '0');
                break;

            case RequestLineParser::State::FirstMinorDigit:
                if (ch < '0' || ch > '9') {
                    return ParseCode::InvalidVersion;
                }
                r.http_minor = ch - '0';
                state = RequestLineParser::State::MinorDigit;
                break;

            case RequestLineParser::State::MinorDigit:
                if (ch == '\r') {
                    state = RequestLineParser::State::AlmostDone;
                    break;
                }
                if (ch == '\n') {
                    goto done;
                }
                if (ch == ' ') {
                    state = RequestLineParser::State::SpacesAfterDigit;
                    break;
                }
                if (ch < '0' || ch > '9') {
                    return ParseCode::InvalidVersion;
                }
                r.http_minor = r.http_minor * 10 + (ch - '0');
                break;

            case RequestLineParser::State::SpacesAfterDigit:
                switch (ch) {
                    case ' ':
                        break;
                    case '\r':
                        state = RequestLineParser::State::AlmostDone;
                        break;
                    case '\n':
                        goto done;
                    default:
                        return ParseCode::InvalidRequest;
                }
                break;

            case RequestLineParser::State::AlmostDone:
                r.request_end = buffer_offset(b, p) - 1;
                if (ch == '\n') {
                    goto done;
                }
                return ParseCode::InvalidRequest;
        }
    }

    b.pos = p;
    return ParseCode::Again;

done:
    b.pos = p + 1;
    if (r.request_end == kInvalidPos) {
        r.request_end = buffer_offset(b, p);
    }
    r.http_version = r.http_major * 1000 + r.http_minor;
    state = RequestLineParser::State::Start;
    if (r.http_version == 9 && r.method != HttpMethod::Get) {
        return ParseCode::Invalid09Method;
    }
    return ParseCode::Ok;
}

ParseCode parse_header_line(HeaderLineParser::HeaderLineState &r, HeaderLineParser::State &state, ParseBuffer &b,
                            bool allow_underscores) {
    unsigned char c = 0;
    unsigned char ch = 0;
    const char *p = nullptr;
    uint32_t hash = 0;
    uint32_t i = 0;

    static const unsigned char lowcase[] = "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
                                           "\0\0\0\0\0\0\0\0\0\0\0\0\0-\0\0"
                                           "0123456789\0\0\0\0\0\0"
                                           "\0abcdefghijklmnopqrstuvwxyz\0\0\0\0\0"
                                           "\0abcdefghijklmnopqrstuvwxyz\0\0\0\0\0"
                                           "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
                                           "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
                                           "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
                                           "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0";

    hash = r.header_hash;
    i = r.lowcase_index;

    for (p = b.pos; p < b.last; p++) {
        ch = static_cast<unsigned char>(*p);

        switch (state) {

            case HeaderLineParser::State::Start:
                r.header_name_start = buffer_offset(b, p);
                r.invalid_header = false;

                switch (ch) {
                    case '\r':
                        r.header_end = buffer_offset(b, p);
                        state = HeaderLineParser::State::HeaderAlmostDone;
                        break;
                    case '\n':
                        r.header_end = buffer_offset(b, p);
                        goto header_done;
                    default:
                        state = HeaderLineParser::State::Name;
                        c = lowcase[ch];
                        if (c) {
                            hash = ngx_hash(0, c);
                            r.lowcase_header[0] = static_cast<char>(c);
                            i = 1;
                            break;
                        }
                        if (ch == '_') {
                            if (allow_underscores) {
                                hash = ngx_hash(0, ch);
                                r.lowcase_header[0] = static_cast<char>(ch);
                                i = 1;
                            } else {
                                hash = 0;
                                i = 0;
                                r.invalid_header = true;
                            }
                            break;
                        }
                        if (ch <= 0x20 || ch == 0x7f || ch == ':') {
                            r.header_end = buffer_offset(b, p);
                            return ParseCode::InvalidHeader;
                        }
                        hash = 0;
                        i = 0;
                        r.invalid_header = true;
                        break;
                }
                break;

            case HeaderLineParser::State::Name:
                c = lowcase[ch];
                if (c) {
                    hash = ngx_hash(hash, c);
                    r.lowcase_header[i++] = static_cast<char>(c);
                    i &= (sizeof(r.lowcase_header) - 1);
                    break;
                }
                if (ch == '_') {
                    if (allow_underscores) {
                        hash = ngx_hash(hash, ch);
                        r.lowcase_header[i++] = static_cast<char>(ch);
                        i &= (sizeof(r.lowcase_header) - 1);
                    } else {
                        r.invalid_header = true;
                    }
                    break;
                }
                if (ch == ':') {
                    r.header_name_end = buffer_offset(b, p);
                    state = HeaderLineParser::State::SpaceBeforeValue;
                    break;
                }
                if (ch == '\r') {
                    r.header_name_end = buffer_offset(b, p);
                    r.header_start = buffer_offset(b, p);
                    r.header_end = buffer_offset(b, p);
                    state = HeaderLineParser::State::AlmostDone;
                    break;
                }
                if (ch == '\n') {
                    r.header_name_end = buffer_offset(b, p);
                    r.header_start = buffer_offset(b, p);
                    r.header_end = buffer_offset(b, p);
                    goto done;
                }
                if (ch <= 0x20 || ch == 0x7f) {
                    r.header_end = buffer_offset(b, p);
                    return ParseCode::InvalidHeader;
                }
                r.invalid_header = true;
                break;

            case HeaderLineParser::State::SpaceBeforeValue:
                switch (ch) {
                    case ' ':
                        break;
                    case '\r':
                        r.header_start = buffer_offset(b, p);
                        r.header_end = buffer_offset(b, p);
                        state = HeaderLineParser::State::AlmostDone;
                        break;
                    case '\n':
                        r.header_start = buffer_offset(b, p);
                        r.header_end = buffer_offset(b, p);
                        goto done;
                    case '\0':
                        r.header_end = buffer_offset(b, p);
                        return ParseCode::InvalidHeader;
                    default:
                        r.header_start = buffer_offset(b, p);
                        state = HeaderLineParser::State::Value;
                        break;
                }
                break;

            case HeaderLineParser::State::Value:
                switch (ch) {
                    case ' ':
                        r.header_end = buffer_offset(b, p);
                        state = HeaderLineParser::State::SpaceAfterValue;
                        break;
                    case '\r':
                        r.header_end = buffer_offset(b, p);
                        state = HeaderLineParser::State::AlmostDone;
                        break;
                    case '\n':
                        r.header_end = buffer_offset(b, p);
                        goto done;
                    case '\0':
                        r.header_end = buffer_offset(b, p);
                        return ParseCode::InvalidHeader;
                }
                break;

            case HeaderLineParser::State::SpaceAfterValue:
                switch (ch) {
                    case ' ':
                        break;
                    case '\r':
                        state = HeaderLineParser::State::AlmostDone;
                        break;
                    case '\n':
                        goto done;
                    case '\0':
                        r.header_end = buffer_offset(b, p);
                        return ParseCode::InvalidHeader;
                    default:
                        state = HeaderLineParser::State::Value;
                        break;
                }
                break;

            case HeaderLineParser::State::IgnoreLine:
                switch (ch) {
                    case '\n':
                        state = HeaderLineParser::State::Start;
                        break;
                    default:
                        break;
                }
                break;

            case HeaderLineParser::State::AlmostDone:
                switch (ch) {
                    case '\n':
                        goto done;
                    case '\r':
                        break;
                    default:
                        return ParseCode::InvalidHeader;
                }
                break;

            case HeaderLineParser::State::HeaderAlmostDone:
                switch (ch) {
                    case '\n':
                        goto header_done;
                    default:
                        return ParseCode::InvalidHeader;
                }
        }
    }

    b.pos = p;
    r.header_hash = hash;
    r.lowcase_index = i;
    return ParseCode::Again;

done:
    b.pos = p + 1;
    state = HeaderLineParser::State::Start;
    r.header_hash = hash;
    r.lowcase_index = i;
    return ParseCode::Ok;

header_done:
    b.pos = p + 1;
    state = HeaderLineParser::State::Start;
    return ParseCode::HeaderDone;
}

ParseCode parse_chunked(BodyParser::ChunkedState &ctx, ParseBuffer &b) {
    unsigned char ch = 0;
    unsigned char c = 0;
    const char *pos = nullptr;
    ParseCode rc = ParseCode::Again;
    enum {
        sw_chunk_start = 0,
        sw_chunk_size,
        sw_chunk_extension,
        sw_chunk_extension_almost_done,
        sw_chunk_data,
        sw_after_data,
        sw_after_data_almost_done,
        sw_last_chunk_extension,
        sw_last_chunk_extension_almost_done,
        sw_trailer,
        sw_trailer_almost_done,
        sw_trailer_header,
        sw_trailer_header_almost_done
    } state;

    state = static_cast<decltype(state)>(ctx.state);

    if (state == sw_chunk_data && ctx.size == 0) {
        state = sw_after_data;
    }

    for (pos = b.pos; pos < b.last; pos++) {
        ch = static_cast<unsigned char>(*pos);
        switch (state) {
            case sw_chunk_start:
                if (ch >= '0' && ch <= '9') {
                    state = sw_chunk_size;
                    ctx.size = ch - '0';
                    break;
                }
                c = static_cast<unsigned char>(ch | 0x20u);
                if (c >= 'a' && c <= 'f') {
                    state = sw_chunk_size;
                    ctx.size = c - 'a' + 10;
                    break;
                }
                goto invalid;

            case sw_chunk_size:
                if (ctx.size > kMaxOffT / 16) {
                    goto invalid;
                }
                if (ch >= '0' && ch <= '9') {
                    ctx.size = ctx.size * 16 + (ch - '0');
                    break;
                }
                c = static_cast<unsigned char>(ch | 0x20u);
                if (c >= 'a' && c <= 'f') {
                    ctx.size = ctx.size * 16 + (c - 'a' + 10);
                    break;
                }
                if (ctx.size == 0) {
                    switch (ch) {
                        case '\r':
                            state = sw_last_chunk_extension_almost_done;
                            break;
                        case '\n':
                            state = sw_trailer;
                            break;
                        case ';':
                        case ' ':
                        case '\t':
                            state = sw_last_chunk_extension;
                            break;
                        default:
                            goto invalid;
                    }
                    break;
                }
                switch (ch) {
                    case '\r':
                        state = sw_chunk_extension_almost_done;
                        break;
                    case '\n':
                        state = sw_chunk_data;
                        break;
                    case ';':
                    case ' ':
                    case '\t':
                        state = sw_chunk_extension;
                        break;
                    default:
                        goto invalid;
                }
                break;

            case sw_chunk_extension:
                switch (ch) {
                    case '\r':
                        state = sw_chunk_extension_almost_done;
                        break;
                    case '\n':
                        state = sw_chunk_data;
                }
                break;

            case sw_chunk_extension_almost_done:
                if (ch == '\n') {
                    state = sw_chunk_data;
                    break;
                }
                goto invalid;

            case sw_chunk_data:
                rc = ParseCode::Ok;
                goto data;

            case sw_after_data:
                switch (ch) {
                    case '\r':
                        state = sw_after_data_almost_done;
                        break;
                    case '\n':
                        state = sw_chunk_start;
                        break;
                    default:
                        goto invalid;
                }
                break;

            case sw_after_data_almost_done:
                if (ch == '\n') {
                    state = sw_chunk_start;
                    break;
                }
                goto invalid;

            case sw_last_chunk_extension:
                switch (ch) {
                    case '\r':
                        state = sw_last_chunk_extension_almost_done;
                        break;
                    case '\n':
                        state = sw_trailer;
                }
                break;

            case sw_last_chunk_extension_almost_done:
                if (ch == '\n') {
                    state = sw_trailer;
                    break;
                }
                goto invalid;

            case sw_trailer:
                switch (ch) {
                    case '\r':
                        state = sw_trailer_almost_done;
                        break;
                    case '\n':
                        goto done;
                    default:
                        state = sw_trailer_header;
                }
                break;

            case sw_trailer_almost_done:
                if (ch == '\n') {
                    goto done;
                }
                goto invalid;

            case sw_trailer_header:
                switch (ch) {
                    case '\r':
                        state = sw_trailer_header_almost_done;
                        break;
                    case '\n':
                        state = sw_trailer;
                }
                break;

            case sw_trailer_header_almost_done:
                if (ch == '\n') {
                    state = sw_trailer;
                    break;
                }
                goto invalid;
        }
    }

data:
    ctx.state = static_cast<int>(state);
    b.pos = pos;

    if (ctx.size > kMaxOffT - 5) {
        goto invalid;
    }

    switch (state) {
        case sw_chunk_start:
            ctx.length = 3;
            break;
        case sw_chunk_size:
            ctx.length = 1 + (ctx.size ? ctx.size + 4 : 1);
            break;
        case sw_chunk_extension:
        case sw_chunk_extension_almost_done:
            ctx.length = 1 + ctx.size + 4;
            break;
        case sw_chunk_data:
            ctx.length = ctx.size + 4;
            break;
        case sw_after_data:
        case sw_after_data_almost_done:
            ctx.length = 4;
            break;
        case sw_last_chunk_extension:
        case sw_last_chunk_extension_almost_done:
            ctx.length = 2;
            break;
        case sw_trailer:
        case sw_trailer_almost_done:
            ctx.length = 1;
            break;
        case sw_trailer_header:
        case sw_trailer_header_almost_done:
            ctx.length = 2;
            break;
    }

    return rc;

done:
    ctx.state = 0;
    b.pos = pos + 1;
    return ParseCode::Done;

invalid:
    return ParseCode::Error;
}

} // namespace

RequestLineParser::RequestLineParser(HttpExchange &exchange, const HttpServerOptions &options) :
    exchange_(&exchange), options_(&options) {}

void RequestLineParser::reset() {
    state_ = State::Start;
    line_ = RequestLineState{};
}

bool RequestLineParser::carry_over(const char *data, size_t size, size_t pos, char *dst, size_t dst_cap,
                                   size_t &dst_size, size_t &dst_pos) {
    if (state_ == State::Start || line_.request_start == kInvalidPos) {
        dst_size = 0;
        dst_pos = 0;
        return true;
    }
    if (line_.request_start > size) {
        return false;
    }
    size_t copy_len = size - line_.request_start;
    if (copy_len > dst_cap) {
        return false;
    }
    std::memcpy(dst, data + line_.request_start, copy_len);
    dst_size = copy_len;
    dst_pos = (pos >= line_.request_start) ? (pos - line_.request_start) : 0;

    size_t shift = line_.request_start;
    auto adjust = [&](size_t &value) {
        if (value != kInvalidPos) {
            value -= shift;
        }
    };

    adjust(line_.request_start);
    adjust(line_.method_end);
    adjust(line_.uri_start);
    adjust(line_.uri_end);
    adjust(line_.schema_start);
    adjust(line_.schema_end);
    adjust(line_.host_start);
    adjust(line_.host_end);
    adjust(line_.port_start);
    adjust(line_.port_end);
    adjust(line_.args_start);
    adjust(line_.request_end);
    adjust(line_.uri_ext);
    adjust(line_.http_protocol_start);
    return true;
}

ParseCode RequestLineParser::execute(ParseBuffer *buffer) {
    if (!exchange_ || !buffer || !buffer->start || buffer->pos >= buffer->last) {
        return ParseCode::Again;
    }

    ParseCode rc = parse_request_line(line_, state_, *buffer);
    if (rc != ParseCode::Ok) {
        return rc;
    }

    if (line_.request_start == kInvalidPos || line_.method_end == kInvalidPos || line_.uri_start == kInvalidPos ||
        line_.uri_end == kInvalidPos) {
        return ParseCode::InvalidRequest;
    }

    const char *base = buffer->start;
    size_t method_len = line_.method_end - line_.request_start + 1;
    size_t uri_len = line_.uri_end - line_.uri_start;
    exchange_->method_view_ = std::string_view(base + line_.request_start, method_len);
    exchange_->method_ = line_.method;
    exchange_->uri_.unparsed_uri = std::string_view(base + line_.uri_start, uri_len);

    if (line_.http_major < 0 || line_.http_minor < 0) {
        return ParseCode::InvalidRequest;
    }

    exchange_->version_ = static_cast<HttpVersion>(line_.http_version);
    exchange_->version_view_ = {};

    if (line_.http_protocol_start != kInvalidPos && line_.request_end != kInvalidPos &&
        line_.request_end >= line_.http_protocol_start) {
        size_t version_len = line_.request_end - line_.http_protocol_start + 1;
        exchange_->version_view_ = std::string_view(base + line_.http_protocol_start, version_len);
    } else if (line_.http_version == 9) {
        exchange_->version_view_ = "HTTP/0.9";
    }

    size_t path_end = line_.uri_end;
    if (line_.args_start != kInvalidPos && line_.args_start <= line_.uri_end) {
        if (line_.args_start > line_.uri_start) {
            path_end = line_.args_start - 1;
        } else {
            path_end = line_.uri_start;
        }
        exchange_->uri_.query = std::string_view(base + line_.args_start, line_.uri_end - line_.args_start);
    } else {
        exchange_->uri_.query = {};
    }
    exchange_->uri_.path = std::string_view(base + line_.uri_start, path_end - line_.uri_start);
    if (line_.uri_ext != kInvalidPos && line_.uri_ext >= line_.uri_start && line_.uri_ext < path_end) {
        exchange_->uri_.exten = std::string_view(base + line_.uri_ext, path_end - line_.uri_ext);
    } else {
        exchange_->uri_.exten = {};
    }

    return rc;
}

HeaderLineParser::HeaderLineParser(HttpExchange &exchange, const HttpServerOptions &options) :
    exchange_(&exchange), options_(&options) {}

void HeaderLineParser::reset() {
    state_ = State::Start;
    line_ = HeaderLineState{};
    connection_close_ = false;
    connection_keep_alive_ = false;
    last_error_ = HttpParseError::None;
}

bool HeaderLineParser::carry_over(const char *data, size_t size, size_t pos, char *dst, size_t dst_cap,
                                  size_t &dst_size, size_t &dst_pos) {
    if (state_ == State::Start || line_.header_name_start == kInvalidPos) {
        dst_size = 0;
        dst_pos = 0;
        return true;
    }
    if (line_.header_name_start > size) {
        return false;
    }
    size_t copy_len = size - line_.header_name_start;
    if (copy_len > dst_cap) {
        return false;
    }
    std::memcpy(dst, data + line_.header_name_start, copy_len);
    dst_size = copy_len;
    dst_pos = (pos >= line_.header_name_start) ? (pos - line_.header_name_start) : 0;

    size_t shift = line_.header_name_start;
    auto adjust = [&](size_t &value) {
        if (value != kInvalidPos) {
            value -= shift;
        }
    };

    adjust(line_.header_name_start);
    adjust(line_.header_name_end);
    adjust(line_.header_start);
    adjust(line_.header_end);
    return true;
}

ParseCode HeaderLineParser::execute(ParseBuffer *buffer) {
    if (!exchange_ || !buffer || !buffer->start || buffer->pos >= buffer->last) {
        return ParseCode::Again;
    }

    for (;;) {
        ParseCode rc = parse_header_line(line_, state_, *buffer, true);

        if (rc == ParseCode::Ok) {
            if (line_.header_name_start != kInvalidPos && line_.header_name_end != kInvalidPos &&
                line_.header_name_end >= line_.header_name_start) {
                const char *base = buffer->start;
                size_t name_len = line_.header_name_end - line_.header_name_start;
                std::string_view name(base + line_.header_name_start, name_len);
                std::string_view value;
                if (line_.header_start != kInvalidPos && line_.header_end != kInvalidPos &&
                    line_.header_end >= line_.header_start) {
                    size_t value_len = line_.header_end - line_.header_start;
                    value = std::string_view(base + line_.header_start, value_len);
                } else {
                    value = std::string_view();
                }
                if (!exchange_->request_headers_.add(name, value)) {
                    last_error_ = HttpParseError::HeadersTooLarge;
                    return ParseCode::Error;
                }

                const bool lc_valid = name_len <= sizeof(line_.lowcase_header);
                const uint32_t name_hash = line_.header_hash;
                const char *lc = line_.lowcase_header;

                if (lc_valid && match_content_length(name_hash, name_len, lc)) {
                    size_t length = 0;
                    auto res = std::from_chars(value.data(), value.data() + value.size(), length);
                    if (res.ec != std::errc() || res.ptr != value.data() + value.size()) {
                        last_error_ = HttpParseError::BadRequest;
                        return ParseCode::Error;
                    }
                } else if (lc_valid && match_transfer_encoding(name_hash, name_len, lc)) {
                    bool chunked = false;
                    if (!parse_transfer_encoding(value, chunked)) {
                        last_error_ = HttpParseError::UnsupportedTransferEncoding;
                        return ParseCode::Error;
                    }
                } else if (lc_valid && match_expect(name_hash, name_len, lc)) {
                } else if (lc_valid && match_connection(name_hash, name_len, lc)) {
                    if (has_token(value, "close")) {
                        connection_close_ = true;
                    }
                    if (has_token(value, "keep-alive")) {
                        connection_keep_alive_ = true;
                    }
                }
            }
            continue;
        }

        if (rc == ParseCode::HeaderDone) {
            return rc;
        }

        if (rc == ParseCode::Again) {
            return rc;
        }

        last_error_ = HttpParseError::BadRequest;
        return rc;
    }
}

BodyParser::BodyParser() = default;

void BodyParser::reset() {
    state_ = State::Init;
    parse_buffer_.clear();
    parse_offset_ = 0;
    body_bytes_ = 0;
    chunked_state_ = ChunkedState{};
}

HttpParseResult BodyParser::execute(HttpExchange &exchange, const HttpServerOptions &options, const char *data,
                                    size_t len) {
    HttpParseResult result{};
    if (!data || len == 0) {
        result.state = HttpParseState::NeedMore;
        return result;
    }

    parse_buffer_.append(data, len);
    result.consumed = len;

    auto fail = [&](HttpParseError error) {
        result.state = HttpParseState::Error;
        result.error = error;
        return result;
    };

    auto compact_buffer = [&]() {
        if (parse_offset_ == 0) {
            return;
        }
        parse_buffer_.erase(0, parse_offset_);
        parse_offset_ = 0;
    };

    if (state_ == State::Init) {
    }

    if (state_ == State::ContentLength) {

        size_t available = parse_buffer_.size() - parse_offset_;
        if (available == 0) {
            result.state = HttpParseState::NeedMore;
            return result;
        }


        result.state = HttpParseState::NeedMore;
        return result;
    }

    if (state_ == State::Chunked) {
        for (;;) {
            if (chunked_state_.state == kChunkDataState && chunked_state_.size > 0) {
                size_t available = parse_buffer_.size() - parse_offset_;
                if (available == 0) {
                    result.state = HttpParseState::NeedMore;
                    return result;
                }
                size_t to_copy = std::min(chunked_state_.size, available);
                if (body_bytes_ + to_copy > options.max_body_bytes) {
                    return fail(HttpParseError::BodyTooLarge);
                }
                exchange.body_buffer_.append(parse_buffer_.data() + parse_offset_, to_copy);
                parse_offset_ += to_copy;
                body_bytes_ += to_copy;
                chunked_state_.size -= to_copy;
                compact_buffer();
                if (chunked_state_.size > 0) {
                    result.state = HttpParseState::NeedMore;
                    return result;
                }
                continue;
            }

            ParseBuffer buffer{parse_buffer_.data(), parse_buffer_.data() + parse_offset_,
                               parse_buffer_.data() + parse_buffer_.size()};
            ParseCode rc = parse_chunked(chunked_state_, buffer);
            parse_offset_ = static_cast<size_t>(buffer.pos - buffer.start);
            compact_buffer();

            if (rc == ParseCode::Ok) {
                if (chunked_state_.size > options.max_chunk_bytes) {
                    return fail(HttpParseError::ChunkTooLarge);
                }
                continue;
            }

            if (rc == ParseCode::Again) {
                result.state = HttpParseState::NeedMore;
                return result;
            }

            if (rc == ParseCode::Done) {
                state_ = State::Done;
                result.state = HttpParseState::MessageComplete;
                return result;
            }

            return fail(HttpParseError::BadRequest);
        }
    }

    result.state = HttpParseState::MessageComplete;
    return result;
}

} // namespace fiber::http
