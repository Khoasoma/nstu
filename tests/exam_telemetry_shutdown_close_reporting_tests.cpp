#include "nstu/exam_telemetry_shutdown_close_reporting.hpp"
#include "nstu/secret_store.hpp"
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using Reporter = nstu::client::ExamTelemetryShutdownCloseReporter;
using Disposition = nstu::client::TelemetryShutdownCloseReportDisposition;
using Result = nstu::client::TelemetryShutdownCloseReportResult;
using Snapshot = nstu::client::TelemetryShutdownDispatchResult;
using Dispatch = nstu::client::TelemetryShutdownDispatch;
using Pair = nstu::client::TelemetryShutdownCloseOutcome;
using Shutdown = nstu::client::TelemetryShutdownOutcome;
using Close = nstu::client::TelemetryCloseOutcome;
using State = nstu::client::TelemetryDisconnectState;
using Pending = nstu::client::TelemetryDisconnectPending;
using Publication = nstu::client::TelemetryDisconnectPublication;
using Cleanup = nstu::client::TelemetryDisconnectCleanup;
using Error = nstu::client::TelemetryDisconnectError;
using Router = nstu::client::ExamTelemetryRouter;
using Coordinator = nstu::client::ExamTelemetryServiceCoordinator;

static_assert(noexcept(std::declval<Reporter&>().report(std::declval<const Snapshot&>())));
static_assert(!std::is_copy_constructible_v<Reporter> && !std::is_copy_assignable_v<Reporter>);
static_assert(!std::is_move_constructible_v<Reporter> && !std::is_move_assignable_v<Reporter>);
static_assert(std::is_same_v<decltype(std::declval<Reporter&>().report(std::declval<const Snapshot&>())), Result>);

Snapshot typed(Pair pair) { return {Dispatch::outcome_callback_invoked, pair}; }
Pair failure_pair() {
    return {{State::enabled, State::suspended, Pending::retained,
             Publication::current_protected_state_confirmed, Cleanup::not_needed, Error::pending_enqueue_unfinished},
            {State::suspended, State::suspended, Pending::retained,
             Publication::not_attempted, Cleanup::not_needed, Error::pending_enqueue_unfinished}};
}
Pair complete_pair() {
    return {{State::enabled, State::suspended, Pending::not_present,
             Publication::current_protected_state_confirmed, Cleanup::not_needed, Error::none},
            {State::suspended, State::closed, Pending::not_present,
             Publication::not_attempted, Cleanup::not_needed, Error::none}};
}
const std::string kShutdownFailure = "dispatch=outcome_callback_invoked before=enabled after=suspended pending=retained "
    "p=current_protected_state_confirmed cleanup=not_needed error=pending_enqueue_unfinished";
const std::string kCloseFailure = "dispatch=outcome_callback_invoked before=suspended after=suspended pending=retained "
    "p=not_attempted cleanup=not_needed error=pending_enqueue_unfinished";

void dispositions(Result value, Disposition shutdown, Disposition close) {
    assert(value.shutdown == shutdown && value.close == close);
}
struct Capture {
    nstu::audit::Spool spool;
    std::vector<nstu::audit::Event> events;
    std::vector<std::string> debug;
    std::vector<char> order;
    Reporter reporter;
    Capture() : reporter([this](const nstu::audit::Event& event) {
        order.push_back('a'); events.push_back(event); return spool.emit(event);
    }, [this](const std::string& line) { order.push_back('d'); debug.push_back(line); }) {}
    void notice(std::size_t index, const std::string& action, const std::string& detail,
                const std::string& result = "failed") const {
        const auto records = spool.peek_chunk();
        const auto decoded = nstu::audit::decode_audit_chunk(nstu::audit::encode_audit_chunk(records));
        assert(decoded && decoded->size() == records.size());
        assert(index < records.size() && index < events.size() && index < debug.size());
        const auto& event = (*decoded)[index].event;
        assert(event.category == nstu::audit::Category::exam && event.severity == nstu::audit::Severity::warning);
        assert(event.component == "exam_telemetry" && event.action == action && event.result == result);
        assert(event.operation_id == 0 && event.client_id == 0 && event.detail == detail);
        assert(event.detail.size() <= 191 && event.detail.find("<redacted>") == std::string::npos);
        assert(events[index].action == action && events[index].result == result && events[index].detail == detail);
        assert(events[index].category == event.category && events[index].severity == event.severity);
        assert(events[index].component == event.component && events[index].operation_id == 0 && events[index].client_id == 0);
        const auto prefix = action == "shutdown_dispatch" ? "shutdown dispatch" : action;
        assert(debug[index] == "NSTU exam telemetry " + prefix + ": " + result + " " + detail + "\n");
        assert(std::count(debug[index].begin(), debug[index].end(), '\n') == 1);
        assert(nstu::audit::sanitize_event(events[index]).detail == detail);
        for (std::size_t i = 0; i < records.size(); ++i) {
            assert((*decoded)[i].sequence == records[i].sequence);
            assert((*decoded)[i].timestamp_unix_milliseconds == records[i].timestamp_unix_milliseconds);
            assert((*decoded)[i].event.detail == records[i].event.detail);
        }
    }
};

void test_interface_dispatch_and_two_notices() {
    bool emitter_rejected = false, debug_rejected = false;
    try { Reporter invalid({}, [](const std::string&) {}); }
    catch (const std::invalid_argument&) { emitter_rejected = true; }
    try { Reporter invalid([](const nstu::audit::Event&) { return std::uint64_t{1}; }, {}); }
    catch (const std::invalid_argument&) { debug_rejected = true; }
    assert(emitter_rejected && debug_rejected);
    Capture capture;
    Router absent;
    dispositions(capture.reporter.report(absent.handle_shutdown_with_outcome()), Disposition::outcome_unavailable, Disposition::outcome_unavailable);
    capture.notice(0, "shutdown_dispatch", "dispatch=not_configured outcome=unavailable", "outcome_unavailable");
    dispositions(capture.reporter.report({Dispatch::not_configured, failure_pair()}), Disposition::outcome_unavailable, Disposition::outcome_unavailable);
    assert(capture.spool.size() == 1);
    dispositions(capture.reporter.report(typed(failure_pair())), Disposition::observed_failure, Disposition::observed_failure);
    capture.notice(1, "shutdown", kShutdownFailure);
    capture.notice(2, "close", kCloseFailure);
    assert((capture.order == std::vector<char>{'d', 'a', 'd', 'a', 'd', 'a'}));
    dispositions(capture.reporter.report(typed(failure_pair())), Disposition::observed_failure, Disposition::observed_failure);
    assert(capture.spool.size() == 3 && capture.debug.size() == 3);
    dispositions(capture.reporter.report(typed(Pair{})), Disposition::no_applicable_work, Disposition::no_applicable_work);
    dispositions(capture.reporter.report(typed(failure_pair())), Disposition::observed_failure, Disposition::observed_failure);
    assert(capture.spool.size() == 3);
    dispositions(capture.reporter.report({Dispatch::not_configured, std::nullopt}), Disposition::outcome_unavailable, Disposition::outcome_unavailable);
    capture.notice(3, "shutdown_dispatch", "dispatch=not_configured outcome=unavailable", "outcome_unavailable");
    auto mixed = failure_pair(); mixed.close = complete_pair().close;
    dispositions(capture.reporter.report(typed(mixed)), Disposition::observed_failure, Disposition::complete);
    assert(capture.spool.size() == 4);
    dispositions(capture.reporter.report(typed(failure_pair())), Disposition::observed_failure, Disposition::observed_failure);
    assert(capture.spool.size() == 5);
    capture.notice(4, "close", kCloseFailure);
    dispositions(capture.reporter.report(typed(complete_pair())), Disposition::complete, Disposition::complete);
    dispositions(capture.reporter.report(typed(failure_pair())), Disposition::observed_failure, Disposition::observed_failure);
    assert(capture.spool.size() == 7);
    capture.notice(5, "shutdown", kShutdownFailure); capture.notice(6, "close", kCloseFailure);
    std::cout << "Interface, separate sanitized notices and independent completion/suppression passed.\n";
}

