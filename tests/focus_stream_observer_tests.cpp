#include "../server/app/focus_stream_observer.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>

using Observer = nstu::ui::FocusStreamObserver;
using Requests = nstu::ui::FocusStreamRequests;
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
void verify_production_same_client_handover() {
    Requests requests;
    Observer observer;
    auto remote_tick = tick(100, 0ms);
    remote_tick.owner = Observer::Owner::legacy_remote;
    const auto remote = requests.advance({7, remote_tick.owner, 100, remote_tick.now});
    assert(remote.request.client_id == 7);
    remote_tick.outcome = requests.complete(remote.request, true);
    observer.tick(remote_tick);
    assert(!observer.present(frame(100, 0ms)).live);

    auto focus_tick = tick(101, 100ms);
    const auto handover = requests.advance({7, focus_tick.owner, 101, focus_tick.now});
    assert(handover.stop_client_id == 0);
    assert(handover.request.client_id == 7);
    assert(handover.request.owner == Observer::Owner::focus);
    focus_tick.outcome = requests.complete(handover.request, true);
    observer.tick(focus_tick);
    assert(!observer.present(frame(101, 100ms)).live);
    assert(observer.present(frame(101, 100ms)).phase == Observer::Phase::requesting);
    auto delivered = tick(102, 200ms);
    const auto continuing = requests.advance({7, delivered.owner, 102, delivered.now});
    assert(continuing.request.client_id == 0);
    observer.tick(delivered);
    assert(observer.present(frame(102, 200ms)).live);

    remote_tick = tick(103, 300ms);
    remote_tick.owner = Observer::Owner::legacy_remote;
    const auto back_to_remote = requests.advance({7, remote_tick.owner, 103, remote_tick.now});
    assert(back_to_remote.stop_client_id == 0);
    assert(back_to_remote.request.owner == Observer::Owner::legacy_remote);
    remote_tick.outcome = requests.complete(back_to_remote.request, true);
    observer.tick(remote_tick);
    assert(!observer.present(frame(103, 300ms)).live);
    focus_tick = tick(104, 400ms);
    const auto back_to_focus = requests.advance({7, focus_tick.owner, 104, focus_tick.now});
    assert(back_to_focus.stop_client_id == 0 && back_to_focus.request.client_id == 7);
    focus_tick.outcome = requests.complete(back_to_focus.request, true);
    observer.tick(focus_tick);
    assert(!observer.present(frame(104, 400ms)).live);
    observer.tick(tick(105, 500ms));
    assert(observer.present(frame(105, 500ms)).live);
}
void verify_production_pending_and_failed_outcomes() {
    for (const bool sent : {false, true}) {
        Requests requests;
        Observer observer;
        auto start = tick(100, 0ms, Observer::RequestOutcome::none, false);
        const auto decision = requests.advance({7, start.owner, 100, start.now});
        start.outcome = requests.complete(decision.request, sent);
        assert(start.outcome == (sent ? Observer::RequestOutcome::start_sent
                                     : Observer::RequestOutcome::start_failed));
        observer.tick(start);
        auto pending = tick(101, 100ms, Observer::RequestOutcome::none, false);
        const auto advancing = requests.advance({7, pending.owner, 101, pending.now});
        assert(advancing.request.client_id == 0);
        observer.tick(pending);
        assert(!observer.present(frame(101, 100ms)).live);
        observer.tick(tick(101, 200ms));
        assert(!observer.present(frame(101, 200ms)).live);
        auto delivered = tick(102, 300ms);
        assert(requests.advance({7, delivered.owner, 102, delivered.now}).request.client_id == 0);
        observer.tick(delivered);
        assert(observer.present(frame(102, 300ms)).live == sent);
        if (!sent) assert(observer.present(frame(102, 300ms)).phase == Observer::Phase::request_failed);
    }
}
void verify_production_stale_completion_and_target_switch() {
    Requests requests;
    Observer observer;
    const auto old_start = requests.advance({7, Observer::Owner::focus, 100, {}});
    auto target_tick = tick(400, 100ms);
    target_tick.selected_client_id = target_tick.desired_stream_id = 9;
    const auto switched = requests.advance({9, target_tick.owner, 400, target_tick.now});
    assert(switched.stop_client_id == 7 && switched.request.client_id == 9);
    assert(requests.complete(old_start.request, true) == Observer::RequestOutcome::none);
    observer.tick(target_tick);
    auto target_frame = frame(400, 100ms);
    target_frame.rendered_client_id = target_frame.selected_client_id = 9;
    assert(!observer.present(target_frame).live);
    target_tick.outcome = requests.complete(switched.request, true);
    observer.tick(target_tick);
    assert(!observer.present(target_frame).live);
    assert(requests.complete(switched.request, false) == Observer::RequestOutcome::none);
    target_tick.generation = target_frame.generation = 401;
    target_tick.now = target_frame.now = Observer::Clock::time_point{} + 200ms;
    target_tick.outcome = Observer::RequestOutcome::none;
    observer.tick(target_tick);
    assert(observer.present(target_frame).live);
    auto stale_frame = frame(101, 200ms);
    stale_frame.selected_client_id = 9;
    assert(!observer.present(stale_frame).live);

    const auto pending_rearm = requests.advance({9, Observer::Owner::focus, 400,
                                                 Observer::Clock::time_point{} + 2100ms});
    assert(pending_rearm.request.kind == Requests::Kind::rearm);
    const auto remote = requests.advance({9, Observer::Owner::legacy_remote, 401,
                                          Observer::Clock::time_point{} + 2200ms});
    assert(remote.stop_client_id == 0 && remote.request.client_id == 9);
    assert(requests.complete(pending_rearm.request, true) == Observer::RequestOutcome::none);
    assert(requests.complete(remote.request, true) == Observer::RequestOutcome::none);
    const auto focus = requests.advance({9, Observer::Owner::focus, 402,
                                         Observer::Clock::time_point{} + 2300ms});
    assert(focus.request.serial != switched.request.serial);
    assert(requests.complete(switched.request, true) == Observer::RequestOutcome::none);
    assert(requests.complete(focus.request, false) == Observer::RequestOutcome::start_failed);
}
void verify_production_watchdog_and_stop_restart() {
    Requests requests;
    const auto at = [](auto elapsed) { return Observer::Clock::time_point{} + elapsed; };
    const auto start = requests.advance({7, Observer::Owner::focus, 100, at(0ms)});
    assert(start.rearm == nstu::server::StreamRearm::none);
    assert(requests.complete(start.request, true) == Observer::RequestOutcome::start_sent);
    assert(requests.advance({7, Observer::Owner::focus, 100, at(1999ms)}).request.client_id == 0);
    const auto first = requests.advance({7, Observer::Owner::focus, 100, at(2000ms)});
    assert(first.rearm == nstu::server::StreamRearm::first);
    assert(requests.complete(first.request, false) == Observer::RequestOutcome::rearm_failed);
    assert(requests.advance({7, Observer::Owner::focus, 100, at(3999ms)}).request.client_id == 0);
    const auto repeat = requests.advance({7, Observer::Owner::focus, 100, at(4000ms)});
    assert(repeat.rearm == nstu::server::StreamRearm::repeat);
    const auto remote = requests.advance({7, Observer::Owner::legacy_remote, 100, at(6000ms)});
    assert(remote.request.kind == Requests::Kind::start);
    assert(remote.rearm == nstu::server::StreamRearm::none);
    assert(requests.complete(repeat.request, true) == Observer::RequestOutcome::none);
    assert(requests.complete(remote.request, true) == Observer::RequestOutcome::none);
    assert(requests.advance({7, Observer::Owner::legacy_remote, 101, at(7000ms)}).request.client_id == 0);
    assert(requests.advance({7, Observer::Owner::legacy_remote, 101, at(8999ms)}).request.client_id == 0);
    const auto resumed_stall = requests.advance({7, Observer::Owner::legacy_remote, 101, at(9000ms)});
    assert(resumed_stall.rearm == nstu::server::StreamRearm::first);
    const auto stop = requests.advance({0, Observer::Owner::none, 0, at(9100ms)});
    assert(stop.stop_client_id == 7 && stop.request.client_id == 0);
    assert(requests.client_id() == 0 && requests.owner() == Observer::Owner::none);
    assert(requests.complete(resumed_stall.request, true) == Observer::RequestOutcome::none);

    Observer observer;
    auto restart_tick = tick(500, 12000ms);
    const auto restart = requests.advance({7, restart_tick.owner, 500, restart_tick.now});
    assert(restart.request.kind == Requests::Kind::start);
    assert(restart.rearm == nstu::server::StreamRearm::none);
    restart_tick.outcome = requests.complete(restart.request, true);
    observer.tick(restart_tick);
    assert(!observer.present(frame(500, 12000ms)).live);
    assert(requests.advance({7, Observer::Owner::focus, 500, at(13999ms)}).request.client_id == 0);
    const auto recovery = requests.advance({7, Observer::Owner::focus, 500, at(14000ms)});
    auto recovery_tick = tick(500, 14000ms);
    recovery_tick.outcome = requests.complete(recovery.request, true);
    assert(recovery_tick.outcome == Observer::RequestOutcome::rearm_sent);
    observer.tick(recovery_tick);
    assert(!observer.present(frame(500, 14000ms)).live);
    observer.tick(tick(501, 14100ms));
    assert(observer.present(frame(501, 14100ms)).live);

    const auto older = requests.advance({7, Observer::Owner::focus, 501, at(15000ms)});
    assert(older.request.client_id == 0);
    const auto pending = requests.advance({7, Observer::Owner::focus, 501, at(17000ms)});
    const auto newer = requests.advance({7, Observer::Owner::focus, 501, at(19000ms)});
    assert(pending.request.serial != newer.request.serial);
    assert(requests.complete(pending.request, true) == Observer::RequestOutcome::none);
    assert(requests.complete(newer.request, false) == Observer::RequestOutcome::rearm_failed);
}
void verify_production_stop_clears_live_evidence() {
    Requests requests;
    Observer observer;
    auto t = tick(100, 0ms);
    auto decision = requests.advance({7, t.owner, 100, t.now});
    t.outcome = requests.complete(decision.request, true);
    observer.tick(t);
    t = tick(101, 100ms);
    assert(requests.advance({7, t.owner, 101, t.now}).request.client_id == 0);
    observer.tick(t);
    assert(observer.present(frame(101, 100ms)).live);

    t = tick(102, 200ms);
    t.in_focus = false;
    t.desired_stream_id = 0;
    t.owner = Observer::Owner::none;
    decision = requests.advance({0, t.owner, 0, t.now});
    assert(decision.stop_client_id == 7 && decision.request.client_id == 0);
    observer.tick(t);
    assert(!observer.present(frame(102, 200ms)).live);
    t = tick(102, 300ms);
    decision = requests.advance({7, t.owner, 102, t.now});
    t.outcome = requests.complete(decision.request, true);
    observer.tick(t);
    assert(!observer.present(frame(102, 300ms)).live);
    t = tick(103, 400ms);
    assert(requests.advance({7, t.owner, 103, t.now}).request.client_id == 0);
    observer.tick(t);
    assert(observer.present(frame(103, 400ms)).live);
}
int main() {
    verify_request_and_rate(); verify_pending_report(); verify_failures_and_reset();
    verify_first_report_with_new_delivery();
    verify_frame_masks_and_freshness(); verify_owner_and_target();
    verify_production_same_client_handover();
    verify_production_pending_and_failed_outcomes();
    verify_production_stale_completion_and_target_switch();
    verify_production_watchdog_and_stop_restart();
    verify_production_stop_clears_live_evidence();
    std::cout << "Focus request, ownership, delivery, freshness and FPS observations: PASS\n";
}
