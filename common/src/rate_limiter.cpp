#include "nstu/rate_limiter.hpp"

#include <algorithm>
#include <iterator>

namespace nstu::security {

HandshakeRateLimiter::HandshakeRateLimiter(HandshakeRateLimitPolicy policy)
    : policy_(policy) {
    policy_.maximum_sources = std::max<std::size_t>(policy_.maximum_sources, 1);
    policy_.maximum_failures = std::max<std::uint32_t>(policy_.maximum_failures, 1);
    policy_.window = std::max(policy_.window, std::chrono::seconds{1});
    policy_.block = std::max(policy_.block, std::chrono::seconds{1});
}

bool HandshakeRateLimiter::allow(
    std::string_view source,
    std::chrono::steady_clock::time_point now) noexcept {
    if (source.empty()) {
        return false;
    }
    std::scoped_lock lock(mutex_);
    const auto found = sources_.find(std::string(source));
    if (found == sources_.end()) {
        return true;
    }
    auto& entry = found->second;
    entry.last_seen = now;
    if (entry.blocked_until > now) {
        return false;
    }
    reset_window_locked(entry, now);
    return entry.failures < policy_.maximum_failures;
}

void HandshakeRateLimiter::record_failure(
    std::string_view source,
    std::chrono::steady_clock::time_point now) noexcept {
    if (source.empty()) {
        return;
    }
    std::scoped_lock lock(mutex_);
    auto [found, inserted] = sources_.try_emplace(std::string(source));
    auto& entry = found->second;
    if (inserted) {
        entry.window_start = now;
    } else {
        reset_window_locked(entry, now);
    }
    entry.last_seen = now;
    if (entry.failures < policy_.maximum_failures) {
        ++entry.failures;
    }
    if (entry.failures >= policy_.maximum_failures) {
        entry.blocked_until = now + policy_.block;
    }
    trim_locked();
}

void HandshakeRateLimiter::record_success(std::string_view source) noexcept {
    if (source.empty()) {
        return;
    }
    std::scoped_lock lock(mutex_);
    sources_.erase(std::string(source));
}

std::size_t HandshakeRateLimiter::tracked_sources() const noexcept {
    std::scoped_lock lock(mutex_);
    return sources_.size();
}

void HandshakeRateLimiter::trim_locked() noexcept {
    while (sources_.size() > policy_.maximum_sources) {
        auto oldest = sources_.begin();
        for (auto it = std::next(sources_.begin()); it != sources_.end(); ++it) {
            if (it->second.last_seen < oldest->second.last_seen) {
                oldest = it;
            }
        }
        sources_.erase(oldest);
    }
}

void HandshakeRateLimiter::reset_window_locked(
    Entry& entry, std::chrono::steady_clock::time_point now) noexcept {
    if (entry.window_start.time_since_epoch().count() == 0 ||
        now - entry.window_start >= policy_.window) {
        entry.failures = 0;
        entry.window_start = now;
        entry.blocked_until = {};
    }
    if (entry.blocked_until <= now) {
        entry.blocked_until = {};
    }
}

} // namespace nstu::security
