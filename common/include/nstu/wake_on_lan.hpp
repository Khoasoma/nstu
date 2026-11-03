#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace nstu::net {

struct MacAddress {
    std::array<std::uint8_t, 6> octets{};

    [[nodiscard]] bool operator==(const MacAddress& other) const noexcept = default;

    [[nodiscard]] bool is_zero() const noexcept {
        return octets == std::array<std::uint8_t, 6>{0, 0, 0, 0, 0, 0};
    }

    [[nodiscard]] bool is_broadcast() const noexcept {
        return octets == std::array<std::uint8_t, 6>{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    }
};

enum class WolSupportState : std::uint8_t {
    unknown = 0,
    supported = 1,
    unsupported = 2,
    missing_mac = 3,
};

[[nodiscard]] const char* to_string(WolSupportState state) noexcept;
[[nodiscard]] const char* wol_support_label(WolSupportState state, bool vietnamese) noexcept;

[[nodiscard]] std::optional<MacAddress> parse_mac_address(std::string_view text) noexcept;

[[nodiscard]] std::string format_mac_address(const MacAddress& mac,
                                             char separator = ':',
                                             bool uppercase = true);

inline constexpr std::size_t kWolMagicPacketSize = 102; // 6 bytes 0xFF + 16 * 6 bytes MAC

[[nodiscard]] std::array<std::uint8_t, kWolMagicPacketSize> create_magic_packet(
    const MacAddress& mac) noexcept;

[[nodiscard]] bool verify_magic_packet(
    std::span<const std::uint8_t> packet,
    const MacAddress& expected_mac) noexcept;

struct WolBroadcastOptions {
    std::string broadcast_ip = "255.255.255.255";
    std::uint16_t port = 9;
};

[[nodiscard]] bool send_wake_on_lan_packet(
    const MacAddress& mac,
    const WolBroadcastOptions& options = {},
    std::string* error = nullptr);

} // namespace nstu::net

