#include "nstu/rate_limiter.hpp"

#include <cassert>
#include <chrono>

int main() {
    using namespace std::chrono_literals;
    using Clock = std::chrono::steady_clock;
    nstu::security::HandshakeRateLimiter limiter({
        .maximum_sources = 2,
        .maximum_failures = 3,
        .window = 10s,
        .block = 5s,
    });
    const auto start = Clock::time_point{1s};

    assert(!limiter.allow("" , start));
    assert(limiter.allow("10.0.0.1", start));
    limiter.record_failure("10.0.0.1", start + 1s);
    limiter.record_failure("10.0.0.1", start + 2s);
    assert(limiter.allow("10.0.0.1", start + 2s));
    limiter.record_failure("10.0.0.1", start + 3s);
    assert(!limiter.allow("10.0.0.1", start + 3s));
    assert(!limiter.allow("10.0.0.1", start + 7s));
    assert(!limiter.allow("10.0.0.1", start + 9s));
    assert(limiter.allow("10.0.0.1", start + 11s));

    limiter.record_failure("10.0.0.2", start + 12s);
    limiter.record_success("10.0.0.2");
    assert(limiter.allow("10.0.0.2", start + 12s));

    limiter.record_failure("10.0.0.3", start + 13s);
    assert(limiter.tracked_sources() == 2);
    limiter.record_success("10.0.0.1");
    limiter.record_success("10.0.0.3");
    assert(limiter.tracked_sources() == 0);
    return 0;
}
