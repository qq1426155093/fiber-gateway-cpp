#include "Http2ExchangeIo.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "HttpExchange.h"

namespace fiber::http {

namespace {

bool equals_ascii_ci(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        unsigned char ca = static_cast<unsigned char>(a[i]);
        unsigned char cb = static_cast<unsigned char>(b[i]);
        if (ca >= 'A' && ca <= 'Z') {
            ca = static_cast<unsigned char>(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = static_cast<unsigned char>(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

std::string to_lower_ascii(std::string_view in) {
    std::string out;
    out.resize(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        unsigned char ch = static_cast<unsigned char>(in[i]);
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<unsigned char>(ch - 'A' + 'a');
        }
        out[i] = static_cast<char>(ch);
    }
    return out;
}

bool is_h2_forbidden_header(std::string_view lower_name) {
    return lower_name == "connection" ||
           lower_name == "proxy-connection" ||
           lower_name == "keep-alive" ||
           lower_name == "transfer-encoding" ||
           lower_name == "upgrade";
}

HttpMethod parse_method(std::string_view method) {
    if (equals_ascii_ci(method, "GET")) {
        return HttpMethod::Get;
    }
    if (equals_ascii_ci(method, "HEAD")) {
        return HttpMethod::Head;
    }
    if (equals_ascii_ci(method, "POST")) {
        return HttpMethod::Post;
    }
    if (equals_ascii_ci(method, "PUT")) {
        return HttpMethod::Put;
    }
    if (equals_ascii_ci(method, "DELETE")) {
        return HttpMethod::Delete;
    }
    if (equals_ascii_ci(method, "OPTIONS")) {
        return HttpMethod::Options;
    }
    if (equals_ascii_ci(method, "PATCH")) {
        return HttpMethod::Patch;
    }
    if (equals_ascii_ci(method, "CONNECT")) {
        return HttpMethod::Connect;
    }
    if (equals_ascii_ci(method, "TRACE")) {
        return HttpMethod::Trace;
    }
    if (equals_ascii_ci(method, "COPY")) {
        return HttpMethod::Copy;
    }
    if (equals_ascii_ci(method, "MOVE")) {
        return HttpMethod::Move;
    }
    if (equals_ascii_ci(method, "LOCK")) {
        return HttpMethod::Lock;
    }
    if (equals_ascii_ci(method, "UNLOCK")) {
        return HttpMethod::Unlock;
    }
    if (equals_ascii_ci(method, "MKCOL")) {
        return HttpMethod::MKCOL;
    }
    if (equals_ascii_ci(method, "PROPFIND")) {
        return HttpMethod::PropFind;
    }
    if (equals_ascii_ci(method, "PROPPATCH")) {
        return HttpMethod::PropPatch;
    }
    return HttpMethod::Unknown;
}

common::IoResult<std::pair<bool, size_t>> parse_content_length(const HttpHeaders &headers) {
    bool seen = false;
    size_t value = 0;
    auto range = headers.get_all("content-length");
    for (const auto &field : range) {
        std::string_view text = field.value_view();
        unsigned long long parsed = 0;
        auto result = std::from_chars(text.data(), text.data() + text.size(), parsed, 10);
        if (result.ec != std::errc() || result.ptr != text.data() + text.size()) {
            return std::unexpected(common::IoErr::Invalid);
        }
        size_t parsed_size = static_cast<size_t>(parsed);
        if (!seen) {
            value = parsed_size;
            seen = true;
        } else if (value != parsed_size) {
            return std::unexpected(common::IoErr::Invalid);
        }
    }
    return std::make_pair(seen, value);
}

} // namespace

Http2ExchangeIo::Http2ExchangeIo(nghttp2_session *session, int32_t stream_id, FlushCallback on_flush)
    : session_(session), stream_id_(stream_id), on_flush_(std::move(on_flush)) {
    data_provider_.source.ptr = this;
    data_provider_.read_callback = &Http2ExchangeIo::data_source_read_callback;
}

common::IoResult<void> Http2ExchangeIo::on_request_header(HttpExchange &exchange,
                                                          std::string_view name,
                                                          std::string_view value) {
    if (name.empty()) {
        return std::unexpected(common::IoErr::Invalid);
    }

    if (!name.empty() && name.front() == ':') {
        if (name == ":method") {
            if (!method_text_.empty()) {
                return std::unexpected(common::IoErr::Invalid);
            }
            method_text_.assign(value.data(), value.size());
            return {};
        }
        if (name == ":path") {
            if (!path_text_.empty()) {
                return std::unexpected(common::IoErr::Invalid);
            }
            path_text_.assign(value.data(), value.size());
            return {};
        }
        if (name == ":scheme") {
            if (!scheme_text_.empty()) {
                return std::unexpected(common::IoErr::Invalid);
            }
            scheme_text_.assign(value.data(), value.size());
            return {};
        }
        if (name == ":authority") {
            if (!authority_text_.empty()) {
                return std::unexpected(common::IoErr::Invalid);
            }
            authority_text_.assign(value.data(), value.size());
            return {};
        }
        return std::unexpected(common::IoErr::Invalid);
    }

    std::string lower_name = to_lower_ascii(name);
    if (is_h2_forbidden_header(lower_name)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (!exchange.request_headers_.add(name, value)) {
        return std::unexpected(common::IoErr::NoMem);
    }
    return {};
}

common::IoResult<void> Http2ExchangeIo::on_request_headers_complete(HttpExchange &exchange) {
    if (method_text_.empty()) {
        return std::unexpected(common::IoErr::Invalid);
    }

    if (path_text_.empty()) {
        if (equals_ascii_ci(method_text_, "CONNECT")) {
            path_text_ = authority_text_;
        } else {
            path_text_ = "/";
        }
    }

    exchange.version_ = HttpVersion::HTTP_2_0;
    exchange.version_view_ = version_text_;
    exchange.method_ = parse_method(method_text_);
    exchange.method_view_ = method_text_;

    exchange.uri_ = HttpUri{};
    exchange.uri_.unparsed_uri = path_text_;
    size_t query_pos = path_text_.find('?');
    if (query_pos == std::string_view::npos) {
        exchange.uri_.path = path_text_;
    } else {
        exchange.uri_.path = std::string_view(path_text_.data(), query_pos);
        if (query_pos + 1 < path_text_.size()) {
            exchange.uri_.query = std::string_view(path_text_.data() + query_pos + 1, path_text_.size() - query_pos - 1);
        }
    }
    std::string_view path_only = exchange.uri_.path;
    size_t dot_pos = path_only.rfind('.');
    if (dot_pos != std::string_view::npos && dot_pos + 1 < path_only.size()) {
        size_t slash_pos = path_only.rfind('/');
        if (slash_pos == std::string_view::npos || dot_pos > slash_pos) {
            exchange.uri_.exten = path_only.substr(dot_pos + 1);
        }
    }

    if (!authority_text_.empty() && !exchange.request_headers_.contains("host")) {
        if (!exchange.request_headers_.add("host", authority_text_)) {
            return std::unexpected(common::IoErr::NoMem);
        }
    }

    auto content_length = parse_content_length(exchange.request_headers_);
    if (!content_length) {
        return std::unexpected(content_length.error());
    }
    exchange.request_chunked_ = false;
    exchange.request_content_length_set_ = content_length->first;
    exchange.request_content_length_ = content_length->second;
    exchange.request_close_ = false;
    exchange.request_keep_alive_ = true;
    exchange.response_chunked_ = false;
    exchange.response_close_ = false;
    exchange.response_content_length_set_ = false;
    exchange.response_content_length_ = 0;
    return {};
}

void Http2ExchangeIo::append_request_body(std::string_view data) {
    if (!data.empty()) {
        request_body_.append(data.data(), data.size());
    }
}

void Http2ExchangeIo::mark_request_body_end() {
    request_body_end_ = true;
}

fiber::async::Task<common::IoResult<ReadBodyResult>> Http2ExchangeIo::read_body(HttpExchange &,
                                                                                 void *buf,
                                                                                 size_t len) noexcept {
    ReadBodyResult out{};
    if (!buf && len > 0) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (request_body_read_ < request_body_.size() && len > 0) {
        size_t available = request_body_.size() - request_body_read_;
        size_t take = std::min(available, len);
        std::memcpy(buf, request_body_.data() + request_body_read_, take);
        request_body_read_ += take;
        out.size = take;
    }
    if (request_body_read_ >= request_body_.size()) {
        if (request_body_read_ > 0) {
            request_body_.clear();
            request_body_read_ = 0;
        }
        if (request_body_end_) {
            out.end = true;
        }
    }
    if (out.size == 0 && !out.end) {
        co_return std::unexpected(common::IoErr::WouldBlock);
    }
    co_return out;
}

fiber::async::Task<common::IoResult<void>> Http2ExchangeIo::send_response_header(HttpExchange &exchange,
                                                                                  int status,
                                                                                  std::string_view reason) {
    (void) reason;
    if (!session_ || stream_id_ <= 0) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (response_header_sent_) {
        co_return std::unexpected(common::IoErr::Already);
    }
    if (status < 100 || status > 999) {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    std::vector<std::pair<std::string, std::string>> fields;
    fields.reserve(exchange.response_headers_.size() + 2);
    fields.emplace_back(":status", std::to_string(status));

    bool has_content_length = false;
    for (auto it = exchange.response_headers_.begin(); it != exchange.response_headers_.end(); ++it) {
        const auto &field = *it;
        if (field.name_len == 0) {
            continue;
        }
        std::string name = to_lower_ascii(field.name_view());
        if (name.empty() || name.front() == ':') {
            continue;
        }
        if (is_h2_forbidden_header(name)) {
            continue;
        }
        if (name == "content-length") {
            has_content_length = true;
        }
        fields.emplace_back(std::move(name), std::string(field.value_view()));
    }

    if (exchange.response_content_length_set_ && !has_content_length) {
        fields.emplace_back("content-length", std::to_string(exchange.response_content_length_));
    }

    bool end_immediately =
        exchange.response_content_length_set_ &&
        exchange.response_content_length_ == 0 &&
        !exchange.response_chunked_;

    std::vector<nghttp2_nv> nva;
    nva.reserve(fields.size());
    for (auto &field : fields) {
        nghttp2_nv nv{};
        nv.name = reinterpret_cast<uint8_t *>(field.first.data());
        nv.namelen = field.first.size();
        nv.value = reinterpret_cast<uint8_t *>(field.second.data());
        nv.valuelen = field.second.size();
        nv.flags = NGHTTP2_NV_FLAG_NONE;
        nva.push_back(nv);
    }

    nghttp2_data_provider2 *provider = end_immediately ? nullptr : &data_provider_;
    int rc = nghttp2_submit_response2(session_, stream_id_, nva.data(), nva.size(), provider);
    if (rc != 0) {
        co_return std::unexpected(map_nghttp2_error(rc));
    }

    response_header_sent_ = true;
    if (end_immediately) {
        response_end_stream_ = true;
        response_complete_ = true;
    }
    if (on_flush_) {
        on_flush_();
    }
    co_return common::IoResult<void>{};
}

fiber::async::Task<common::IoResult<size_t>> Http2ExchangeIo::write_body(HttpExchange &exchange,
                                                                          const uint8_t *buf,
                                                                          size_t len,
                                                                          bool end) noexcept {
    if (len > 0 && buf == nullptr) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (!session_ || stream_id_ <= 0) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (!response_header_sent_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (response_complete_) {
        co_return std::unexpected(common::IoErr::Already);
    }

    if (exchange.response_content_length_set_) {
        size_t remaining = exchange.response_content_length_ - response_body_sent_;
        if (len > remaining) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
    }

    if (len > 0) {
        response_chunks_.emplace_back(reinterpret_cast<const char *>(buf), len);
        response_body_sent_ += len;
    }
    if (end) {
        if (exchange.response_content_length_set_ && response_body_sent_ != exchange.response_content_length_) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
        response_end_stream_ = true;
        response_complete_ = true;
    }

    int rc = nghttp2_session_resume_data(session_, stream_id_);
    if (rc != 0) {
        co_return std::unexpected(map_nghttp2_error(rc));
    }
    response_data_deferred_ = false;
    if (on_flush_) {
        on_flush_();
    }
    co_return len;
}

nghttp2_ssize Http2ExchangeIo::data_source_read_callback(nghttp2_session *,
                                                         int32_t,
                                                         uint8_t *buf,
                                                         size_t length,
                                                         uint32_t *data_flags,
                                                         nghttp2_data_source *source,
                                                         void *) {
    if (!source || !source->ptr) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    auto *self = static_cast<Http2ExchangeIo *>(source->ptr);
    return self->read_response_data(buf, length, data_flags);
}

nghttp2_ssize Http2ExchangeIo::read_response_data(uint8_t *buf, size_t length, uint32_t *data_flags) {
    if (!buf || !data_flags) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }

    size_t written = 0;
    while (written < length && !response_chunks_.empty()) {
        std::string &chunk = response_chunks_.front();
        if (response_chunk_offset_ >= chunk.size()) {
            response_chunks_.pop_front();
            response_chunk_offset_ = 0;
            continue;
        }
        size_t available = chunk.size() - response_chunk_offset_;
        size_t take = std::min(length - written, available);
        std::memcpy(buf + written, chunk.data() + response_chunk_offset_, take);
        response_chunk_offset_ += take;
        written += take;
        if (response_chunk_offset_ >= chunk.size()) {
            response_chunks_.pop_front();
            response_chunk_offset_ = 0;
        }
    }

    if (written == 0) {
        if (response_end_stream_) {
            *data_flags |= NGHTTP2_DATA_FLAG_EOF;
            return 0;
        }
        response_data_deferred_ = true;
        return NGHTTP2_ERR_DEFERRED;
    }

    if (response_chunks_.empty() && response_end_stream_) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    }
    return static_cast<nghttp2_ssize>(written);
}

common::IoErr Http2ExchangeIo::map_nghttp2_error(int rc) noexcept {
    switch (rc) {
        case 0:
            return common::IoErr::None;
        case NGHTTP2_ERR_NOMEM:
            return common::IoErr::NoMem;
        case NGHTTP2_ERR_INVALID_ARGUMENT:
        case NGHTTP2_ERR_PROTO:
            return common::IoErr::Invalid;
        case NGHTTP2_ERR_WOULDBLOCK:
            return common::IoErr::WouldBlock;
        case NGHTTP2_ERR_STREAM_CLOSED:
            return common::IoErr::ConnReset;
        case NGHTTP2_ERR_DATA_EXIST:
            return common::IoErr::Already;
        default:
            return common::IoErr::Unknown;
    }
}

} // namespace fiber::http
