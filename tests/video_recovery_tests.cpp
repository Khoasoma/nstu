#include "nstu/recovering_pipeline.hpp"

#include <cassert>
#include <chrono>

int main() {
    using namespace std::chrono_literals;
    const auto start = std::chrono::steady_clock::time_point{1s};
    nstu::video::RecoveryController recovery({
        .initial_backoff = 100ms,
        .maximum_backoff = 800ms,
        .maximum_consecutive_failures = 5,
    });
    recovery.start(start);
    assert(recovery.state() == nstu::video::RecoveryState::recovering);
    assert(recovery.attempt_due(start));
    recovery.record_failure(start);
    assert(!recovery.attempt_due(start + 99ms));
    assert(recovery.attempt_due(start + 100ms));
    recovery.record_failure(start + 100ms);
    assert(recovery.next_attempt() == start + 300ms);
    recovery.record_success();
    assert(recovery.state() == nstu::video::RecoveryState::running);
    assert(recovery.total_recoveries() == 1);
    assert(recovery.consecutive_failures() == 0);

    recovery.record_failure(start + 400ms);
    recovery.record_failure(start + 500ms);
    recovery.record_failure(start + 700ms);
    recovery.record_failure(start + 1100ms);
    recovery.record_failure(start + 1900ms);
    assert(recovery.state() == nstu::video::RecoveryState::failed);
    assert(!recovery.attempt_due(start + 10s));
    recovery.stop();
    assert(recovery.state() == nstu::video::RecoveryState::stopped);
    return 0;
}
