#include "IpAddress.h"

#include <arpa/inet.h>
#include <cstring>

namespace fiber::net {

IpAddress::IpAddress() = default;

IpAddress IpAddress::v4(std::array<std::uint8_t, 4> bytes) {
    IpAddress out;
    out.family_ = IpFamily::V4;
    out.bytes_.v4 = bytes;
    return out;
}

IpAddress IpAddress::v6(std::array<std::uint8_t, 16> bytes, std::uint32_t scope_id) {
    IpAddress out;
    out.family_ = IpFamily::V6;
    out.bytes_.v6 = bytes;
    out.scope_id_ = scope_id;
    return out;
}

IpAddress IpAddress::any_v4() {
    return v4({0, 0, 0, 0});
}

IpAddress IpAddress::any_v6() {
    std::array<std::uint8_t, 16> bytes{};
    return v6(bytes);
}

IpAddress IpAddress::loopback_v4() {
    return v4({127, 0, 0, 1});
}

IpAddress IpAddress::loopback_v6() {
    std::array<std::uint8_t, 16> bytes{};
    bytes[15] = 1;
    return v6(bytes);
}

bool IpAddress::is_loopback() const noexcept {
    if (is_v4()) {
        return bytes_.v4[0] == 127;
    }
    if (is_v6()) {
        for (std::size_t i = 0; i + 1 < bytes_.v6.size(); ++i) {
            if (bytes_.v6[i] != 0) {
                return false;
            }
        }
        return bytes_.v6[15] == 1;
    }
    return false;
}

bool IpAddress::is_unspecified() const noexcept {
    if (is_v4()) {
        return bytes_.v4[0] == 0 && bytes_.v4[1] == 0 && bytes_.v4[2] == 0 && bytes_.v4[3] == 0;
    }
    if (is_v6()) {
        for (auto byte : bytes_.v6) {
            if (byte != 0) {
                return false;
            }
        }
        return true;
    }
    return false;
}

bool IpAddress::is_multicast() const noexcept {
    if (is_v4()) {
        return (bytes_.v4[0] & 0xF0) == 0xE0;
    }
    if (is_v6()) {
        return bytes_.v6[0] == 0xFF;
    }
    return false;
}

bool IpAddress::parse(std::string_view text, IpAddress &out) {
    if (text.empty()) {
        return false;
    }
    std::string input(text);
    if (input.size() >= 2 && input.front() == '[' && input.back() == ']') {
        input = input.substr(1, input.size() - 2);
    }
    std::array<std::uint8_t, 16> buf{};
    if (::inet_pton(AF_INET, input.c_str(), buf.data()) == 1) {
        std::array<std::uint8_t, 4> v4{};
        std::memcpy(v4.data(), buf.data(), v4.size());
        out = IpAddress::v4(v4);
        return true;
    }
    if (::inet_pton(AF_INET6, input.c_str(), buf.data()) == 1) {
        out = v6(buf);
        return true;
    }
    return false;
}

std::string IpAddress::to_string() const {
    char buffer[INET6_ADDRSTRLEN]{};
    const void *src = nullptr;
    int family = AF_INET;
    if (is_v4()) {
        src = bytes_.v4.data();
        family = AF_INET;
    } else {
        src = bytes_.v6.data();
        family = AF_INET6;
    }
    const char *result = ::inet_ntop(family, src, buffer, sizeof(buffer));
    if (!result) {
        return {};
    }
    return std::string(result);
}

} // namespace fiber::net
