#ifndef FIBER_HTTP_TLS_CONTEXT_H
#define FIBER_HTTP_TLS_CONTEXT_H

#include <string>
#include <vector>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "TlsOptions.h"

struct ssl_ctx_st;
typedef struct ssl_ctx_st SSL_CTX;
struct ssl_st;
typedef struct ssl_st SSL;

namespace fiber::http {

class TlsContext : public common::NonCopyable, public common::NonMovable {
public:
    explicit TlsContext(TlsOptions options, bool is_server = true);
    ~TlsContext();

    common::IoResult<void> init();

    [[nodiscard]] SSL_CTX *raw() const noexcept { return ctx_; }
    [[nodiscard]] const TlsOptions &options() const noexcept { return options_; }
    [[nodiscard]] const std::vector<std::string> &alpn() const noexcept { return alpn_; }
    [[nodiscard]] bool is_server() const noexcept { return is_server_; }

private:
    static int alpn_select_cb(SSL *ssl,
                              const unsigned char **out,
                              unsigned char *outlen,
                              const unsigned char *in,
                              unsigned int inlen,
                              void *arg);

    SSL_CTX *ctx_ = nullptr;
    TlsOptions options_{};
    std::vector<std::string> alpn_{};
    std::vector<unsigned char> alpn_wire_{};
    bool is_server_ = true;
};

} // namespace fiber::http

#endif // FIBER_HTTP_TLS_CONTEXT_H
