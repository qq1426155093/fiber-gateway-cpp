#include "TlsContext.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

#include <openssl/ssl.h>

namespace fiber::http {

TlsContext::TlsContext(TlsOptions options, bool is_server)
    : options_(std::move(options)), is_server_(is_server) {
    alpn_ = options_.alpn;
    alpn_.erase(std::remove_if(alpn_.begin(),
                               alpn_.end(),
                               [](const std::string &proto) { return proto.empty(); }),
                alpn_.end());
}

TlsContext::~TlsContext() {
    if (ctx_) {
        SSL_CTX_free(ctx_);
        ctx_ = nullptr;
    }
}

common::IoResult<void> TlsContext::init() {
    if (ctx_) {
        return {};
    }
    if (is_server_) {
        if (options_.cert_file.empty() || options_.key_file.empty()) {
            return std::unexpected(common::IoErr::Invalid);
        }
    }

    SSL_CTX *ctx = SSL_CTX_new(is_server_ ? TLS_server_method() : TLS_client_method());
    if (!ctx) {
        return std::unexpected(common::IoErr::NoMem);
    }

    if (options_.min_version > 0) {
        SSL_CTX_set_min_proto_version(ctx, options_.min_version);
    }
    if (options_.max_version > 0) {
        SSL_CTX_set_max_proto_version(ctx, options_.max_version);
    }

    if (is_server_) {
        if (SSL_CTX_use_certificate_file(ctx, options_.cert_file.c_str(), SSL_FILETYPE_PEM) != 1) {
            SSL_CTX_free(ctx);
            return std::unexpected(common::IoErr::Invalid);
        }
        if (SSL_CTX_use_PrivateKey_file(ctx, options_.key_file.c_str(), SSL_FILETYPE_PEM) != 1) {
            SSL_CTX_free(ctx);
            return std::unexpected(common::IoErr::Invalid);
        }
        if (SSL_CTX_check_private_key(ctx) != 1) {
            SSL_CTX_free(ctx);
            return std::unexpected(common::IoErr::Invalid);
        }

        if (options_.verify_client) {
            if (options_.ca_file.empty()) {
                SSL_CTX_free(ctx);
                return std::unexpected(common::IoErr::Invalid);
            }
            if (SSL_CTX_load_verify_locations(ctx, options_.ca_file.c_str(), nullptr) != 1) {
                SSL_CTX_free(ctx);
                return std::unexpected(common::IoErr::Invalid);
            }
            SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
        }
    } else {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
    }

    if (!alpn_.empty()) {
        if (is_server_) {
            SSL_CTX_set_alpn_select_cb(ctx, &TlsContext::alpn_select_cb, this);
        } else {
            alpn_wire_.clear();
            for (const auto &proto : alpn_) {
                if (proto.size() > 255) {
                    SSL_CTX_free(ctx);
                    return std::unexpected(common::IoErr::Invalid);
                }
                alpn_wire_.push_back(static_cast<unsigned char>(proto.size()));
                alpn_wire_.insert(alpn_wire_.end(), proto.begin(), proto.end());
            }
            if (SSL_CTX_set_alpn_protos(ctx, alpn_wire_.data(),
                                        static_cast<unsigned int>(alpn_wire_.size())) != 0) {
                SSL_CTX_free(ctx);
                return std::unexpected(common::IoErr::Invalid);
            }
        }
    }

    ctx_ = ctx;
    return {};
}

int TlsContext::alpn_select_cb(SSL *,
                               const unsigned char **out,
                               unsigned char *outlen,
                               const unsigned char *in,
                               unsigned int inlen,
                               void *arg) {
    auto *self = static_cast<TlsContext *>(arg);
    if (!self || self->alpn().empty() || !in || inlen == 0) {
        return SSL_TLSEXT_ERR_NOACK;
    }

    for (const auto &proto : self->alpn()) {
        const unsigned char *ptr = in;
        unsigned int remaining = inlen;
        while (remaining > 0) {
            unsigned int len = ptr[0];
            if (len + 1 > remaining) {
                break;
            }
            if (len == proto.size() &&
                std::memcmp(ptr + 1, proto.data(), proto.size()) == 0) {
                *out = ptr + 1;
                *outlen = static_cast<unsigned char>(len);
                return SSL_TLSEXT_ERR_OK;
            }
            ptr += len + 1;
            remaining -= len + 1;
        }
    }

    return SSL_TLSEXT_ERR_NOACK;
}

} // namespace fiber::http
