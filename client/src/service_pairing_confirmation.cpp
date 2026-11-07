#include "nstu/service_pairing_confirmation.hpp"
#include "nstu/agent_pairing_confirmation.hpp"

#include "nstu/auth.hpp"

#include <windows.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace nstu::client {
namespace {

bool six_digits(std::string_view code) noexcept {
    return code.size() == 6 && std::all_of(code.begin(), code.end(),
        [](char digit) { return digit >= '0' && digit <= '9'; });
}

StudentConfirmationChoice student_choice(AgentPairingConfirmationChoice choice) noexcept {
    switch (choice) {
    case AgentPairingConfirmationChoice::confirmed: return StudentConfirmationChoice::confirmed;
    case AgentPairingConfirmationChoice::rejected:
    case AgentPairingConfirmationChoice::cancelled: return StudentConfirmationChoice::rejected;
    case AgentPairingConfirmationChoice::mismatched_code: return StudentConfirmationChoice::mismatched_code;
    case AgentPairingConfirmationChoice::timed_out: return StudentConfirmationChoice::timed_out;
    case AgentPairingConfirmationChoice::no_ui: return StudentConfirmationChoice::no_ui;
    }
    return StudentConfirmationChoice::rejected;
}

} // namespace

bool AgentPairingConfirmation::receive(const AgentPairingConfirmationRequest& request,
                                       std::uint64_t now_ms) {
    disconnect();
    if (request.expires_at_ms <= now_ms ||
        request.expires_at_ms - now_ms > kMaximumPairingConfirmationMs ||
        encode_agent_pairing_confirmation_request(request).empty()) return false;
    request_ = request;
    return true;
}

bool AgentPairingConfirmation::mark_presented(const AgentPairingAttemptToken& token,
                                              std::uint64_t now_ms) noexcept {
    if (!request_ || request_->attempt_token != token || request_->expires_at_ms <= now_ms)
        return false;
    presented_ = true;
    return true;
}

std::optional<AgentPairingConfirmationReply> AgentPairingConfirmation::choose(
    const AgentPairingAttemptToken& token, AgentPairingConfirmationChoice choice,
    std::string_view entered_teacher_code, std::uint64_t now_ms) {
    if (!request_ || request_->attempt_token != token) return std::nullopt;
    std::string entered;
    if (request_->expires_at_ms <= now_ms) choice = AgentPairingConfirmationChoice::timed_out;
    else if (choice == AgentPairingConfirmationChoice::confirmed) {
        if (!presented_) choice = AgentPairingConfirmationChoice::no_ui;
        else if (!six_digits(entered_teacher_code) || entered_teacher_code != request_->code)
            choice = AgentPairingConfirmationChoice::mismatched_code;
        if (presented_ && six_digits(entered_teacher_code)) entered = entered_teacher_code;
    }
    AgentPairingConfirmationReply reply{token, choice, presented_, std::move(entered)};
    disconnect();
    // Invalid enum values from a caller must never produce affirmative IPC.
    if (encode_agent_pairing_confirmation_reply(reply).empty()) return std::nullopt;
    return reply;
}

std::optional<AgentPairingConfirmationReply> AgentPairingConfirmation::expire(std::uint64_t now_ms) {
    if (!request_ || request_->expires_at_ms > now_ms) return std::nullopt;
    return choose(request_->attempt_token, AgentPairingConfirmationChoice::timed_out, {}, now_ms);
}

bool AgentPairingConfirmation::end(const AgentPairingAttemptToken& token) noexcept {
    if (!request_ || request_->attempt_token != token) return false;
    disconnect();
    return true;
}

void AgentPairingConfirmation::disconnect() noexcept {
    request_.reset();
    presented_ = false;
}

