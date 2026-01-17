#ifndef FIBER_HTTP_TLS_OPTIONS_H
#define FIBER_HTTP_TLS_OPTIONS_H

#include <chrono>
#include <string>
#include <vector>

namespace fiber::http {

struct TlsOptions {
    bool enabled = false;
    std::string cert_file;
    std::string key_file;
    std::string ca_file;
    bool verify_client = false;
    std::chrono::seconds handshake_timeout{10};
    int min_version = 0x0303; // TLS 1.2
    int max_version = 0x0304; // TLS 1.3
    std::vector<std::string> alpn{"http/1.1"};
};

} // namespace fiber::http

#endif // FIBER_HTTP_TLS_OPTIONS_H