void test_unavailable_and_unsupported_attachments() {
    Capture capture;
    int calls = 0;
    nstu::client::ExamTelemetryRouterCallbacks callbacks;
    callbacks.on_shutdown = [&] { ++calls; };
    Router legacy(std::move(callbacks));
    dispositions(capture.reporter.report(legacy.handle_shutdown_with_outcome()), Disposition::outcome_unavailable, Disposition::outcome_unavailable);
    assert(calls == 1);
    capture.notice(0, "shutdown_dispatch", "dispatch=legacy_callback_invoked outcome=unavailable", "outcome_unavailable");
    dispositions(capture.reporter.report({Dispatch::legacy_callback_invoked, failure_pair()}), Disposition::outcome_unavailable, Disposition::outcome_unavailable);
    assert(capture.spool.size() == 1);
    dispositions(capture.reporter.report({Dispatch::outcome_callback_invoked, std::nullopt}), Disposition::outcome_unavailable, Disposition::outcome_unavailable);
    capture.notice(1, "shutdown_dispatch", "dispatch=outcome_callback_invoked outcome=unavailable", "outcome_unavailable");
    const auto unknown = static_cast<Dispatch>(255);
    dispositions(capture.reporter.report({unknown, std::nullopt}), Disposition::observed_failure, Disposition::observed_failure);
    capture.notice(2, "shutdown_dispatch", "dispatch=unknown outcome=unavailable");
    dispositions(capture.reporter.report({unknown, failure_pair()}), Disposition::observed_failure, Disposition::observed_failure);
    assert(capture.spool.size() == 3 && capture.debug.size() == 3);
    dispositions(capture.reporter.report(typed(Pair{})), Disposition::no_applicable_work, Disposition::no_applicable_work);
    dispositions(capture.reporter.report({unknown, Pair{}}), Disposition::observed_failure, Disposition::observed_failure);
    capture.notice(3, "shutdown_dispatch", "dispatch=unknown outcome=unavailable");
    std::cout << "Actual legacy dispatch, ignored attachments and availability recovery passed.\n";
}

void test_literal_sanitized_vocabulary() {
    struct Row { Shutdown value; const char* detail; };
    // Supplied enum controls, deliberately failed by witness/error/open facts.
    const std::array<Row, 29> rows{{
        {{State::closed, State::suspended, Pending::revoked_age_uncertain, Publication::earlier_protected_snapshot_confirmed, Cleanup::not_needed, Error::revocation_witness_unconfirmed}, "dispatch=outcome_callback_invoked before=closed after=suspended pending=revoked_age_uncertain p=earlier_protected_snapshot_confirmed cleanup=not_needed error=revocation_witness_unconfirmed"},
        {{State::disabled, State::suspended, Pending::revoked_age_uncertain, Publication::earlier_protected_snapshot_confirmed, Cleanup::not_needed, Error::revocation_witness_unconfirmed}, "dispatch=outcome_callback_invoked before=disabled after=suspended pending=revoked_age_uncertain p=earlier_protected_snapshot_confirmed cleanup=not_needed error=revocation_witness_unconfirmed"},
        {{State::enabled, State::suspended, Pending::revoked_age_uncertain, Publication::earlier_protected_snapshot_confirmed, Cleanup::not_needed, Error::revocation_witness_unconfirmed}, "dispatch=outcome_callback_invoked before=enabled after=suspended pending=revoked_age_uncertain p=earlier_protected_snapshot_confirmed cleanup=not_needed error=revocation_witness_unconfirmed"},
        {{State::suspended, State::suspended, Pending::revoked_age_uncertain, Publication::earlier_protected_snapshot_confirmed, Cleanup::not_needed, Error::revocation_witness_unconfirmed}, "dispatch=outcome_callback_invoked before=suspended after=suspended pending=revoked_age_uncertain p=earlier_protected_snapshot_confirmed cleanup=not_needed error=revocation_witness_unconfirmed"},
        {{State::revoked, State::suspended, Pending::revoked_age_uncertain, Publication::earlier_protected_snapshot_confirmed, Cleanup::not_needed, Error::revocation_witness_unconfirmed}, "dispatch=outcome_callback_invoked before=revoked after=suspended pending=revoked_age_uncertain p=earlier_protected_snapshot_confirmed cleanup=not_needed error=revocation_witness_unconfirmed"},
        {{State::suspended, State::closed, Pending::revoked_age_uncertain, Publication::earlier_protected_snapshot_confirmed, Cleanup::not_needed, Error::revocation_witness_unconfirmed}, "dispatch=outcome_callback_invoked before=suspended after=closed pending=revoked_age_uncertain p=earlier_protected_snapshot_confirmed cleanup=not_needed error=revocation_witness_unconfirmed"},
        {{State::suspended, State::disabled, Pending::revoked_age_uncertain, Publication::earlier_protected_snapshot_confirmed, Cleanup::not_needed, Error::revocation_witness_unconfirmed}, "dispatch=outcome_callback_invoked before=suspended after=disabled pending=revoked_age_uncertain p=earlier_protected_snapshot_confirmed cleanup=not_needed error=revocation_witness_unconfirmed"},
        {{State::suspended, State::enabled, Pending::revoked_age_uncertain, Publication::earlier_protected_snapshot_confirmed, Cleanup::not_needed, Error::revocation_witness_unconfirmed}, "dispatch=outcome_callback_invoked before=suspended after=enabled pending=revoked_age_uncertain p=earlier_protected_snapshot_confirmed cleanup=not_needed error=revocation_witness_unconfirmed"},
        {{State::suspended, State::suspended, Pending::revoked_age_uncertain, Publication::earlier_protected_snapshot_confirmed, Cleanup::not_needed, Error::revocation_witness_unconfirmed}, "dispatch=outcome_callback_invoked before=suspended after=suspended pending=revoked_age_uncertain p=earlier_protected_snapshot_confirmed cleanup=not_needed error=revocation_witness_unconfirmed"},
        {{State::suspended, State::revoked, Pending::revoked_age_uncertain, Publication::earlier_protected_snapshot_confirmed, Cleanup::not_needed, Error::revocation_witness_unconfirmed}, "dispatch=outcome_callback_invoked before=suspended after=revoked pending=revoked_age_uncertain p=earlier_protected_snapshot_confirmed cleanup=not_needed error=revocation_witness_unconfirmed"},
        {{State::suspended, State::suspended, Pending::not_present, Publication::earlier_protected_snapshot_confirmed, Cleanup::not_needed, Error::revocation_witness_unconfirmed}, "dispatch=outcome_callback_invoked before=suspended after=suspended pending=not_present p=earlier_protected_snapshot_confirmed cleanup=not_needed error=revocation_witness_unconfirmed"},
        {{State::suspended, State::suspended, Pending::retained, Publication::earlier_protected_snapshot_confirmed, Cleanup::not_needed, Error::revocation_witness_unconfirmed}, "dispatch=outcome_callback_invoked before=suspended after=suspended pending=retained p=earlier_protected_snapshot_confirmed cleanup=not_needed error=revocation_witness_unconfirmed"},
        {{State::suspended, State::suspended, Pending::completed, Publication::earlier_protected_snapshot_confirmed, Cleanup::not_needed, Error::revocation_witness_unconfirmed}, "dispatch=outcome_callback_invoked before=suspended after=suspended pending=completed p=earlier_protected_snapshot_confirmed cleanup=not_needed error=revocation_witness_unconfirmed"},
        {{State::suspended, State::suspended, Pending::revoked_expired, Publication::earlier_protected_snapshot_confirmed, Cleanup::not_needed, Error::revocation_witness_unconfirmed}, "dispatch=outcome_callback_invoked before=suspended after=suspended pending=revoked_expired p=earlier_protected_snapshot_confirmed cleanup=not_needed error=revocation_witness_unconfirmed"},
        {{State::suspended, State::suspended, Pending::revoked_age_uncertain, Publication::earlier_protected_snapshot_confirmed, Cleanup::not_needed, Error::revocation_witness_unconfirmed}, "dispatch=outcome_callback_invoked before=suspended after=suspended pending=revoked_age_uncertain p=earlier_protected_snapshot_confirmed cleanup=not_needed error=revocation_witness_unconfirmed"},
        {{State::suspended, State::suspended, Pending::revoked_age_uncertain, Publication::not_attempted, Cleanup::not_needed, Error::revocation_witness_unconfirmed}, "dispatch=outcome_callback_invoked before=suspended after=suspended pending=revoked_age_uncertain p=not_attempted cleanup=not_needed error=revocation_witness_unconfirmed"},
        {{State::suspended, State::suspended, Pending::revoked_age_uncertain, Publication::current_protected_state_confirmed, Cleanup::not_needed, Error::revocation_witness_unconfirmed}, "dispatch=outcome_callback_invoked before=suspended after=suspended pending=revoked_age_uncertain p=current_protected_state_confirmed cleanup=not_needed error=revocation_witness_unconfirmed"},
        {{State::suspended, State::suspended, Pending::revoked_age_uncertain, Publication::earlier_protected_snapshot_confirmed, Cleanup::not_needed, Error::revocation_witness_unconfirmed}, "dispatch=outcome_callback_invoked before=suspended after=suspended pending=revoked_age_uncertain p=earlier_protected_snapshot_confirmed cleanup=not_needed error=revocation_witness_unconfirmed"},
        {{State::suspended, State::suspended, Pending::revoked_age_uncertain, Publication::refusal_marker_confirmed, Cleanup::not_needed, Error::revocation_witness_unconfirmed}, "dispatch=outcome_callback_invoked before=suspended after=suspended pending=revoked_age_uncertain p=refusal_marker_confirmed cleanup=not_needed error=revocation_witness_unconfirmed"},
        {{State::suspended, State::suspended, Pending::revoked_age_uncertain, Publication::unconfirmed, Cleanup::not_needed, Error::revocation_witness_unconfirmed}, "dispatch=outcome_callback_invoked before=suspended after=suspended pending=revoked_age_uncertain p=unconfirmed cleanup=not_needed error=revocation_witness_unconfirmed"},
        {{State::suspended, State::suspended, Pending::revoked_age_uncertain, Publication::earlier_protected_snapshot_confirmed, Cleanup::not_needed, Error::revocation_witness_unconfirmed}, "dispatch=outcome_callback_invoked before=suspended after=suspended pending=revoked_age_uncertain p=earlier_protected_snapshot_confirmed cleanup=not_needed error=revocation_witness_unconfirmed"},
        {{State::suspended, State::suspended, Pending::revoked_age_uncertain, Publication::earlier_protected_snapshot_confirmed, Cleanup::complete, Error::revocation_witness_unconfirmed}, "dispatch=outcome_callback_invoked before=suspended after=suspended pending=revoked_age_uncertain p=earlier_protected_snapshot_confirmed cleanup=complete error=revocation_witness_unconfirmed"},
        {{State::suspended, State::suspended, Pending::revoked_age_uncertain, Publication::earlier_protected_snapshot_confirmed, Cleanup::pending, Error::revocation_witness_unconfirmed}, "dispatch=outcome_callback_invoked before=suspended after=suspended pending=revoked_age_uncertain p=earlier_protected_snapshot_confirmed cleanup=pending error=revocation_witness_unconfirmed"},
        {{State::suspended, State::suspended, Pending::revoked_age_uncertain, Publication::earlier_protected_snapshot_confirmed, Cleanup::not_needed, Error::none}, "dispatch=outcome_callback_invoked before=suspended after=suspended pending=revoked_age_uncertain p=earlier_protected_snapshot_confirmed cleanup=not_needed error=none"},
        {{State::suspended, State::suspended, Pending::revoked_age_uncertain, Publication::earlier_protected_snapshot_confirmed, Cleanup::not_needed, Error::protected_state_failed}, "dispatch=outcome_callback_invoked before=suspended after=suspended pending=revoked_age_uncertain p=earlier_protected_snapshot_confirmed cleanup=not_needed error=protected_state_failed"},
        {{State::suspended, State::suspended, Pending::revoked_age_uncertain, Publication::earlier_protected_snapshot_confirmed, Cleanup::not_needed, Error::revocation_witness_unconfirmed}, "dispatch=outcome_callback_invoked before=suspended after=suspended pending=revoked_age_uncertain p=earlier_protected_snapshot_confirmed cleanup=not_needed error=revocation_witness_unconfirmed"},
        {{State::suspended, State::suspended, Pending::revoked_age_uncertain, Publication::earlier_protected_snapshot_confirmed, Cleanup::not_needed, Error::cleanup_failed}, "dispatch=outcome_callback_invoked before=suspended after=suspended pending=revoked_age_uncertain p=earlier_protected_snapshot_confirmed cleanup=not_needed error=cleanup_failed"},
        {{State::suspended, State::suspended, Pending::revoked_age_uncertain, Publication::earlier_protected_snapshot_confirmed, Cleanup::not_needed, Error::pending_conflict}, "dispatch=outcome_callback_invoked before=suspended after=suspended pending=revoked_age_uncertain p=earlier_protected_snapshot_confirmed cleanup=not_needed error=pending_conflict"},
        {{State::suspended, State::suspended, Pending::revoked_age_uncertain, Publication::earlier_protected_snapshot_confirmed, Cleanup::not_needed, Error::pending_enqueue_unfinished}, "dispatch=outcome_callback_invoked before=suspended after=suspended pending=revoked_age_uncertain p=earlier_protected_snapshot_confirmed cleanup=not_needed error=pending_enqueue_unfinished"}
    }};
    for (const auto& row : rows) {
        Capture capture;
        const auto& s = row.value;
        const Pair pair{s, {s.before, s.after, s.pending, s.publication, s.cleanup, s.current_failure}};
        dispositions(capture.reporter.report(typed(pair)), Disposition::observed_failure, Disposition::observed_failure);
        assert(capture.spool.size() == 2);
        capture.notice(0, "shutdown", row.detail); capture.notice(1, "close", row.detail);
        std::cout << capture.debug[0] << capture.debug[1];
    }
    assert(std::string("p=earlier_protected_snapshot_confirmed").size() == 38);
    std::cout << "All known enum spellings survived literal sanitized Event/debug and real Spool roundtrip.\n";
}

