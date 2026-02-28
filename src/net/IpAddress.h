#ifndef FIBER_NET_IP_ADDRESS_H
#define FIBER_NET_IP_ADDRESS_H

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "../common/Assert.h"

namespace fiber::net {

enum class IpFamily : std::uint8_t { V4, V6 };

class IpAddress {
public:
    IpAddress();

    static IpAddress v4(std::array<std::uint8_t, 4> bytes);
    static IpAddress v6(std::array<std::uint8_t, 16> bytes, std::uint32_t scope_id = 0);

    static IpAddress any_v4();
    static IpAddress any_v6();
    static IpAddress loopback_v4();
    static IpAddress loopback_v6();

    [[nodiscard]] IpFamily family() const noexcept {
        return family_;
    }
    [[nodiscard]] bool is_v4() const noexcept {
        return family_ == IpFamily::V4;
    }
    [[nodiscard]] bool is_v6() const noexcept {
        return family_ == IpFamily::V6;
    }
    [[nodiscard]] std::uint32_t scope_id() const noexcept {
        return scope_id_;
    }
    [[nodiscard]] bool is_loopback() const noexcept;
    [[nodiscard]] bool is_unspecified() const noexcept;
    [[nodiscard]] bool is_multicast() const noexcept;
    [[nodiscard]] const std::array<std::uint8_t, 4> &v4_bytes() const {
        FIBER_ASSERT(is_v4());
        return bytes_.v4;
    }
    [[nodiscard]] const std::array<std::uint8_t, 16> &v6_bytes() const {
        FIBER_ASSERT(is_v6());
        return bytes_.v6;
    }

    static bool parse(std::string_view text, IpAddress &out);
    std::string to_string() const;

private:
    union AddressBytes {
        std::array<std::uint8_t, 4> v4;
        std::array<std::uint8_t, 16> v6;

        constexpr AddressBytes() : v4{} {}
    };

    IpFamily family_{IpFamily::V4};
    AddressBytes bytes_{};
    std::uint32_t scope_id_{0};
};

} // namespace fiber::net

#endif // FIBER_NET_IP_ADDRESS_H