PairingAgentPeer ServicePairingConfirmation::connect_agent(std::uint32_t validated_session_id) {
    std::optional<std::stop_source> abort;
    PairingAgentPeer connected;
    {
        std::scoped_lock lock(mutex_);
        abort = confirmed_transport_stop_locked();
        attempt_.reset();
        peer_ = {};
        if (validated_session_id != 0 && generation_ != std::numeric_limits<std::uint64_t>::max())
            peer_ = {validated_session_id, ++generation_};
        connected = peer_;
        signal_.notify_all();
    }
    if (abort) (void)abort->request_stop();
    return connected;
}

void ServicePairingConfirmation::disconnect_agent(PairingAgentPeer peer) {
    std::optional<std::stop_source> abort;
    {
        std::scoped_lock lock(mutex_);
        if (peer != peer_) return;
        abort = confirmed_transport_stop_locked();
        peer_ = {};
        attempt_.reset();
        signal_.notify_all();
    }
    if (abort) (void)abort->request_stop();
}

std::optional<std::stop_source> ServicePairingConfirmation::confirmed_transport_stop_locked() const {
    if (attempt_ && attempt_->decision &&
        attempt_->decision->choice == StudentConfirmationChoice::confirmed)
        return attempt_->transport_stop;
    return std::nullopt;
}

PairingAgentPeer ServicePairingConfirmation::peer() const {
    std::scoped_lock lock(mutex_);
    return peer_;
}

std::optional<AgentPairingConfirmationRequest> ServicePairingConfirmation::begin_attempt(
    PairingAgentPeer peer, std::string_view code, std::string_view server_name,
    std::chrono::milliseconds timeout, std::stop_source transport_stop) {
    if (peer.session_id == 0 || peer.connection_generation == 0) return std::nullopt;
    std::uint64_t admission = 0;
    std::optional<std::stop_source> abort;
    {
        std::scoped_lock lock(mutex_);
        if (peer != peer_) return std::nullopt;
        abort = confirmed_transport_stop_locked();
        attempt_.reset();
        signal_.notify_all();
        if (attempt_generation_ != std::numeric_limits<std::uint64_t>::max())
            admission = ++attempt_generation_;
    }
    if (abort) (void)abort->request_stop();
    if (admission == 0 || timeout.count() <= 0) return std::nullopt;
    const auto duration = static_cast<std::uint64_t>(std::min<std::int64_t>(
        timeout.count(), static_cast<std::int64_t>(kMaximumPairingConfirmationMs)));
    const auto now_ms = GetTickCount64();
    if (now_ms > std::numeric_limits<std::uint64_t>::max() - duration) return std::nullopt;
    AgentPairingConfirmationRequest request{{}, now_ms + duration,
                                           std::string(code), std::string(server_name)};
    if (!security::generate_random(request.attempt_token) ||
        encode_agent_pairing_confirmation_request(request).empty()) return std::nullopt;
    const auto deadline = Clock::now() + std::chrono::milliseconds(duration);
    std::scoped_lock lock(mutex_);
    if (peer != peer_ || admission != attempt_generation_) return std::nullopt;
    attempt_ = Attempt{request, peer, deadline, std::nullopt, std::move(transport_stop)};
    signal_.notify_all();
    return request;
}

