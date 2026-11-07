#pragma once

#include <chrono>
#include <cstdint>

#include "../include/nstu/stream_rearm_watchdog.hpp"

namespace nstu::ui {

// Presentation-only evidence. Callers supply real request outcomes and delivered
// generations; this observer cannot send commands or infer media from a report.
class FocusStreamObserver {
public:
    using Clock = std::chrono::steady_clock;
    enum class Owner { none, focus, legacy_remote };
    enum class RequestOutcome { none, start_sent, start_failed, rearm_sent, rearm_failed };
    enum class Phase { inactive, offline, unverified, requesting, rearming,
                       request_failed, generation_reset, frame_unavailable, stalled, live };
    struct Tick {
        bool in_focus = false;
        std::uint64_t selected_client_id = 0, desired_stream_id = 0;
        Owner owner = Owner::none;
        std::uint64_t generation = 0;
        bool non_offline = false, reported_streaming = false;
        RequestOutcome outcome = RequestOutcome::none;
        Clock::time_point now{};
    };
    struct Frame {
        std::uint64_t rendered_client_id = 0, selected_client_id = 0, generation = 0;
        bool in_focus = false, usable_texture = false;
        Clock::time_point now{};
    };
    struct Presentation {
        Phase phase = Phase::inactive;
        bool live = false, fps_sample_ready = false;
        double observed_fps = 0.0;
        Clock::duration delivery_age{};
    };

    void reset() noexcept { *this = FocusStreamObserver{}; }
    void tick(const Tick& t) noexcept {
        const bool eligible = t.in_focus && t.non_offline && t.owner == Owner::focus &&
            t.selected_client_id != 0 && t.selected_client_id == t.desired_stream_id;
        if (!eligible) {
            reset();
            phase_ = !t.in_focus ? Phase::inactive
                : !t.non_offline ? Phase::offline : Phase::unverified;
            return;
        }
        if (!eligible_ || target_ != t.selected_client_id) {
            reset();
            phase_ = Phase::unverified;
        } else if (t.now < last_tick_ || t.generation < generation_) {
            reset();
            phase_ = Phase::generation_reset;
        }
        const bool advanced = t.generation > generation_;
        const auto remember = [&]() noexcept {
            eligible_ = true; target_ = t.selected_client_id;
            generation_ = t.generation; last_tick_ = t.now;
            reported_ = t.reported_streaming;
        };
        if (t.outcome == RequestOutcome::start_failed || t.outcome == RequestOutcome::rearm_failed) {
            invalidate(); phase_ = Phase::request_failed; remember(); return;
        }
        if (t.outcome == RequestOutcome::start_sent || t.outcome == RequestOutcome::rearm_sent) {
            invalidate(); accepted_ = true; request_generation_ = t.generation;
            phase_ = t.outcome == RequestOutcome::start_sent ? Phase::requesting : Phase::rearming;
            if (t.reported_streaming) start_sample(t);
            remember(); return;
        }
        if (reported_ && !t.reported_streaming) {
            invalidate(); phase_ = Phase::unverified;
        } else if (accepted_ && !reported_ && t.reported_streaming) {
            // Generations that arrived before the first streaming report stay
            // cached. A later generation must earn proof under that report.
            if (advanced && t.generation > request_generation_) {
                delivered_ = true; last_delivery_ = t.now; phase_ = Phase::live;
            } else request_generation_ = t.generation;
            start_sample(t);
        } else if (accepted_ && t.reported_streaming) {
            if (advanced && t.generation > request_generation_) {
                delivered_ = true; last_delivery_ = t.now; phase_ = Phase::live;
            }
            const auto elapsed = t.now - sample_time_;
            if (sample_active_ && elapsed >= std::chrono::seconds{1}) {
                fps_ = static_cast<double>(t.generation - sample_generation_) /
                       std::chrono::duration<double>(elapsed).count();
                sample_ready_ = true;
                sample_generation_ = t.generation; sample_time_ = t.now;
            }
        }
        remember();
    }