void test_exact_predicates_and_neighbors() {
    const std::array<Shutdown, 10> shutdown_complete{{
        {State::enabled, State::suspended, Pending::not_present, Publication::current_protected_state_confirmed, Cleanup::not_needed, Error::none},
        {State::enabled, State::suspended, Pending::completed, Publication::current_protected_state_confirmed, Cleanup::not_needed, Error::none},
        {State::suspended, State::suspended, Pending::not_present, Publication::current_protected_state_confirmed, Cleanup::not_needed, Error::none},
        {State::suspended, State::suspended, Pending::completed, Publication::current_protected_state_confirmed, Cleanup::not_needed, Error::none},
        {State::revoked, State::revoked, Pending::not_present, Publication::current_protected_state_confirmed, Cleanup::not_needed, Error::none},
        {State::revoked, State::revoked, Pending::not_present, Publication::current_protected_state_confirmed, Cleanup::complete, Error::none},
        {State::enabled, State::revoked, Pending::revoked_expired, Publication::current_protected_state_confirmed, Cleanup::complete, Error::none},
        {State::enabled, State::revoked, Pending::revoked_age_uncertain, Publication::current_protected_state_confirmed, Cleanup::complete, Error::none},
        {State::suspended, State::revoked, Pending::revoked_expired, Publication::current_protected_state_confirmed, Cleanup::complete, Error::none},
        {State::suspended, State::revoked, Pending::revoked_age_uncertain, Publication::current_protected_state_confirmed, Cleanup::complete, Error::none}
    }};
    for (auto value : shutdown_complete) {
        Capture capture;
        dispositions(capture.reporter.report(typed({value, failure_pair().close})), Disposition::complete, Disposition::observed_failure);
        assert(capture.spool.size() == 1); capture.notice(0, "close", kCloseFailure);
        value.current_failure = Error::protected_state_failed;
        dispositions(capture.reporter.report(typed({value, failure_pair().close})), Disposition::observed_failure, Disposition::observed_failure);
        assert(capture.spool.size() == 2); // Only the newly failed shutdown emits.
    }
    const std::array<Close, 12> close_complete{{
        {State::disabled, State::closed, Pending::not_present, Publication::not_attempted, Cleanup::not_needed, Error::none},
        {State::disabled, State::closed, Pending::not_present, Publication::current_protected_state_confirmed, Cleanup::not_needed, Error::none},
        {State::enabled, State::closed, Pending::not_present, Publication::not_attempted, Cleanup::not_needed, Error::none},
        {State::enabled, State::closed, Pending::not_present, Publication::current_protected_state_confirmed, Cleanup::not_needed, Error::none},
        {State::suspended, State::closed, Pending::not_present, Publication::not_attempted, Cleanup::not_needed, Error::none},
        {State::suspended, State::closed, Pending::not_present, Publication::current_protected_state_confirmed, Cleanup::not_needed, Error::none},
        {State::revoked, State::closed, Pending::not_present, Publication::not_attempted, Cleanup::not_needed, Error::none},
        {State::revoked, State::closed, Pending::not_present, Publication::current_protected_state_confirmed, Cleanup::not_needed, Error::none},
        {State::enabled, State::closed, Pending::completed, Publication::current_protected_state_confirmed, Cleanup::not_needed, Error::none},
        {State::suspended, State::closed, Pending::completed, Publication::current_protected_state_confirmed, Cleanup::not_needed, Error::none},
        {State::revoked, State::closed, Pending::not_present, Publication::not_attempted, Cleanup::complete, Error::none},
        {State::revoked, State::closed, Pending::not_present, Publication::current_protected_state_confirmed, Cleanup::complete, Error::none}
    }};
    for (auto value : close_complete) {
        Capture capture;
        dispositions(capture.reporter.report(typed({failure_pair().shutdown, value})), Disposition::observed_failure, Disposition::complete);
        assert(capture.spool.size() == 1); capture.notice(0, "shutdown", kShutdownFailure);
        value.publication = Publication::earlier_protected_snapshot_confirmed;
        dispositions(capture.reporter.report(typed({failure_pair().shutdown, value})), Disposition::observed_failure, Disposition::observed_failure);
        assert(capture.spool.size() == 2); // Close cannot reset unchanged shutdown.
    }
    Capture quiet;
    auto disabled = Pair{};
    disabled.shutdown = {State::disabled, State::disabled, Pending::not_present, Publication::current_protected_state_confirmed, Cleanup::not_needed, Error::none};
    dispositions(quiet.reporter.report(typed(disabled)), Disposition::no_applicable_work, Disposition::no_applicable_work);
    assert(quiet.spool.empty());
    disabled.shutdown.publication = Publication::unconfirmed;
    dispositions(quiet.reporter.report(typed(disabled)), Disposition::observed_failure, Disposition::no_applicable_work);
    quiet.notice(0, "shutdown", "dispatch=outcome_callback_invoked before=disabled after=disabled pending=not_present p=unconfirmed cleanup=not_needed error=none");
    auto mismatch = complete_pair(); mismatch.close.before = State::disabled;
    dispositions(quiet.reporter.report(typed(mismatch)), Disposition::complete, Disposition::complete);
    assert(quiet.spool.size() == 1); // No close.before == shutdown.after inference.
    const std::array<Close, 4> incomplete{{
        {State::enabled, State::closed, Pending::completed, Publication::not_attempted, Cleanup::not_needed, Error::none},
        {State::closed, State::closed, Pending::not_present, Publication::current_protected_state_confirmed, Cleanup::not_needed, Error::none},
        {State::enabled, State::closed, Pending::not_present, Publication::not_attempted, Cleanup::pending, Error::none},
        // Supplied clean-close expiry/remained-open control; native dependency is
        // lifecycle-close-outcome-1 at a93a6dc. Not a native paired occurrence.
        {State::enabled, State::revoked, Pending::revoked_expired, Publication::current_protected_state_confirmed, Cleanup::complete, Error::none}
    }};
    const std::array<const char*, 4> details{{
        "dispatch=outcome_callback_invoked before=enabled after=closed pending=completed p=not_attempted cleanup=not_needed error=none",
        "dispatch=outcome_callback_invoked before=closed after=closed pending=not_present p=current_protected_state_confirmed cleanup=not_needed error=none",
        "dispatch=outcome_callback_invoked before=enabled after=closed pending=not_present p=not_attempted cleanup=pending error=none",
        "dispatch=outcome_callback_invoked before=enabled after=revoked pending=revoked_expired p=current_protected_state_confirmed cleanup=complete error=none"
    }};
    for (std::size_t i = 0; i < incomplete.size(); ++i) {
        Capture capture;
        dispositions(capture.reporter.report(typed({Shutdown{}, incomplete[i]})), Disposition::no_applicable_work, Disposition::observed_failure);
        capture.notice(0, "close", details[i]); std::cout << capture.debug[0];
    }
    std::cout << "Every exact complete/quiet predicate, neighboring failures and supplied still-open expiry passed.\n";
}

