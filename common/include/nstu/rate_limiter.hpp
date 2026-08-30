#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace nstu::security {

struct HandshakeRateLimitPolicy {
    std::size_t maximum_sources = 4096;
    std::uint32_t maximum_failures = 5;
    std::chrono::seconds window{60};
    std::chrono::seconds block{300};
};

// Bounded, thread-safe admission limiter for unauthenticated handshakes.
// Call allow() before starting a handshake, record_failure() on rejection, and
// record_success() after authentication to clear the source state.
class HandshakeRateLimiter {
public:
    explicit HandshakeRateLimiter(HandshakeRateLimitPolicy policy = {});

    [[nodiscard]] bool allow(
        std::string_view source,
        std::chrono::steady_clock::time_point now =
            std::chrono::steady_clock::now()) noexcept;
    void record_failure(
        std::string_view source,
        std::chrono::steady_clock::time_point now =
            std::chrono::steady_clock::now()) noexcept;
    void record_success(std::string_view source) noexcept;
    [[nodiscard]] std::size_t tracked_sources() const noexcept;

private:
    struct Entry {
        std::uint32_t failures = 0;
        std::chrono::steady_clock::time_point window_start{};
        std::chrono::steady_clock::time_point blocked_until{};
        std::chrono::steady_clock::time_point last_seen{};
    };

    void trim_locked() noexcept;
    void reset_window_locked(Entry& entry,
                             std::chrono::steady_clock::time_point now) noexcept;

    HandshakeRateLimitPolicy policy_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Entry> sources_;
};

} // namespace nstu::security
