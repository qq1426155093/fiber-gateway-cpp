#ifndef FIBER_HTTP_TLS_ALPN_H
#define FIBER_HTTP_TLS_ALPN_H

#include "TlsOptions.h"

namespace fiber::http {

void normalize_http1_alpn(TlsOptions &options);

} // namespace fiber::http

#endif // FIBER_HTTP_TLS_ALPN_H