void test_twelve_unsupported_fields() {
    const std::array<const char*, 6> shutdown_details{{
        "dispatch=outcome_callback_invoked before=unknown after=suspended pending=not_present p=current_protected_state_confirmed cleanup=not_needed error=none",
        "dispatch=outcome_callback_invoked before=enabled after=unknown pending=not_present p=current_protected_state_confirmed cleanup=not_needed error=none",
        "dispatch=outcome_callback_invoked before=enabled after=suspended pending=unknown p=current_protected_state_confirmed cleanup=not_needed error=none",
        "dispatch=outcome_callback_invoked before=enabled after=suspended pending=not_present p=unknown cleanup=not_needed error=none",
        "dispatch=outcome_callback_invoked before=enabled after=suspended pending=not_present p=current_protected_state_confirmed cleanup=unknown error=none",
        "dispatch=outcome_callback_invoked before=enabled after=suspended pending=not_present p=current_protected_state_confirmed cleanup=not_needed error=unknown"
    }};
    const std::array<const char*, 6> close_details{{
        "dispatch=outcome_callback_invoked before=unknown after=closed pending=not_present p=not_attempted cleanup=not_needed error=none",
        "dispatch=outcome_callback_invoked before=suspended after=unknown pending=not_present p=not_attempted cleanup=not_needed error=none",
        "dispatch=outcome_callback_invoked before=suspended after=closed pending=unknown p=not_attempted cleanup=not_needed error=none",
        "dispatch=outcome_callback_invoked before=suspended after=closed pending=not_present p=unknown cleanup=not_needed error=none",
        "dispatch=outcome_callback_invoked before=suspended after=closed pending=not_present p=not_attempted cleanup=unknown error=none",
        "dispatch=outcome_callback_invoked before=suspended after=closed pending=not_present p=not_attempted cleanup=not_needed error=unknown"
    }};
    for (std::size_t i = 0; i < 6; ++i) {
        const auto unknown = [i](auto& value) {
            switch (i) {
            case 0: value.before = static_cast<State>(255); break;
            case 1: value.after = static_cast<State>(255); break;
            case 2: value.pending = static_cast<Pending>(255); break;
            case 3: value.publication = static_cast<Publication>(255); break;
            case 4: value.cleanup = static_cast<Cleanup>(255); break;
            case 5: value.current_failure = static_cast<Error>(255); break;
            default: assert(false);
            }
        };
        Capture shutdown, close;
        auto s = complete_pair(), c = complete_pair(); unknown(s.shutdown); unknown(c.close);
        dispositions(shutdown.reporter.report(typed(s)), Disposition::observed_failure, Disposition::complete);
        dispositions(close.reporter.report(typed(c)), Disposition::complete, Disposition::observed_failure);
        assert(shutdown.spool.size() == 1 && close.spool.size() == 1);
        shutdown.notice(0, "shutdown", shutdown_details[i]); close.notice(0, "close", close_details[i]);
        std::cout << shutdown.debug[0] << close.debug[0];
    }
    std::cout << "Twelve unsupported fields fail only their own operation and retain fixed unknown text.\n";
}
std::uint64_t now_ms() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}
struct Storage {
    std::filesystem::path root, outbox, state, marker;
    explicit Storage(const std::string& tag) {
        root = std::filesystem::temp_directory_path() / ("nstu-shutdown-close-report-" + tag + "-" +
            std::to_string(GetCurrentProcessId()) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        assert(std::filesystem::equivalent(root.parent_path(), std::filesystem::temp_directory_path()));
        assert(std::filesystem::create_directories(root));
        outbox = root / "telemetry.bin";
        state = outbox.wstring() + L".coord_state";
        marker = state.wstring() + L".blocked";
    }
    ~Storage() { std::error_code error; std::filesystem::remove_all(root, error); assert(!error); }
};
class LockedFile {
public:
    explicit LockedFile(const std::filesystem::path& path)
        : handle_(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)) { assert(handle_ != INVALID_HANDLE_VALUE); }
    ~LockedFile() { assert(CloseHandle(handle_)); }
    LockedFile(const LockedFile&) = delete;
    LockedFile& operator=(const LockedFile&) = delete;
private:
    HANDLE handle_;
};
std::vector<std::byte> raw_file(const std::filesystem::path& path) {
    const auto attributes = GetFileAttributesW(path.c_str());
    assert(attributes != INVALID_FILE_ATTRIBUTES && (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0);
    const auto size = std::filesystem::file_size(path);
    assert(size <= 8192);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    std::ifstream input(path, std::ios::binary);
    assert(input.is_open());
    if (!bytes.empty()) input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    assert(input.good());
    return bytes;
}
std::vector<std::byte> protected_payload(const std::filesystem::path& path, std::string_view entropy) {
    auto bytes = nstu::security::load_machine_secret(path.wstring(),
        {reinterpret_cast<const std::byte*>(entropy.data()), entropy.size()});
    assert(bytes.size() > 32);
    const auto payload = std::span<const std::byte>(bytes).first(bytes.size() - 32);
    const auto hash = nstu::security::sha256(payload);
    assert(hash && std::equal(hash->begin(), hash->end(), bytes.end() - 32));
    bytes.resize(bytes.size() - 32);
    return bytes;
}
std::vector<std::byte> state_payload(const Storage& storage) {
    auto payload = protected_payload(storage.state, "NSTU-Exam-Telemetry-Service-Coordinator-v1");
    assert(payload.size() > 100 && payload[0] == std::byte{0x53} && payload[1] == std::byte{0x4d});
    assert(payload[2] == std::byte{0x4c} && payload[3] == std::byte{0x54} && payload[4] == std::byte{3} && payload[5] == std::byte{0});
    return payload;
}
void physical_empty_outbox(const Storage& storage) {
    const auto payload = protected_payload(storage.outbox, "NSTU-Exam-Telemetry-Outbox-v1");
    const auto u32 = [&](std::size_t offset) {
        assert(offset <= payload.size() && payload.size() - offset >= 4);
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < 4; ++i) value |= std::to_integer<std::uint32_t>(payload[offset + i]) << (8 * i);
        return value;
    };
    assert(u32(0) == 0x544c4d4f && payload[4] == std::byte{2} && payload[5] == std::byte{0});
    assert(payload[6] == std::byte{0} && payload[7] == std::byte{0});
    const auto losses = u32(24);
    assert(losses <= 64);
    const auto records = std::size_t{28} + std::size_t{56} * losses;
    assert(u32(records) == 0 && payload.size() == records + 4);
}
nstu::exam::TelemetryEnable enable_for(std::uint64_t now) {
    nstu::exam::TelemetryEnable enable;
    enable.package_id = "shutdown-close-report-private-package";
    enable.package_digest[0] = std::byte{1}; enable.client_id[0] = std::byte{2}; enable.session_id[0] = std::byte{3};
    enable.candidate_id = "shutdown-close-report-private-candidate"; enable.server_time_unix_milliseconds = now;
    return enable;
}
nstu::exam::TelemetrySample sample_for(std::uint64_t now) {
    nstu::exam::TelemetrySample sample;
    sample.kind = nstu::exam::TelemetrySampleKind::question_dwell; sample.source = nstu::exam::TelemetrySource::browser;
    sample.collector_id = 41; sample.question_id = "private-question"; sample.question_revision = 1;
    sample.monotonic_elapsed_milliseconds = 2000; sample.duration_milliseconds = 1000;
    sample.client_time_unix_milliseconds = now; sample.count = 1; sample.reason = nstu::exam::TelemetryReason::navigation_button;
    return sample;
}

