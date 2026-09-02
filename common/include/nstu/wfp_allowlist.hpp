#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nstu::net {

struct WfpAllowlistConfig {
    std::vector<std::uint32_t> allowed_ipv4;
    bool block_http = true;
    bool block_https = true;
};

class WfpWebsiteAllowlist {
public:
    WfpWebsiteAllowlist() = default;
    WfpWebsiteAllowlist(const WfpWebsiteAllowlist&) = delete;
    WfpWebsiteAllowlist& operator=(const WfpWebsiteAllowlist&) = delete;

    [[nodiscard]] bool apply(const WfpAllowlistConfig& config,
                              std::string* error = nullptr);
    [[nodiscard]] bool clear(std::string* error = nullptr);
    [[nodiscard]] bool active() const noexcept { return active_; }

private:
    bool active_ = false;
};

} // namespace nstu::net