bool ServicePairingConfirmation::receive_reply(PairingAgentPeer peer,
                                               std::span<const std::byte> payload) {
    const auto reply = decode_agent_pairing_confirmation_reply(payload);
    std::unique_lock lock(mutex_);
    if (!attempt_ || peer != peer_ || peer != attempt_->peer)
        return false;
    if (attempt_->decision) {
        // The one affirmative remains revocable during the teacher wait. Only
        // explicit current-peer/current-token cancelled6 can revoke it; no
        // duplicate affirmative, stale reply or malformed payload restores it.
        if (!reply || reply->attempt_token != attempt_->request.attempt_token ||
            reply->choice != AgentPairingConfirmationChoice::cancelled ||
            attempt_->decision->choice != StudentConfirmationChoice::confirmed)
            return false;
        auto abort = attempt_->transport_stop;
        attempt_->decision = StudentConfirmation{StudentConfirmationChoice::rejected, {}};
        signal_.notify_all();
        lock.unlock();
        (void)abort.request_stop();
        return true;
    }
    if (Clock::now() >= attempt_->deadline || GetTickCount64() >= attempt_->request.expires_at_ms) {
        attempt_->decision = StudentConfirmation{StudentConfirmationChoice::timed_out, {}};
        signal_.notify_all();
        return false;
    }
    if (!reply) {
        attempt_->decision = StudentConfirmation{StudentConfirmationChoice::rejected, {}};
        signal_.notify_all();
        return false;
    }
    if (reply->attempt_token != attempt_->request.attempt_token) return false;
    auto choice = student_choice(reply->choice);
    if (choice == StudentConfirmationChoice::confirmed &&
        evaluate_student_confirmation(reply->code_shown, choice, attempt_->request.code,
                                      reply->confirmed_code) != StudentConfirmationDecision::save)
        choice = StudentConfirmationChoice::mismatched_code;
    attempt_->decision = StudentConfirmation{choice, reply->confirmed_code};
    signal_.notify_all();
    return true;
}

StudentConfirmation ServicePairingConfirmation::wait(const AgentPairingAttemptToken& token,
                                                     Clock::time_point slice_deadline,
                                                     std::stop_token stop_token) {
    std::stop_callback wake(stop_token, [this] { signal_.notify_all(); });
    std::unique_lock lock(mutex_);
    const auto bounded_slice = std::min(slice_deadline, Clock::now() + std::chrono::milliseconds(250));
    for (;;) {
        if (!attempt_ || token != attempt_->request.attempt_token || attempt_->peer != peer_)
            return {StudentConfirmationChoice::no_ui, {}};
        if (stop_token.stop_requested()) {
            attempt_->decision = StudentConfirmation{StudentConfirmationChoice::rejected, {}};
            return *attempt_->decision;
        }
        const auto now = Clock::now();
        if (now >= attempt_->deadline || GetTickCount64() >= attempt_->request.expires_at_ms) {
            attempt_->decision = StudentConfirmation{StudentConfirmationChoice::timed_out, {}};
            return *attempt_->decision;
        }
        if (attempt_->decision) return *attempt_->decision;
        if (now >= bounded_slice) return {StudentConfirmationChoice::waiting, {}};
        signal_.wait_until(lock, std::min(bounded_slice, attempt_->deadline));
    }
}

void ServicePairingConfirmation::cancel_attempt(const AgentPairingAttemptToken& token) {
    std::optional<std::stop_source> abort;
    {
        std::scoped_lock lock(mutex_);
        if (!attempt_ || token != attempt_->request.attempt_token) return;
        abort = confirmed_transport_stop_locked();
        attempt_->decision = StudentConfirmation{StudentConfirmationChoice::rejected, {}};
        signal_.notify_all();
    }
    if (abort) (void)abort->request_stop();
}

bool ServicePairingConfirmation::end_attempt(const AgentPairingAttemptToken& token) {
    std::optional<std::stop_source> abort;
    {
        std::scoped_lock lock(mutex_);
        if (!attempt_ || token != attempt_->request.attempt_token) return false;
        abort = confirmed_transport_stop_locked();
        attempt_.reset();
        signal_.notify_all();
    }
    if (abort) (void)abort->request_stop();
    return true;
}

bool ServicePairingConfirmation::is_current_request(PairingAgentPeer peer,
                                                   const AgentPairingAttemptToken& token) const {
    std::scoped_lock lock(mutex_);
    return attempt_ && peer == peer_ && peer == attempt_->peer && !attempt_->decision &&
           token == attempt_->request.attempt_token && Clock::now() < attempt_->deadline &&
           GetTickCount64() < attempt_->request.expires_at_ms;
}

