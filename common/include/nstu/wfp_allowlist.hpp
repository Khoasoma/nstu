#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <string>
#include <vector>

namespace nstu::net {

struct Ipv6Address {
    std::array<std::uint8_t, 16> bytes{};

    constexpr bool operator==(const Ipv6Address& other) const noexcept = default;
    constexpr auto operator<=>(const Ipv6Address& other) const noexcept = default;
};

struct WfpAllowlistConfig {
    std::vector<std::uint32_t> allowed_ipv4{};
    std::vector<Ipv6Address> allowed_ipv6{};
    std::uint32_t server_ipv4 = 0;       // Host byte order IPv4
    Ipv6Address server_ipv6{};
    std::uint16_t server_port = 0;
    bool default_deny = true;
    bool permit_loopback = true;
    bool permit_dns_dhcp = true;
    bool block_http = true;
    bool block_https = true;
};

class WfpWebsiteAllowlist {
public:
    WfpWebsiteAllowlist() = default;
    ~WfpWebsiteAllowlist();

    WfpWebsiteAllowlist(const WfpWebsiteAllowlist&) = delete;
    WfpWebsiteAllowlist& operator=(const WfpWebsiteAllowlist&) = delete;

    WfpWebsiteAllowlist(WfpWebsiteAllowlist&& other) noexcept;
    WfpWebsiteAllowlist& operator=(WfpWebsiteAllowlist&& other) noexcept;

    [[nodiscard]] bool apply(const WfpAllowlistConfig& config,
                             std::string* error = nullptr);
    [[nodiscard]] bool clear(std::string* error = nullptr);
    [[nodiscard]] bool active() const noexcept { return active_; }

private:
    bool active_ = false;
};

} // namespace nstu::net
