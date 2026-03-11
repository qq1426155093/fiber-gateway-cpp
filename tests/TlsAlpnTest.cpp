#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "http/TlsAlpn.h"

namespace {

TEST(TlsAlpnTest, NormalizeHttp1AlpnPrefersHttp11AndDropsH2) {
    fiber::http::TlsOptions options;
    options.alpn = {"h2", "", "acme/1", "http/1.1", "custom"};

    fiber::http::normalize_http1_alpn(options);

    const std::vector<std::string> expected = {"http/1.1", "acme/1", "custom"};
    EXPECT_EQ(options.alpn, expected);
}

TEST(TlsAlpnTest, NormalizeHttp1AlpnAddsHttp11WhenMissing) {
    fiber::http::TlsOptions options;
    options.alpn.clear();

    fiber::http::normalize_http1_alpn(options);

    const std::vector<std::string> expected = {"http/1.1"};
    EXPECT_EQ(options.alpn, expected);
}

} // namespace