StudentConfirmation ServicePairingConfirmation::finish_attempt(const AgentPairingAttemptToken& token,
                                                               std::stop_token stop_token) {
    std::scoped_lock lock(mutex_);
    if (!attempt_ || token != attempt_->request.attempt_token || attempt_->peer != peer_)
        return {StudentConfirmationChoice::no_ui, {}};
    auto decision = attempt_->decision.value_or(StudentConfirmation{StudentConfirmationChoice::no_ui, {}});
    if (Clock::now() >= attempt_->deadline || GetTickCount64() >= attempt_->request.expires_at_ms)
        decision = {StudentConfirmationChoice::timed_out, {}};
    if (stop_token.stop_requested()) decision = {StudentConfirmationChoice::rejected, {}};
    attempt_.reset();
    signal_.notify_all();
    return decision;
}

namespace {
std::optional<AgentPairingAttemptToken> message_token(const AgentMessage& message) {
    if (message.type == AgentMessageType::pairing_confirmation_request) {
        const auto request = decode_agent_pairing_confirmation_request(message.payload);
        if (request) return request->attempt_token;
    } else if (message.type == AgentMessageType::pairing_confirmation_reply) {
        const auto reply = decode_agent_pairing_confirmation_reply(message.payload);
        if (reply) return reply->attempt_token;
    } else if (message.type == AgentMessageType::pairing_confirmation_end) {
        return decode_agent_pairing_confirmation_end(message.payload);
    }
    return std::nullopt;
}

bool native_controls_usable(const PairingNativeControls& controls) noexcept {
    if (!IsWindowVisible(controls.window) || !IsWindowEnabled(controls.window) ||
        GetWindowThreadProcessId(controls.window, nullptr) != GetCurrentThreadId()) return false;
    for (const auto child : {controls.teacher_input, controls.match, controls.differ, controls.reject}) {
        if (!IsChild(controls.window, child) || !IsWindowVisible(child) || !IsWindowEnabled(child))
            return false;
    }
    return true;
}
} // namespace

void discard_pairing_confirmation_messages(std::deque<AgentMessage>& queue,
    const std::optional<AgentPairingAttemptToken>& token) {
    std::erase_if(queue, [&](const AgentMessage& message) {
        const bool pairing = message.type == AgentMessageType::pairing_confirmation_request ||
            message.type == AgentMessageType::pairing_confirmation_reply ||
            message.type == AgentMessageType::pairing_confirmation_end;
        return pairing && (!token || message_token(message) == token);
    });
}

bool pairing_agent_message_current(const ServicePairingConfirmation& broker,
    PairingAgentPeer peer, const AgentMessage& message) {
    if (peer.session_id == 0 || peer.connection_generation == 0 || broker.peer() != peer)
        return false;
    const auto token = message_token(message);
    if (!token) return false;
    if (message.type == AgentMessageType::pairing_confirmation_request)
        return broker.is_current_request(peer, *token);
    // End remains deliverable after the final broker consumption. Replacement
    // connections purge queued pairing traffic, and the agent compares tokens.
    return message.type == AgentMessageType::pairing_confirmation_end;
}

bool enqueue_pairing_agent_message(const ServicePairingConfirmation& broker,
    PairingAgentPeer peer, std::deque<AgentMessage>& queue, std::size_t capacity,
    AgentMessage message) {
    if (!pairing_agent_message_current(broker, peer, message)) return false;
    const auto token = message_token(message);
    if (message.type == AgentMessageType::pairing_confirmation_request)
        discard_pairing_confirmation_messages(queue);
    else discard_pairing_confirmation_messages(queue, token);
    if (queue.size() >= capacity) return false;
    queue.push_back(std::move(message));
    return true;
}

bool AgentPairingNativeConfirmation::receive(const AgentPairingConfirmationRequest& request,
                                             std::uint64_t now_ms) {
    disconnect();
    if (!model_.receive(request, now_ms)) return false;
    visible_request_ = request;
    return true;
}