    [[nodiscard]] Presentation present(const Frame& f) const noexcept {
        Presentation p;
        p.phase = phase_;
        if (!eligible_) return p;
        if (!f.in_focus || f.selected_client_id != target_ || f.rendered_client_id != target_ ||
            f.generation != generation_) { p.phase = Phase::unverified; return p; }
        if (f.now < last_tick_) { p.phase = Phase::generation_reset; return p; }
        if (!accepted_ || !reported_ || !delivered_) return p;
        p.delivery_age = f.now - last_delivery_;
        if (p.delivery_age >= std::chrono::milliseconds{2000}) { p.phase = Phase::stalled; return p; }
        if (!f.usable_texture) { p.phase = Phase::frame_unavailable; return p; }
        p.phase = Phase::live; p.live = true;
        p.fps_sample_ready = sample_ready_;
        p.observed_fps = sample_ready_ ? fps_ : 0.0;
        return p;
    }

private:
    void invalidate() noexcept {
        accepted_ = false; delivered_ = false; sample_active_ = false;
        sample_ready_ = false; fps_ = 0.0;
    }
    void start_sample(const Tick& t) noexcept {
        sample_active_ = true; sample_generation_ = t.generation; sample_time_ = t.now;
    }
    Phase phase_ = Phase::inactive;
    bool eligible_ = false, accepted_ = false, reported_ = false, delivered_ = false;
    bool sample_active_ = false, sample_ready_ = false;
    std::uint64_t target_ = 0, generation_ = 0, request_generation_ = 0, sample_generation_ = 0;
    Clock::time_point last_tick_{}, last_delivery_{}, sample_time_{};
    double fps_ = 0.0;
};

// The dashboard's command decision seam. Request identities protect local
// completion authority; they do not identify the capture that produced a frame.
class FocusStreamRequests {
public:
    using Owner = FocusStreamObserver::Owner;
    using Outcome = FocusStreamObserver::RequestOutcome;
    using Clock = FocusStreamObserver::Clock;
    enum class Kind { start, rearm };
    struct Request {
        std::uint64_t client_id = 0;
        Owner owner = Owner::none;
        Kind kind = Kind::start;
        std::uint64_t serial = 0;
        bool operator==(const Request&) const = default;
    };
    struct Input {
        std::uint64_t client_id = 0;
        Owner owner = Owner::none;
        std::uint64_t generation = 0;
        Clock::time_point now{};
    };
    struct Decision {
        std::uint64_t stop_client_id = 0;
        Request request;
        server::StreamRearm rearm = server::StreamRearm::none;
    };

    [[nodiscard]] Decision advance(const Input& input) noexcept {
        Decision decision;
        const auto desired_id = input.owner == Owner::none ? 0 : input.client_id;
        const auto desired_owner = desired_id == 0 ? Owner::none : input.owner;
        if (client_id_ != desired_id || owner_ != desired_owner) {
            if (client_id_ != desired_id) decision.stop_client_id = client_id_;
            client_id_ = desired_id;
            owner_ = desired_owner;
            pending_ = {};
            // Same-target handover reasserts start without interrupting capture.
            // It also seeds a full stall window, never a second request now.
            watchdog_.reset();
            if (client_id_ != 0) decision.request = select(Kind::start);
        }
        decision.rearm = watchdog_.update(client_id_, input.generation, input.now,
                                          std::chrono::milliseconds{2000});
        if (decision.rearm != server::StreamRearm::none)
            decision.request = select(Kind::rearm);
        return decision;
    }

    [[nodiscard]] Outcome complete(const Request& request, bool sent) noexcept {
        if (request.client_id == 0 || request != pending_) return Outcome::none;
        pending_ = {};
        if (request.owner != Owner::focus) return Outcome::none;
        if (request.kind == Kind::start)
            return sent ? Outcome::start_sent : Outcome::start_failed;
        return sent ? Outcome::rearm_sent : Outcome::rearm_failed;
    }
    [[nodiscard]] std::uint64_t client_id() const noexcept { return client_id_; }
    [[nodiscard]] Owner owner() const noexcept { return owner_; }

private:
    [[nodiscard]] Request select(Kind kind) noexcept {
        pending_ = {client_id_, owner_, kind, ++serial_};
        return pending_;
    }
    std::uint64_t client_id_ = 0, serial_ = 0;
    Owner owner_ = Owner::none;
    Request pending_;
    server::StreamRearmWatchdog watchdog_;
};

} // namespace nstu::ui
