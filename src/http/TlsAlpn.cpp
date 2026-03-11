#include "TlsAlpn.h"

#include <string>
#include <vector>

namespace fiber::http {

void normalize_http1_alpn(TlsOptions &options) {
    std::vector<std::string> normalized;
    normalized.reserve(options.alpn.size() + 1);

    for (const auto &proto : options.alpn) {
        if (proto.empty() || proto == "h2" || proto == "http/1.1") {
            continue;
        }
        normalized.push_back(proto);
    }

    normalized.insert(normalized.begin(), "http/1.1");
    options.alpn = std::move(normalized);
}

} // namespace fiber::http
