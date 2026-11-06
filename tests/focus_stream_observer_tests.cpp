#include "../server/app/focus_stream_observer.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>

using Observer = nstu::ui::FocusStreamObserver;
using namespace std::chrono_literals;

Observer::Tick tick(std::uint64_t generation, std::chrono::milliseconds elapsed,
                    Observer::RequestOutcome outcome = Observer::RequestOutcome::none,
                    bool reported = true) {
    return {.in_focus = true, .selected_client_id = 7, .desired_stream_id = 7,
            .owner = Observer::Owner::focus, .generation = generation,
            .non_offline = true, .reported_streaming = reported, .outcome = outcome,
            .now = Observer::Clock::time_point{} + elapsed};
}
Observer::Frame frame(std::uint64_t generation, std::chrono::milliseconds elapsed) {
    return {.rendered_client_id = 7, .selected_client_id = 7, .generation = generation,
            .in_focus = true, .usable_texture = true,
            .now = Observer::Clock::time_point{} + elapsed};
}
void verify_request_and_rate() {
    Observer o;
    o.tick(tick(100, 0ms));
    assert(!o.present(frame(100, 0ms)).live);
    o.tick(tick(100, 0ms, Observer::RequestOutcome::start_sent));
    assert(!o.present(frame(100, 0ms)).live);
    assert(!o.present(frame(100, 0ms)).fps_sample_ready);
    o.tick(tick(101, 100ms));
    assert(o.present(frame(101, 100ms)).live);
    assert(!o.present(frame(101, 100ms)).fps_sample_ready);
    o.tick(tick(113, 999ms));
    assert(!o.present(frame(113, 999ms)).fps_sample_ready);
    o.tick(tick(113, 1000ms));
    const auto p = o.present(frame(113, 1000ms));
    assert(p.live && p.fps_sample_ready && std::abs(p.observed_fps - 13.0) < 1e-9);
    o.tick(tick(139, 3000ms));
    assert(std::abs(o.present(frame(139, 3000ms)).observed_fps - 13.0) < 1e-9);
}
void verify_pending_report() {
    Observer o;
    o.tick(tick(100, 0ms, Observer::RequestOutcome::start_sent, false));
    o.tick(tick(101, 100ms, Observer::RequestOutcome::none, false));
    assert(!o.present(frame(101, 100ms)).live);
    o.tick(tick(101, 150ms));
    assert(!o.present(frame(101, 150ms)).live);
    o.tick(tick(102, 200ms));
    assert(o.present(frame(102, 200ms)).live);
    o.tick(tick(114, 1150ms));
    assert(std::abs(o.present(frame(114, 1150ms)).observed_fps - 13.0) < 1e-9);
    o.tick(tick(115, 1200ms, Observer::RequestOutcome::none, false));
    o.tick(tick(115, 1300ms));
    o.tick(tick(116, 1400ms));
    assert(!o.present(frame(116, 1400ms)).live);
    o.tick(tick(116, 1500ms, Observer::RequestOutcome::rearm_sent));
    assert(!o.present(frame(116, 1500ms)).live);
    o.tick(tick(117, 1600ms));
    assert(o.present(frame(117, 1600ms)).live);
}
void verify_first_report_with_new_delivery() {
    for (const auto outcome : {Observer::RequestOutcome::start_sent, Observer::RequestOutcome::rearm_sent}) {
        Observer o;
        o.tick(tick(100, 0ms, outcome, false));
        o.tick(tick(101, 100ms));
        const auto first = o.present(frame(101, 100ms));
        assert(first.live && !first.fps_sample_ready);
        o.tick(tick(113, 1100ms));
        assert(std::abs(o.present(frame(113, 1100ms)).observed_fps - 12.0) < 1e-9);
    }
}
void verify_failures_and_reset() {
    Observer o;
    o.tick(tick(100, 0ms, Observer::RequestOutcome::start_failed));
    o.tick(tick(113, 1000ms));
    assert(!o.present(frame(113, 1000ms)).live);
    o.tick(tick(113, 1100ms, Observer::RequestOutcome::rearm_sent));
    o.tick(tick(114, 1200ms));
    assert(o.present(frame(114, 1200ms)).live);
    o.tick(tick(114, 1300ms, Observer::RequestOutcome::rearm_failed));
    o.tick(tick(125, 2300ms));
    assert(!o.present(frame(125, 2300ms)).live);
    o.tick(tick(125, 2400ms, Observer::RequestOutcome::rearm_sent));
    o.tick(tick(2, 2500ms));
    o.tick(tick(3, 2600ms));
    assert(!o.present(frame(3, 2600ms)).live);
    o.tick(tick(3, 2700ms, Observer::RequestOutcome::start_sent));
    o.tick(tick(4, 2800ms));
    assert(o.present(frame(4, 2800ms)).live);
    o.tick(tick(4, 2000ms));
    assert(!o.present(frame(4, 2000ms)).live);
    o.tick(tick(4, 1900ms, Observer::RequestOutcome::start_sent));
    o.tick(tick(5, 2000ms));
    assert(o.present(frame(5, 2000ms)).live);
    o.tick(tick(std::numeric_limits<std::uint64_t>::max(), 3000ms));
    o.tick(tick(0, 3100ms));
    assert(!o.present(frame(0, 3100ms)).live);
}
void verify_frame_masks_and_freshness() {
    Observer o;
    o.tick(tick(100, 0ms, Observer::RequestOutcome::start_sent));
    o.tick(tick(113, 1000ms));
    for (int kind = 0; kind < 5; ++kind) {
        auto f = frame(113, 1000ms);
        if (kind == 0) f.selected_client_id = 9;
        if (kind == 1) f.rendered_client_id = 9;
        if (kind == 2) f.generation = 112;
        if (kind == 3) f.in_focus = false;
        if (kind == 4) f.usable_texture = false;
        const auto p = o.present(f);
        assert(!p.live && !p.fps_sample_ready && p.observed_fps == 0.0);
    }
    assert(o.present(frame(113, 2999ms)).live);
    assert(!o.present(frame(113, 3000ms)).live);
    assert(!o.present(frame(113, 999ms)).live);
}
void verify_owner_and_target() {
    for (int kind = 0; kind < 4; ++kind) {
        Observer o;
        o.tick(tick(100, 0ms, Observer::RequestOutcome::start_sent));
        o.tick(tick(113, 1000ms));
        auto t = tick(114, 1100ms);
        if (kind == 0) t.owner = Observer::Owner::legacy_remote;
        if (kind == 1) t.in_focus = false;
        if (kind == 2) t.non_offline = false;
        if (kind == 3) t.selected_client_id = t.desired_stream_id = 9;
        o.tick(t);
        o.tick(tick(115, 1200ms));
        assert(!o.present(frame(115, 1200ms)).live);
        o.tick(tick(115, 1300ms, Observer::RequestOutcome::start_sent));
        o.tick(tick(116, 1400ms));
        assert(o.present(frame(116, 1400ms)).live);
    }
    Observer o;
    auto t = tick(100, 0ms, Observer::RequestOutcome::start_sent);
    t.owner = Observer::Owner::legacy_remote;
    o.tick(t);
    o.tick(tick(101, 100ms));
    assert(!o.present(frame(101, 100ms)).live);
    o.tick(tick(101, 200ms, Observer::RequestOutcome::start_sent));
    o.tick(tick(102, 300ms));
    assert(o.present(frame(102, 300ms)).live);
}
int main() {
    verify_request_and_rate(); verify_pending_report(); verify_failures_and_reset();
    verify_first_report_with_new_delivery();
    verify_frame_masks_and_freshness(); verify_owner_and_target();
    std::cout << "Focus request, ownership, delivery, freshness and FPS observations: PASS\n";
}