bool reset_pairing_native_input(HWND teacher_input, const AgentPairingAttemptToken& token,
    std::optional<AgentPairingAttemptToken>& displayed_token) {
    // The old token loses authority before the synchronous native reset can
    // notify the parent. A returned success alone is not empty-input proof.
    displayed_token.reset();
    const auto live_input = [teacher_input] {
        return IsWindow(teacher_input) &&
            GetWindowThreadProcessId(teacher_input, nullptr) == GetCurrentThreadId();
    };
    if (!live_input() || !SetWindowTextW(teacher_input, L"") || !live_input()) return false;
    SetLastError(ERROR_SUCCESS);
    const auto length = GetWindowTextLengthW(teacher_input);
    if (length != 0 || GetLastError() != ERROR_SUCCESS) return false;
    wchar_t text[2]{L'?', L'?'};
    SetLastError(ERROR_SUCCESS);
    if (GetWindowTextW(teacher_input, text, 2) != 0 || text[0] != L'\0' ||
        GetLastError() != ERROR_SUCCESS || !live_input()) return false;
    displayed_token = token;
    return true;
}

bool AgentPairingNativeConfirmation::present(const PairingNativeControls& controls,
    const AgentPairingAttemptToken& token, std::string_view rendered_code,
    bool paint_succeeded, std::uint64_t now_ms) {
    return visible_request_ && !confirmed_ && visible_request_->attempt_token == token &&
        controls.displayed_token == token &&
        visible_request_->code == rendered_code && paint_succeeded && native_controls_usable(controls) &&
        model_.mark_presented(token, now_ms);
}

std::optional<AgentPairingConfirmationReply> AgentPairingNativeConfirmation::choose(
    const PairingNativeControls& controls, const AgentPairingAttemptToken& token,
    AgentPairingConfirmationChoice choice, std::uint64_t now_ms) {
    if (!visible_request_ || visible_request_->attempt_token != token) return std::nullopt;
    if (confirmed_) {
        if (choice != AgentPairingConfirmationChoice::cancelled &&
            choice != AgentPairingConfirmationChoice::rejected &&
            choice != AgentPairingConfirmationChoice::timed_out) return std::nullopt;
        AgentPairingConfirmationReply reply{token, AgentPairingConfirmationChoice::cancelled, true, {}};
        disconnect();
        return reply;
    }
    std::string entered;
    if (choice == AgentPairingConfirmationChoice::confirmed) {
        if (controls.displayed_token != token || !native_controls_usable(controls))
            choice = AgentPairingConfirmationChoice::no_ui;
        else {
            wchar_t text[8]{};
            const int length = GetWindowTextW(controls.teacher_input, text, static_cast<int>(std::size(text)));
            if (length == 6 && GetWindowTextLengthW(controls.teacher_input) == 6) {
                for (int index = 0; index < length; ++index) {
                    if (text[index] < L'0' || text[index] > L'9') { entered.clear(); break; }
                    entered.push_back(static_cast<char>(text[index]));
                }
            }
        }
    }
    auto reply = model_.choose(token, choice, entered, now_ms);
    if (reply && reply->choice == AgentPairingConfirmationChoice::confirmed) confirmed_ = true;
    else disconnect();
    return reply;
}

std::optional<AgentPairingConfirmationReply> AgentPairingNativeConfirmation::expire(std::uint64_t now_ms) {
    if (!visible_request_ || visible_request_->expires_at_ms > now_ms) return std::nullopt;
    return choose({}, visible_request_->attempt_token, AgentPairingConfirmationChoice::timed_out, now_ms);
}

bool AgentPairingNativeConfirmation::end(const AgentPairingAttemptToken& token) noexcept {
    if (!visible_request_ || visible_request_->attempt_token != token) return false;
    disconnect();
    return true;
}