void exact_pair(const Snapshot& snapshot, Pair expected) {
    assert(snapshot.dispatch == Dispatch::outcome_callback_invoked && snapshot.outcome);
    const auto& s = snapshot.outcome->shutdown; const auto& c = snapshot.outcome->close;
    assert(s.before == expected.shutdown.before && s.after == expected.shutdown.after && s.pending == expected.shutdown.pending);
    assert(s.publication == expected.shutdown.publication && s.cleanup == expected.shutdown.cleanup && s.current_failure == expected.shutdown.current_failure);
    assert(c.before == expected.close.before && c.after == expected.close.after && c.pending == expected.close.pending);
    assert(c.publication == expected.close.publication && c.cleanup == expected.close.cleanup && c.current_failure == expected.close.current_failure);
}
void protected_lifecycle(const Storage& storage, const nstu::exam::TelemetryEnable& enable,
                         nstu::client::TelemetryLifecycleState state) {
    const auto payload = state_payload(storage);
    const auto offset = std::size_t{108} + enable.package_id.size() + enable.candidate_id.size();
    assert(offset < payload.size() && payload[offset] == static_cast<std::byte>(state));
}
void physical_one_batch(const Storage& storage, const nstu::exam::TelemetryBatch& expected) {
    const auto payload = protected_payload(storage.outbox, "NSTU-Exam-Telemetry-Outbox-v1");
    const auto u32 = [&](std::size_t offset) {
        assert(offset <= payload.size() && payload.size() - offset >= 4);
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < 4; ++i) value |= std::to_integer<std::uint32_t>(payload[offset + i]) << (8 * i);
        return value;
    };
    assert(u32(0) == 0x544c4d4f && payload[4] == std::byte{2} && payload[5] == std::byte{0});
    assert(payload[6] == std::byte{0} && payload[7] == std::byte{0});
    const auto losses = u32(24); assert(losses <= 64);
    const auto records = std::size_t{28} + std::size_t{56} * losses;
    assert(u32(records) == 1);
    const auto encoded = nstu::exam::encode_telemetry_batch(expected);
    assert(!encoded.empty() && u32(records + 4) == encoded.size());
    const auto batch_offset = records + 8;
    assert(payload.size() == batch_offset + encoded.size() + 18);
    assert(std::equal(encoded.begin(), encoded.end(), payload.begin() + static_cast<std::ptrdiff_t>(batch_offset)));
    const auto batch = nstu::exam::decode_telemetry_batch(std::span<const std::byte>(payload).subspan(batch_offset, encoded.size()));
    assert(batch && batch->sequence == expected.sequence && batch->producer_id == expected.producer_id);
}
void test_native_state_pair_reporting() {
    Storage storage("state"); const auto now = now_ms(); unsigned clocks = 0, forwards = 0;
    Coordinator coordinator([&] { ++clocks; return now; });
    Router router(coordinator, [&](std::span<const std::byte>) { ++forwards; }); Capture capture;
    const auto enable = enable_for(now); const auto raw = nstu::exam::encode_telemetry_enable(enable);
    const auto sample = sample_for(now);
    assert(router.handle_restart(storage.outbox) && router.handle_server_enable(raw));
    const nstu::exam::TelemetryBatch expected{enable.package_id, enable.package_digest, enable.client_id,
        enable.session_id, enable.candidate_id, coordinator.active_producer_id(), 1, now, std::vector{sample}};
    assert(router.handle_agent_samples(expected.samples, now));
    const auto original_state = raw_file(storage.state), original_payload = raw_file(storage.outbox);
    Snapshot failed;
    const Pair expected_failure{{State::enabled, State::suspended, Pending::not_present, Publication::unconfirmed, Cleanup::not_needed, Error::protected_state_failed},
        {State::suspended, State::suspended, Pending::not_present, Publication::unconfirmed, Cleanup::not_needed, Error::protected_state_failed}};
    {
        LockedFile held(storage.state); failed = router.handle_shutdown_with_outcome();
        exact_pair(failed, expected_failure);
        assert(coordinator.is_open() && coordinator.next_sequence() == 2 && forwards == 1);
        assert(raw_file(storage.state) == original_state && raw_file(storage.outbox) == original_payload);
        protected_lifecycle(storage, enable, nstu::client::TelemetryLifecycleState::enabled);
        const auto observed_clocks = clocks; const auto history = coordinator.last_error(); assert(!history.empty());
        dispositions(capture.reporter.report(failed), Disposition::observed_failure, Disposition::observed_failure);
        dispositions(capture.reporter.report(failed), Disposition::observed_failure, Disposition::observed_failure);
        capture.notice(0, "shutdown", "dispatch=outcome_callback_invoked before=enabled after=suspended pending=not_present p=unconfirmed cleanup=not_needed error=protected_state_failed");
        capture.notice(1, "close", "dispatch=outcome_callback_invoked before=suspended after=suspended pending=not_present p=unconfirmed cleanup=not_needed error=protected_state_failed");
        assert(capture.spool.size() == 2 && clocks == observed_clocks && coordinator.last_error() == history);
        assert(raw_file(storage.state) == original_state && raw_file(storage.outbox) == original_payload);
        assert(coordinator.is_open() && coordinator.next_sequence() == 2 && forwards == 1);
        std::cout << capture.debug[0] << capture.debug[1];
    }
    const auto history = coordinator.last_error();
    const auto recovered = router.handle_shutdown_with_outcome();
    exact_pair(recovered, {{State::suspended, State::suspended, Pending::not_present, Publication::current_protected_state_confirmed, Cleanup::not_needed, Error::none}, complete_pair().close});
    const auto state_after = raw_file(storage.state); const auto observed_clocks = clocks;
    physical_one_batch(storage, expected); // Physical first receipt before reopening.
    dispositions(capture.reporter.report(recovered), Disposition::complete, Disposition::complete);
    exact_pair(failed, expected_failure); // First immutable observation did not change.
    assert(!coordinator.is_open() && clocks == observed_clocks && coordinator.last_error() == history && forwards == 1);
    assert(raw_file(storage.state) == state_after && raw_file(storage.outbox) == original_payload && capture.spool.size() == 2);
    assert(router.handle_restart(storage.outbox) && !router.poll_outbound_batch(now));
    assert(router.handle_server_enable(raw) && forwards == 2);
    const auto offered = router.poll_outbound_batch(now); assert(offered);
    assert(offered->payload == nstu::exam::encode_telemetry_batch(expected));
    std::cout << "Native dual state failure snapshots, immutable reporting and no-writer close recovery passed.\n";
}
void test_native_wal_pair_reporting() {
    Storage storage("wal"); const auto now = now_ms(); unsigned clocks = 0, forwards = 0;
    Coordinator coordinator([&] { ++clocks; return now; });
    Router router(coordinator, [&](std::span<const std::byte>) { ++forwards; }); Capture capture;
    const auto enable = enable_for(now); const auto raw = nstu::exam::encode_telemetry_enable(enable);
    assert(router.handle_restart(storage.outbox) && router.handle_server_enable(raw));
    const nstu::exam::TelemetryBatch expected{enable.package_id, enable.package_digest, enable.client_id,
        enable.session_id, enable.candidate_id, coordinator.active_producer_id(), 1, now, std::vector{sample_for(now)}};
    const auto encoded = nstu::exam::encode_telemetry_batch(expected); const auto original_payload = raw_file(storage.outbox);
    Snapshot failed;
    {
        LockedFile held(storage.outbox);
        assert(!router.handle_agent_samples(expected.samples, now));
        failed = router.handle_shutdown_with_outcome(); exact_pair(failed, failure_pair());
        assert(coordinator.is_open() && coordinator.next_sequence() == 1 && coordinator.outbox_size() == 0 && forwards == 1);
        const auto wal = state_payload(storage), state = raw_file(storage.state);
        assert(!encoded.empty() && wal.size() >= encoded.size() && std::equal(encoded.rbegin(), encoded.rend(), wal.rbegin()));
        assert(raw_file(storage.outbox) == original_payload);
        protected_lifecycle(storage, enable, nstu::client::TelemetryLifecycleState::suspended);
        const auto observed_clocks = clocks; const auto history = coordinator.last_error();
        dispositions(capture.reporter.report(failed), Disposition::observed_failure, Disposition::observed_failure);
        dispositions(capture.reporter.report(failed), Disposition::observed_failure, Disposition::observed_failure);
        capture.notice(0, "shutdown", kShutdownFailure); capture.notice(1, "close", kCloseFailure);
        assert(clocks == observed_clocks && coordinator.last_error() == history && capture.spool.size() == 2);
        assert(raw_file(storage.state) == state && raw_file(storage.outbox) == original_payload && state_payload(storage) == wal);
        assert(coordinator.next_sequence() == 1 && coordinator.is_open() && forwards == 1);
        std::cout << capture.debug[0] << capture.debug[1];
    }
    const auto history = coordinator.last_error(); const auto recovered = router.handle_shutdown_with_outcome();
    exact_pair(recovered, {{State::suspended, State::suspended, Pending::completed, Publication::current_protected_state_confirmed, Cleanup::not_needed, Error::none}, complete_pair().close});
    assert(!coordinator.is_open() && coordinator.next_sequence() == 2 && coordinator.active_producer_id() == expected.producer_id && forwards == 1);
    physical_one_batch(storage, expected); // No reopening/poll/maintenance before raw receipt.
    const auto payload = raw_file(storage.outbox), state = raw_file(storage.state); const auto observed_clocks = clocks;
    dispositions(capture.reporter.report(recovered), Disposition::complete, Disposition::complete);
    exact_pair(failed, failure_pair());
    assert(capture.spool.size() == 2 && clocks == observed_clocks && coordinator.last_error() == history);
    assert(raw_file(storage.outbox) == payload && raw_file(storage.state) == state);
    assert(router.handle_restart(storage.outbox) && coordinator.next_sequence() == 2 && !router.poll_outbound_batch(now));
    assert(router.handle_server_enable(raw) && forwards == 2);
    const auto offered = router.poll_outbound_batch(now); assert(offered && offered->payload == encoded);
    std::cout << "Native retained WAL reports separate publications; one physical batch and gated recovery passed.\n";
}
void test_native_quiet_ordinary_and_expiry() {
    unsigned closed_clocks = 0;
    Coordinator closed([&] { ++closed_clocks; return now_ms(); });
    Router closed_router(closed, [](std::span<const std::byte>) { assert(false); }); Capture closed_capture;
    const auto closed_pair = closed_router.handle_shutdown_with_outcome(); exact_pair(closed_pair, {});
    dispositions(closed_capture.reporter.report(closed_pair), Disposition::no_applicable_work, Disposition::no_applicable_work);
    assert(closed_clocks == 0 && closed_capture.spool.empty());
    for (const bool enabled : {false, true}) {
        Storage storage(enabled ? "ordinary" : "disabled"); const auto now = now_ms(); unsigned clocks = 0, forwards = 0;
        Coordinator coordinator([&] { ++clocks; return now; }); Router router(coordinator, [&](std::span<const std::byte>) { ++forwards; }); Capture capture;
        assert(router.handle_restart(storage.outbox));
        const auto enable = enable_for(now); const auto raw = nstu::exam::encode_telemetry_enable(enable);
        if (enabled) {
            assert(router.handle_server_enable(raw));
            assert(router.handle_agent_samples(std::vector{sample_for(now)}, now));
        }
        const auto original_payload = raw_file(storage.outbox); const auto pair = router.handle_shutdown_with_outcome();
        if (enabled) exact_pair(pair, complete_pair());
        else exact_pair(pair, {{State::disabled, State::disabled, Pending::not_present, Publication::current_protected_state_confirmed, Cleanup::not_needed, Error::none},
                              {State::disabled, State::closed, Pending::not_present, Publication::not_attempted, Cleanup::not_needed, Error::none}});
        assert(!coordinator.is_open() && raw_file(storage.outbox) == original_payload);
        const auto state = raw_file(storage.state); const auto observed_clocks = clocks;
        dispositions(capture.reporter.report(pair), enabled ? Disposition::complete : Disposition::no_applicable_work, Disposition::complete);
        assert(clocks == observed_clocks && capture.spool.empty() && capture.debug.empty());
        assert(raw_file(storage.state) == state && raw_file(storage.outbox) == original_payload && forwards == (enabled ? 1u : 0u));
    }
    Storage storage("clean-expiry"); auto now = now_ms(); unsigned clocks = 0, forwards = 0;
    Coordinator coordinator([&] { ++clocks; return now; }); Router router(coordinator, [&](std::span<const std::byte>) { ++forwards; }); Capture capture;
    const auto enable = enable_for(now); const auto raw = nstu::exam::encode_telemetry_enable(enable);
    assert(router.handle_restart(storage.outbox) && router.handle_server_enable(raw));
    const nstu::exam::TelemetryBatch expected{enable.package_id, enable.package_digest, enable.client_id,
        enable.session_id, enable.candidate_id, coordinator.active_producer_id(), 1, now, std::vector{sample_for(now)}};
    const auto encoded = nstu::exam::encode_telemetry_batch(expected);
    {
        LockedFile held(storage.outbox); assert(!router.handle_agent_samples(expected.samples, now));
        const auto wal = state_payload(storage);
        assert(!encoded.empty() && wal.size() >= encoded.size() && std::equal(encoded.rbegin(), encoded.rend(), wal.rbegin()));
    }
    now += nstu::exam::kTelemetryRetentionMilliseconds;
    const auto pair = router.handle_shutdown_with_outcome();
    exact_pair(pair, {{State::enabled, State::revoked, Pending::revoked_expired, Publication::current_protected_state_confirmed, Cleanup::complete, Error::none},
                     {State::revoked, State::closed, Pending::not_present, Publication::not_attempted, Cleanup::not_needed, Error::none}});
    assert(!coordinator.is_open() && coordinator.next_sequence() == 1 && coordinator.dropped_sample_count() == 1 && coordinator.dropped_batch_count() == 1);
    physical_empty_outbox(storage); protected_lifecycle(storage, enable, nstu::client::TelemetryLifecycleState::revoked);
    const auto state = raw_file(storage.state), payload = raw_file(storage.outbox); const auto history = coordinator.last_error(); const auto observed_clocks = clocks;
    dispositions(capture.reporter.report(pair), Disposition::complete, Disposition::complete);
    assert(!history.empty() && coordinator.last_error() == history && clocks == observed_clocks && capture.spool.empty());
    assert(raw_file(storage.state) == state && raw_file(storage.outbox) == payload && forwards == 1);
    assert(router.handle_restart(storage.outbox) && !router.handle_server_enable(raw) && !router.poll_outbound_batch(now) && forwards == 1);
    std::cout << "Real closed/disabled/ordinary and clean-expiry pairs classify without false final-closure witness.\n";
}