void AgentPairingNativeConfirmation::disconnect() noexcept {
    model_.disconnect();
    visible_request_.reset();
    confirmed_ = false;
}

StudentConfirmation finish_interactive_confirmation(
    ServicePairingConfirmation& broker, const AgentPairingAttemptToken& token,
    const PairingAgentMessageSink& sink, std::stop_token stop_token) {
    bool queued = false;
    try {
        queued = sink && sink({AgentMessageType::pairing_confirmation_end,
                               encode_agent_pairing_confirmation_end(token)});
    } catch (...) {
        // A failed cleanup enqueue is a refusal, not an affirmative handoff.
    }
    if (!queued) broker.cancel_attempt(token);
    // The broker-lock consumption is the handoff linearization point after
    // external cleanup. A later asynchronous cancellation or connection change
    // is an ensuing recovery event, not a promise of indefinite connection.
    auto final = broker.finish_attempt(token, stop_token);
    if (stop_token.stop_requested()) final = {StudentConfirmationChoice::rejected, {}};
    return final;
}

PairingAttemptResult pair_with_interactive_agent(
    const discovery::PairingCandidate& candidate, std::string_view client_uuid,
    std::string_view hostname, ServicePairingConfirmation& broker,
    const PairingAgentMessageSink& sink, std::stop_token stop_token,
    const PairingAttemptOptions& timing, std::string* error) {
    PairingAttemptResult result;
    const auto peer = broker.peer();
    if (!sink || peer.session_id == 0 || peer.connection_generation == 0) {
        result.outcome = PairingOutcome::student_refused;
        if (error) *error = "student confirmation unavailable";
        return result;
    }
    std::stop_source transport_stop;
    std::stop_callback relay(stop_token, [transport_stop]() mutable {
        (void)transport_stop.request_stop();
    });
    std::optional<AgentPairingConfirmationRequest> request;
    try {
        auto options = timing;
        // Clamp before converting seconds to milliseconds, avoiding duration
        // overflow even when the caller supplies an extreme timeout.
        const auto seconds = std::clamp<std::int64_t>(timing.approval_timeout.count(), 0, 180);
        const auto timeout = std::chrono::milliseconds(seconds * 1000);
        options.approval_timeout = std::chrono::seconds(seconds);
        options.student_confirmation = [&](const std::string& code, const std::string& name,
                                            ServicePairingConfirmation::Clock::time_point deadline,
                                            std::stop_token token) {
            if (!request || code != request->code || name != request->server_name)
                return StudentConfirmation{StudentConfirmationChoice::no_ui, {}};
            return broker.wait(request->attempt_token, deadline, token);
        };
        result = pair_with_server(candidate, client_uuid, hostname,
            [&](const std::string& code, const std::string& name) {
                request = broker.begin_attempt(peer, code, name, timeout, transport_stop);
                if (request && !sink({AgentMessageType::pairing_confirmation_request,
                                      encode_agent_pairing_confirmation_request(*request)})) {
                    (void)broker.end_attempt(request->attempt_token);
                }
            }, transport_stop.get_token(), options, error);
    } catch (...) {
        if (error) *error = "student confirmation could not be completed";
    }
    if (request) {
        const auto final = finish_interactive_confirmation(broker, request->attempt_token, sink,
                                                           transport_stop.get_token());
        if (result.outcome == PairingOutcome::enrolled &&
            (transport_stop.stop_requested() || final.choice != StudentConfirmationChoice::confirmed ||
             final.confirmed_code != request->code)) {
            clear_client_runtime_config(result.config);
            result.outcome = transport_stop.stop_requested() ? PairingOutcome::failed :
                final.choice == StudentConfirmationChoice::timed_out ? PairingOutcome::timed_out
                                                                     : PairingOutcome::student_refused;
            if (error) *error = "student confirmation is no longer valid";
        }
    }
    return result;
}

} // namespace nstu::client