void test_sink_independence_and_retry() {
    for (const std::string failed_action : {"shutdown", "close"}) {
        for (const bool throw_debug : {false, true}) {
            nstu::audit::Spool spool; unsigned shutdown_audits = 0, close_audits = 0, debug_calls = 0;
            std::vector<std::pair<std::string, std::uint64_t>> assigned;
            Reporter reporter([&](const nstu::audit::Event& event) {
                auto& count = event.action == "shutdown" ? shutdown_audits : close_audits;
                ++count;
                if (event.action == failed_action && count == 1) throw std::runtime_error("fails before real Spool delegation");
                const auto sequence = spool.emit(event); assigned.emplace_back(event.action, sequence); return sequence;
            }, [&](const std::string&) {
                ++debug_calls; if (throw_debug) throw std::runtime_error("debug sink fails");
            });
            dispositions(reporter.report(typed(failure_pair())), Disposition::observed_failure, Disposition::observed_failure);
            assert(shutdown_audits == 1 && close_audits == 1 && debug_calls == 2 && spool.size() == 1);
            assert(assigned.size() == 1 && assigned[0].first != failed_action && assigned[0].second == 1);
            dispositions(reporter.report(typed(Pair{})), Disposition::no_applicable_work, Disposition::no_applicable_work);
            dispositions(reporter.report(typed(failure_pair())), Disposition::observed_failure, Disposition::observed_failure);
            assert(shutdown_audits == (failed_action == "shutdown" ? 2u : 1u));
            assert(close_audits == (failed_action == "close" ? 2u : 1u));
            assert(debug_calls == 2 && spool.size() == 2 && assigned.size() == 2 && assigned[1].first == failed_action && assigned[1].second == 2);
            const auto decoded = nstu::audit::decode_audit_chunk(nstu::audit::encode_audit_chunk(spool.peek_chunk()));
            assert(decoded && decoded->size() == 2);
            for (std::size_t i = 0; i < 2; ++i) {
                assert((*decoded)[i].sequence == assigned[i].second && (*decoded)[i].event.action == assigned[i].first);
                assert((*decoded)[i].event.detail == (assigned[i].first == "shutdown" ? kShutdownFailure : kCloseFailure));
            }
            dispositions(reporter.report(typed(failure_pair())), Disposition::observed_failure, Disposition::observed_failure);
            assert(assigned.size() == 2 && debug_calls == 2);
        }
    }
    nstu::audit::Spool spool; unsigned shutdown_calls = 0, close_calls = 0, shutdown_debug = 0, close_debug = 0;
    Reporter pending([&](const nstu::audit::Event& event) {
        if (event.action == "shutdown") {
            ++shutdown_calls; if (shutdown_calls <= 2) throw std::runtime_error("shutdown remains unaccepted");
        } else { ++close_calls; }
        return spool.emit(event);
    }, [&](const std::string& line) {
        if (line.find("NSTU exam telemetry shutdown:") == 0) ++shutdown_debug;
        else { ++close_debug; }
    });
    dispositions(pending.report(typed(failure_pair())), Disposition::observed_failure, Disposition::observed_failure);
    auto mixed = failure_pair(); mixed.close = complete_pair().close;
    dispositions(pending.report(typed(mixed)), Disposition::observed_failure, Disposition::complete);
    assert(shutdown_calls == 2 && close_calls == 1 && shutdown_debug == 1 && close_debug == 1 && spool.size() == 1);
    dispositions(pending.report(typed(mixed)), Disposition::observed_failure, Disposition::complete);
    assert(shutdown_calls == 3 && close_calls == 1 && shutdown_debug == 1 && close_debug == 1 && spool.size() == 2);
    assert(spool.peek_chunk()[1].event.action == "shutdown" && spool.peek_chunk()[1].event.detail == kShutdownFailure);
    unsigned audits = 0, debug = 0; nstu::audit::Spool both;
    Reporter failing_both([&](const nstu::audit::Event& event) {
        ++audits; if (audits <= 2) throw std::runtime_error("both initial audit attempts fail"); return both.emit(event);
    }, [&](const std::string&) { ++debug; throw std::runtime_error("both debug attempts fail"); });
    dispositions(failing_both.report(typed(failure_pair())), Disposition::observed_failure, Disposition::observed_failure);
    assert(audits == 2 && debug == 2 && both.empty());
    dispositions(failing_both.report(typed(failure_pair())), Disposition::observed_failure, Disposition::observed_failure);
    assert(audits == 4 && debug == 2 && both.size() == 2);
    std::cout << "Per-operation debug/audit throws preserve both attempts; retry and complete-close independence passed.\n";
}
void test_availability_exceptions_and_memory_loss() {
    for (const bool throw_debug : {false, true}) {
        nstu::audit::Spool spool; unsigned audits = 0, debug = 0;
        Reporter reporter([&](const nstu::audit::Event& event) {
            ++audits; if (audits == 1) throw std::runtime_error("availability audit fails before delegation"); return spool.emit(event);
        }, [&](const std::string&) { ++debug; if (throw_debug) throw std::runtime_error("availability debug fails"); });
        dispositions(reporter.report({}), Disposition::outcome_unavailable, Disposition::outcome_unavailable);
        assert(audits == 1 && debug == 1 && spool.empty());
        dispositions(reporter.report({Dispatch::not_configured, failure_pair()}), Disposition::outcome_unavailable, Disposition::outcome_unavailable);
        assert(audits == 2 && debug == 1 && spool.size() == 1);
        dispositions(reporter.report(typed(Pair{})), Disposition::no_applicable_work, Disposition::no_applicable_work);
        dispositions(reporter.report({}), Disposition::outcome_unavailable, Disposition::outcome_unavailable);
        assert(audits == 3 && debug == 2 && spool.size() == 2);
    }
    nstu::audit::Spool tiny(1); unsigned attempts = 0;
    Reporter reporter([&](const nstu::audit::Event& event) { ++attempts; return tiny.emit(event); }, [](const std::string&) {});
    dispositions(reporter.report(typed(failure_pair())), Disposition::observed_failure, Disposition::observed_failure);
    assert(attempts == 2 && tiny.size() == 1 && tiny.dropped_count() == 1);
    const auto records = nstu::audit::decode_audit_chunk(nstu::audit::encode_audit_chunk(tiny.peek_chunk()));
    assert(records && records->size() == 1 && (*records)[0].sequence == 2 && (*records)[0].event.action == "close");
    assert((*records)[0].event.detail == kCloseFailure);
    dispositions(reporter.report(typed(failure_pair())), Disposition::observed_failure, Disposition::observed_failure);
    assert(attempts == 2 && tiny.dropped_count() == 1);
    nstu::audit::Event unrelated; unrelated.component = "unrelated";
    const auto unrelated_sequence = tiny.emit(unrelated); assert(unrelated_sequence == 3 && tiny.dropped_count() == 2);
    dispositions(reporter.report(typed(failure_pair())), Disposition::observed_failure, Disposition::observed_failure);
    assert(attempts == 2 && tiny.peek_chunk()[0].sequence == 3);
    const auto ack = nstu::audit::decode_audit_ack(nstu::audit::encode_audit_ack(unrelated_sequence));
    assert(ack && *ack == 3); tiny.acknowledge(*ack); assert(tiny.empty());
    dispositions(reporter.report(typed(failure_pair())), Disposition::observed_failure, Disposition::observed_failure);
    assert(attempts == 2 && tiny.empty());
    nstu::audit::Spool shared;
    Reporter first([&](const nstu::audit::Event& event) { return shared.emit(event); }, [](const std::string&) {});
    Reporter second([&](const nstu::audit::Event& event) { return shared.emit(event); }, [](const std::string&) {});
    dispositions(first.report(typed(failure_pair())), Disposition::observed_failure, Disposition::observed_failure);
    dispositions(second.report(typed(failure_pair())), Disposition::observed_failure, Disposition::observed_failure);
    assert(shared.size() == 4 && shared.peek_chunk()[3].sequence == 4);
    // Unavailable never unsuppresses accepted operation epochs.
    dispositions(first.report({}), Disposition::outcome_unavailable, Disposition::outcome_unavailable);
    const auto size = shared.size();
    dispositions(first.report(typed(failure_pair())), Disposition::observed_failure, Disposition::observed_failure);
    assert(shared.size() == size);
    std::cout << "Availability retry/reset, actual assigned sequences, ACK/capacity-one loss and independent instances passed.\n";
}
void test_serialized_pairs_and_operation_local_epochs() {
    nstu::audit::Spool spool; std::atomic<unsigned> active{0}, debug_calls{0}, audit_calls{0};
    Reporter reporter([&](const nstu::audit::Event& event) {
        assert(active.fetch_add(1) == 0); ++audit_calls;
        const auto assigned = spool.emit(event); assert(active.fetch_sub(1) == 1); return assigned;
    }, [&](const std::string&) { assert(active.fetch_add(1) == 0); ++debug_calls; assert(active.fetch_sub(1) == 1); });
    constexpr std::size_t count = 8; std::barrier start(static_cast<std::ptrdiff_t>(count + 1));
    const auto observation = typed(failure_pair()); std::vector<std::thread> threads;
    for (std::size_t i = 0; i < count; ++i) threads.emplace_back([&] {
        start.arrive_and_wait(); dispositions(reporter.report(observation), Disposition::observed_failure, Disposition::observed_failure);
    });
    start.arrive_and_wait(); for (auto& thread : threads) thread.join();
    assert(debug_calls == 2 && audit_calls == 2 && active == 0 && spool.size() == 2);
    assert(spool.peek_chunk()[0].event.action == "shutdown" && spool.peek_chunk()[1].event.action == "close");
    std::mutex mutex; std::condition_variable cv; bool entered = false, second_ready = false, release = false;
    std::vector<std::string> actions; nstu::audit::Spool ordered_spool;
    Reporter ordered([&](const nstu::audit::Event& event) {
        std::unique_lock lock(mutex); actions.push_back(event.action);
        if (actions.size() == 1) {
            entered = true; cv.notify_all(); assert(cv.wait_for(lock, std::chrono::seconds(5), [&] { return release; }));
        }
        return ordered_spool.emit(event);
    }, [](const std::string&) {});
    auto changed = failure_pair(); changed.shutdown.current_failure = Error::pending_conflict;
    std::thread a([&] { dispositions(ordered.report(observation), Disposition::observed_failure, Disposition::observed_failure); });
    { std::unique_lock lock(mutex); assert(cv.wait_for(lock, std::chrono::seconds(5), [&] { return entered; })); }
    std::thread b([&] {
        { std::lock_guard lock(mutex); second_ready = true; cv.notify_all(); }
        dispositions(ordered.report(typed(changed)), Disposition::observed_failure, Disposition::observed_failure);
    });
    {
        std::unique_lock lock(mutex); assert(cv.wait_for(lock, std::chrono::seconds(5), [&] { return second_ready; }));
        release = true; cv.notify_all();
    }
    a.join(); b.join();
    dispositions(ordered.report(observation), Disposition::observed_failure, Disposition::observed_failure);
    assert((actions == std::vector<std::string>{"shutdown", "close", "shutdown", "shutdown"}));
    const auto records = ordered_spool.peek_chunk(); assert(records.size() == 4);
    assert(records[0].event.detail == kShutdownFailure && records[1].event.detail == kCloseFailure && records[3].event.detail == kShutdownFailure);
    assert(records[2].event.detail == "dispatch=outcome_callback_invoked before=enabled after=suspended pending=retained p=current_protected_state_confirmed cleanup=not_needed error=pending_conflict");
    std::cout << "Barrier-start pair serialization and bounded operation-local A-B-A with unchanged close passed.\n";
}
void test_close_epochs_and_availability_preservation() {
    Capture capture;
    dispositions(capture.reporter.report(typed(failure_pair())), Disposition::observed_failure, Disposition::observed_failure);
    auto changed = failure_pair(); changed.close.current_failure = Error::pending_conflict;
    dispositions(capture.reporter.report(typed(changed)), Disposition::observed_failure, Disposition::observed_failure);
    dispositions(capture.reporter.report(typed(failure_pair())), Disposition::observed_failure, Disposition::observed_failure);
    assert(capture.spool.size() == 4);
    capture.notice(0, "shutdown", kShutdownFailure); capture.notice(1, "close", kCloseFailure);
    capture.notice(2, "close", "dispatch=outcome_callback_invoked before=suspended after=suspended pending=retained p=not_attempted cleanup=not_needed error=pending_conflict");
    capture.notice(3, "close", kCloseFailure);
    dispositions(capture.reporter.report({}), Disposition::outcome_unavailable, Disposition::outcome_unavailable);
    dispositions(capture.reporter.report(typed(failure_pair())), Disposition::observed_failure, Disposition::observed_failure);
    assert(capture.spool.size() == 5); // Availability never resets either operation.
    auto quiet_shutdown = complete_pair();
    quiet_shutdown.shutdown = {State::disabled, State::disabled, Pending::not_present, Publication::current_protected_state_confirmed, Cleanup::not_needed, Error::none};
    dispositions(capture.reporter.report(typed(quiet_shutdown)), Disposition::no_applicable_work, Disposition::complete);
    dispositions(capture.reporter.report(typed(failure_pair())), Disposition::observed_failure, Disposition::observed_failure);
    assert(capture.spool.size() == 6); capture.notice(5, "close", kCloseFailure);
    std::cout << "Close-local failing A-B-A, unavailable preservation and disabled-quiet/close-only reset passed.\n";
}
} // namespace

int main() {
    test_interface_dispatch_and_two_notices();
    test_unavailable_and_unsupported_attachments();
    test_literal_sanitized_vocabulary();
    test_exact_predicates_and_neighbors();
    test_twelve_unsupported_fields();
    test_native_state_pair_reporting();
    test_native_wal_pair_reporting();
    test_native_quiet_ordinary_and_expiry();
    test_sink_independence_and_retry();
    test_availability_exceptions_and_memory_loss();
    test_serialized_pairs_and_operation_local_epochs();
    test_close_epochs_and_availability_preservation();
    return 0;
}
