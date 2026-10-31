#include "nstu/exam_telemetry_disconnect_reporting.hpp"
#include "nstu/secret_store.hpp"
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cassert>
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
using Reporter = nstu::client::ExamTelemetryDisconnectReporter;
using Disposition = nstu::client::TelemetryDisconnectReportDisposition;
using Snapshot = nstu::client::TelemetryDisconnectDispatchResult;
using Dispatch = nstu::client::TelemetryDisconnectDispatch;
using Outcome = nstu::client::TelemetryDisconnectOutcome;
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

Snapshot typed(Outcome outcome) { return {Dispatch::outcome_callback_invoked, outcome}; }
Snapshot failure() {
    return typed({State::enabled, State::suspended, Pending::completed,
        Publication::earlier_protected_snapshot_confirmed, Cleanup::not_needed, Error::protected_state_failed});
}
const std::string kFailure = "dispatch=outcome_callback_invoked before=enabled after=suspended pending=completed "
    "p=earlier_protected_snapshot_confirmed cleanup=not_needed error=protected_state_failed";

struct Capture {
    nstu::audit::Spool spool;
    std::vector<nstu::audit::Event> events;
    std::vector<std::string> debug;
    std::vector<char> order;
    Reporter reporter;
    Capture() : reporter([this](const nstu::audit::Event& event) {
        order.push_back('a'); events.push_back(event); return spool.emit(event);
    }, [this](const std::string& text) { order.push_back('d'); debug.push_back(text); }) {}
    void last(const std::string& result, const std::string& detail) const {
        const auto records = spool.peek_chunk();
        assert(!records.empty() && !debug.empty());
        const auto decoded = nstu::audit::decode_audit_chunk(nstu::audit::encode_audit_chunk(records));
        assert(decoded && decoded->size() == records.size());
        const auto& event = decoded->back().event;
        assert(event.category == nstu::audit::Category::exam && event.severity == nstu::audit::Severity::warning);
        assert(event.component == "exam_telemetry" && event.action == "disconnect");
        assert(event.operation_id == 0 && event.client_id == 0);
        assert(event.result == result && event.detail == detail);
        assert(event.detail.size() <= nstu::audit::kMaximumDetailBytes);
        assert(event.detail.find("<redacted>") == std::string::npos);
        assert(debug.back() == "NSTU exam telemetry disconnect: " + result + " " + detail + "\n");
        assert(!events.empty() && events.back().detail == detail && events.back().result == result);
        assert(events.back().category == event.category && events.back().severity == event.severity);
        assert(events.back().component == event.component && events.back().action == event.action);
        assert(events.back().operation_id == 0 && events.back().client_id == 0);
        assert(std::count(debug.back().begin(), debug.back().end(), '\n') == 1);
        assert(detail.find_first_of("\r\n\t") == std::string::npos);
        assert(nstu::audit::sanitize_event(events.back()).detail == detail);
        for (std::size_t i = 0; i < records.size(); ++i) {
            assert((*decoded)[i].sequence == records[i].sequence);
            assert((*decoded)[i].timestamp_unix_milliseconds == records[i].timestamp_unix_milliseconds);
            assert((*decoded)[i].event.detail == records[i].event.detail);
        }
    }
};

void test_interface_and_current_notice() {
    bool emitter_rejected = false, debug_rejected = false;
    try { Reporter invalid({}, [](const std::string&) {}); }
    catch (const std::invalid_argument&) { emitter_rejected = true; }
    try { Reporter invalid([](const nstu::audit::Event&) { return std::uint64_t{1}; }, {}); }
    catch (const std::invalid_argument&) { debug_rejected = true; }
    assert(emitter_rejected && debug_rejected);
    Capture capture;
    Router absent;
    assert(capture.reporter.report(absent.handle_disconnect_with_outcome()) == Disposition::outcome_unavailable);
    capture.last("outcome_unavailable", "dispatch=not_configured outcome=unavailable");
    assert(capture.reporter.report(failure()) == Disposition::observed_failure);
    capture.last("failed", kFailure);
    assert(capture.spool.size() == 2 && capture.debug.size() == 2);
    assert(capture.reporter.report(failure()) == Disposition::observed_failure);
    assert(capture.spool.size() == 2 && capture.debug.size() == 2);
    Coordinator closed;
    Router real(closed, [](std::span<const std::byte>) { assert(false); });
    assert(capture.reporter.report(real.handle_disconnect_with_outcome()) == Disposition::no_applicable_work);
    assert(capture.reporter.report(failure()) == Disposition::observed_failure && capture.spool.size() == 2);
    assert(capture.reporter.report(typed({State::enabled, State::suspended, Pending::completed,
        Publication::current_protected_state_confirmed, Cleanup::not_needed, Error::none})) == Disposition::complete);
    assert(capture.reporter.report(failure()) == Disposition::observed_failure && capture.spool.size() == 3);
    capture.last("failed", kFailure);
    std::cout << "Interface, exact sanitized notice, real Spool wire and current suppression passed.\n";
}

void test_dispatch_and_all_current_fields() {
    Capture capture;
    for (const auto dispatch : {Dispatch::not_configured, Dispatch::legacy_callback_invoked}) {
        const auto expected = dispatch == Dispatch::not_configured ? "dispatch=not_configured outcome=unavailable" :
            "dispatch=legacy_callback_invoked outcome=unavailable";
        assert(capture.reporter.report({dispatch, std::nullopt}) == Disposition::outcome_unavailable);
        capture.last("outcome_unavailable", expected);
        const auto count = capture.spool.size();
        assert(capture.reporter.report({dispatch, *failure().outcome}) == Disposition::outcome_unavailable);
        auto unsupported = *failure().outcome;
        unsupported.current_failure = static_cast<Error>(255);
        assert(capture.reporter.report({dispatch, unsupported}) == Disposition::outcome_unavailable);
        assert(capture.spool.size() == count);
    }
    int calls = 0;
    nstu::client::ExamTelemetryRouterCallbacks callbacks;
    callbacks.on_disconnect = [&] { ++calls; };
    Router legacy(std::move(callbacks));
    assert(capture.reporter.report(legacy.handle_disconnect_with_outcome()) == Disposition::outcome_unavailable);
    assert(calls == 1);
    assert(capture.reporter.report({Dispatch::outcome_callback_invoked, std::nullopt}) == Disposition::outcome_unavailable);
    capture.last("outcome_unavailable", "dispatch=outcome_callback_invoked outcome=unavailable");
    assert(capture.reporter.report({static_cast<Dispatch>(255), std::nullopt}) == Disposition::observed_failure);
    capture.last("failed", "dispatch=unknown outcome=unavailable");
    assert(capture.reporter.report({static_cast<Dispatch>(255), Outcome{}}) == Disposition::observed_failure);
    capture.last("failed", "dispatch=unknown before=closed after=closed pending=not_present p=not_attempted cleanup=not_needed error=none");

    const std::array<Outcome, 6> unknowns{{
        {static_cast<State>(255), State::suspended, Pending::completed, Publication::current_protected_state_confirmed, Cleanup::not_needed, Error::none},
        {State::enabled, static_cast<State>(255), Pending::completed, Publication::current_protected_state_confirmed, Cleanup::not_needed, Error::none},
        {State::enabled, State::suspended, static_cast<Pending>(255), Publication::current_protected_state_confirmed, Cleanup::not_needed, Error::none},
        {State::enabled, State::suspended, Pending::completed, static_cast<Publication>(255), Cleanup::not_needed, Error::none},
        {State::enabled, State::suspended, Pending::completed, Publication::current_protected_state_confirmed, static_cast<Cleanup>(255), Error::none},
        {State::enabled, State::suspended, Pending::completed, Publication::current_protected_state_confirmed, Cleanup::not_needed, static_cast<Error>(255)}
    }};
    const std::array<const char*, 6> expected{{
        "before=unknown after=suspended pending=completed p=current_protected_state_confirmed cleanup=not_needed error=none",
        "before=enabled after=unknown pending=completed p=current_protected_state_confirmed cleanup=not_needed error=none",
        "before=enabled after=suspended pending=unknown p=current_protected_state_confirmed cleanup=not_needed error=none",
        "before=enabled after=suspended pending=completed p=unknown cleanup=not_needed error=none",
        "before=enabled after=suspended pending=completed p=current_protected_state_confirmed cleanup=unknown error=none",
        "before=enabled after=suspended pending=completed p=current_protected_state_confirmed cleanup=not_needed error=unknown"
    }};
    for (std::size_t i = 0; i < unknowns.size(); ++i) {
        assert(capture.reporter.report(typed(unknowns[i])) == Disposition::observed_failure);
        capture.last("failed", std::string("dispatch=outcome_callback_invoked ") + expected[i]);
    }
    const auto longest = typed({State::suspended, State::suspended, Pending::revoked_age_uncertain,
        Publication::earlier_protected_snapshot_confirmed, Cleanup::not_needed, Error::revocation_witness_unconfirmed});
    const std::string longest_detail = "dispatch=outcome_callback_invoked before=suspended after=suspended pending=revoked_age_uncertain "
        "p=earlier_protected_snapshot_confirmed cleanup=not_needed error=revocation_witness_unconfirmed";
    assert(longest_detail.size() == 191);
    assert(capture.reporter.report(longest) == Disposition::observed_failure);
    capture.last("failed", longest_detail);
    assert(capture.reporter.report(typed({State::enabled, State::revoked, Pending::revoked_expired,
        Publication::refusal_marker_confirmed, Cleanup::pending, Error::pending_conflict})) == Disposition::observed_failure);
    capture.last("failed", "dispatch=outcome_callback_invoked before=enabled after=revoked pending=revoked_expired "
        "p=refusal_marker_confirmed cleanup=pending error=pending_conflict");
    assert(capture.reporter.report(typed({State::enabled, State::suspended, Pending::retained,
        Publication::unconfirmed, Cleanup::pending, Error::pending_enqueue_unfinished})) == Disposition::observed_failure);
    capture.last("failed", "dispatch=outcome_callback_invoked before=enabled after=suspended pending=retained "
        "p=unconfirmed cleanup=pending error=pending_enqueue_unfinished");
    for (std::size_t i = 0; i < capture.order.size(); i += 2)
        assert(capture.order[i] == 'd' && capture.order[i + 1] == 'a');
    nstu::audit::Spool spool;
    Reporter explicit_noop([&](const nstu::audit::Event& event) { return spool.emit(event); }, [](const std::string&) {});
    assert(explicit_noop.report(failure()) == Disposition::observed_failure && spool.size() == 1);
    std::cout << "Dispatch/attachments, all six unsupported fields, longest known tokens and independent literal notices passed.\n";
}

void test_complete_predicates_and_nearby_failures() {
    Capture capture;
    const std::array<Outcome, 8> complete{{
        {State::enabled, State::suspended, Pending::not_present, Publication::current_protected_state_confirmed, Cleanup::not_needed, Error::none},
        {State::suspended, State::suspended, Pending::completed, Publication::current_protected_state_confirmed, Cleanup::not_needed, Error::none},
        {State::revoked, State::revoked, Pending::not_present, Publication::current_protected_state_confirmed, Cleanup::not_needed, Error::none},
        {State::revoked, State::revoked, Pending::not_present, Publication::current_protected_state_confirmed, Cleanup::complete, Error::none},
        {State::enabled, State::revoked, Pending::revoked_expired, Publication::current_protected_state_confirmed, Cleanup::complete, Error::none},
        {State::suspended, State::revoked, Pending::revoked_expired, Publication::current_protected_state_confirmed, Cleanup::complete, Error::none},
        {State::enabled, State::revoked, Pending::revoked_age_uncertain, Publication::current_protected_state_confirmed, Cleanup::complete, Error::none},
        {State::suspended, State::revoked, Pending::revoked_age_uncertain, Publication::current_protected_state_confirmed, Cleanup::complete, Error::none}
    }};
    // Supplied classification cases; coordinator/router targets own reachability.
    for (const auto& outcome : complete) {
        const auto count = capture.spool.size();
        assert(capture.reporter.report(failure()) == Disposition::observed_failure);
        assert(capture.reporter.report(typed(outcome)) == Disposition::complete);
        assert(capture.spool.size() == count + 1);
        assert(capture.reporter.report(failure()) == Disposition::observed_failure && capture.spool.size() == count + 2);
        assert(capture.reporter.report(typed(outcome)) == Disposition::complete);
    }
    const std::array<Outcome, 7> failures{{
        {State::enabled, State::suspended, Pending::retained, Publication::current_protected_state_confirmed, Cleanup::not_needed, Error::none},
        {State::enabled, State::suspended, Pending::completed, Publication::current_protected_state_confirmed, Cleanup::pending, Error::none},
        {State::revoked, State::revoked, Pending::retained, Publication::current_protected_state_confirmed, Cleanup::complete, Error::none},
        {State::enabled, State::revoked, Pending::revoked_expired, Publication::current_protected_state_confirmed, Cleanup::not_needed, Error::none},
        {State::disabled, State::disabled, Pending::not_present, Publication::unconfirmed, Cleanup::not_needed, Error::protected_state_failed},
        {State::disabled, State::revoked, Pending::revoked_expired, Publication::current_protected_state_confirmed, Cleanup::complete, Error::none},
        {State::enabled, State::suspended, Pending::not_present, Publication::current_protected_state_confirmed, Cleanup::pending, Error::cleanup_failed}
    }};
    const std::array<const char*, 7> failure_details{{
        "before=enabled after=suspended pending=retained p=current_protected_state_confirmed cleanup=not_needed error=none",
        "before=enabled after=suspended pending=completed p=current_protected_state_confirmed cleanup=pending error=none",
        "before=revoked after=revoked pending=retained p=current_protected_state_confirmed cleanup=complete error=none",
        "before=enabled after=revoked pending=revoked_expired p=current_protected_state_confirmed cleanup=not_needed error=none",
        "before=disabled after=disabled pending=not_present p=unconfirmed cleanup=not_needed error=protected_state_failed",
        "before=disabled after=revoked pending=revoked_expired p=current_protected_state_confirmed cleanup=complete error=none",
        "before=enabled after=suspended pending=not_present p=current_protected_state_confirmed cleanup=pending error=cleanup_failed"
    }};
    for (std::size_t i = 0; i < failures.size(); ++i) {
        const auto& outcome = failures[i];
        const auto count = capture.spool.size();
        assert(capture.reporter.report(typed(outcome)) == Disposition::observed_failure);
        assert(capture.spool.size() == count + 1);
        capture.last("failed", std::string("dispatch=outcome_callback_invoked ") + failure_details[i]);
    }
    std::cout << "Complete predicates and nearby incomplete facts passed; uncertain-age cases are supplied contracts only.\n";
}

std::uint64_t now_ms() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}
struct Storage {
    std::filesystem::path root, outbox, state, marker;
    explicit Storage(const std::string& tag) {
        root = std::filesystem::temp_directory_path() / ("nstu-disconnect-report-" + tag + "-" +
            std::to_string(GetCurrentProcessId()) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
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
    enable.package_id = "disconnect-report-private-package";
    enable.package_digest[0] = std::byte{1}; enable.client_id[0] = std::byte{2}; enable.session_id[0] = std::byte{3};
    enable.candidate_id = "disconnect-report-private-candidate"; enable.server_time_unix_milliseconds = now;
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
void exact(const Snapshot& snapshot, Outcome expected) {
    assert(snapshot.dispatch == Dispatch::outcome_callback_invoked && snapshot.outcome);
    const auto& actual = *snapshot.outcome;
    assert(actual.before == expected.before && actual.after == expected.after && actual.pending == expected.pending);
    assert(actual.publication == expected.publication && actual.cleanup == expected.cleanup && actual.current_failure == expected.current_failure);
}

void test_native_state_failure_and_recovery() {
    Storage storage("state"); const auto now = now_ms();
    Coordinator coordinator([now] { return now; }); int forwards = 0;
    Router router(coordinator, [&](std::span<const std::byte>) { ++forwards; }); Capture capture;
    assert(router.handle_restart(storage.outbox));
    assert(capture.reporter.report(failure()) == Disposition::observed_failure);
    const auto disabled = router.handle_disconnect_with_outcome();
    exact(disabled, {State::disabled, State::disabled, Pending::not_present, Publication::current_protected_state_confirmed, Cleanup::not_needed, Error::none});
    assert(capture.reporter.report(disabled) == Disposition::no_applicable_work);
    assert(capture.reporter.report(failure()) == Disposition::observed_failure && capture.spool.size() == 1);
    const auto enable = enable_for(now);
    assert(router.handle_server_enable(nstu::exam::encode_telemetry_enable(enable)));
    assert(router.handle_agent_samples(std::vector{sample_for(now)}, now));
    const auto before_state = raw_file(storage.state), before_outbox = raw_file(storage.outbox);
    Snapshot failed;
    {
        LockedFile held(storage.state);
        failed = router.handle_disconnect_with_outcome();
        exact(failed, {State::enabled, State::suspended, Pending::not_present, Publication::unconfirmed, Cleanup::not_needed, Error::protected_state_failed});
        assert(raw_file(storage.state) == before_state && raw_file(storage.outbox) == before_outbox);
        const auto protected_state = state_payload(storage);
        assert(protected_state[108 + enable.package_id.size() + enable.candidate_id.size()] == static_cast<std::byte>(nstu::client::TelemetryLifecycleState::enabled));
        assert(capture.reporter.report(failed) == Disposition::observed_failure);
        capture.last("failed", "dispatch=outcome_callback_invoked before=enabled after=suspended pending=not_present "
            "p=unconfirmed cleanup=not_needed error=protected_state_failed");
        assert(raw_file(storage.state) == before_state && raw_file(storage.outbox) == before_outbox);
        assert(coordinator.next_sequence() == 2 && coordinator.outbox_size() == 1 && forwards == 1);
    }
    const auto history = coordinator.last_error(); assert(!history.empty());
    const auto recovered = router.handle_disconnect_with_outcome();
    exact(recovered, {State::suspended, State::suspended, Pending::not_present, Publication::current_protected_state_confirmed, Cleanup::not_needed, Error::none});
    const auto persisted = raw_file(storage.state);
    assert(capture.reporter.report(recovered) == Disposition::complete && capture.spool.size() == 2);
    assert(coordinator.last_error() == history && raw_file(storage.state) == persisted && raw_file(storage.outbox) == before_outbox);
    assert(!router.poll_outbound_batch(now) && forwards == 1);
    assert(capture.reporter.report(router.handle_disconnect_with_outcome()) == Disposition::complete);
    assert(capture.spool.size() == 2);
    std::cout << "Native disabled, protected state failure, immutable reporting and repeated current recovery passed.\n";
}

void test_native_pending_expiry_and_marker_recovery() {
    for (const bool block_state : {false, true}) {
        Storage storage(block_state ? "marker" : "expiry"); auto now = now_ms();
        Coordinator coordinator([&] { return now; }); int forwards = 0;
        Router router(coordinator, [&](std::span<const std::byte>) { ++forwards; }); Capture capture;
        const auto enable = enable_for(now); const auto raw_enable = nstu::exam::encode_telemetry_enable(enable);
        assert(router.handle_restart(storage.outbox) && router.handle_server_enable(raw_enable));
        assert(router.handle_agent_samples(std::vector{sample_for(now)}, now));
        auto sample = sample_for(now); sample.collector_id = 42;
        const nstu::exam::TelemetryBatch pending{enable.package_id, enable.package_digest, enable.client_id,
            enable.session_id, enable.candidate_id, coordinator.active_producer_id(), 2, now, std::vector{sample}};
        const auto encoded = nstu::exam::encode_telemetry_batch(pending);
        const auto before_outbox = raw_file(storage.outbox);
        {
            LockedFile held(storage.outbox);
            assert(!router.handle_agent_samples(pending.samples, now));
            assert(coordinator.next_sequence() == 2 && coordinator.outbox_size() == 1);
            const auto wal = state_payload(storage);
            assert(!encoded.empty() && wal.size() >= encoded.size() && std::equal(encoded.rbegin(), encoded.rend(), wal.rbegin()));
            assert(raw_file(storage.outbox) == before_outbox);
        }
        const auto before_state = raw_file(storage.state);
        now += nstu::exam::kTelemetryRetentionMilliseconds;
        Snapshot expired;
        if (block_state) {
            LockedFile held(storage.state);
            expired = router.handle_disconnect_with_outcome();
            exact(expired, {State::enabled, State::revoked, Pending::revoked_expired, Publication::refusal_marker_confirmed, Cleanup::pending, Error::protected_state_failed});
            assert(raw_file(storage.state) == before_state && raw_file(storage.marker) == std::vector{std::byte{1}});
            physical_empty_outbox(storage); // Direct checksum/count inspection before any aged open.
            const auto outbox_after = raw_file(storage.outbox);
            assert(capture.reporter.report(expired) == Disposition::observed_failure);
            capture.last("failed", "dispatch=outcome_callback_invoked before=enabled after=revoked pending=revoked_expired "
                "p=refusal_marker_confirmed cleanup=pending error=protected_state_failed");
            assert(raw_file(storage.state) == before_state && raw_file(storage.outbox) == outbox_after);
            assert(raw_file(storage.marker) == std::vector{std::byte{1}} && coordinator.next_sequence() == 2 && forwards == 1);
        } else {
            expired = router.handle_disconnect_with_outcome();
            exact(expired, {State::enabled, State::revoked, Pending::revoked_expired, Publication::current_protected_state_confirmed, Cleanup::complete, Error::none});
            assert(!coordinator.last_error().empty());
            assert(capture.reporter.report(expired) == Disposition::complete && capture.spool.empty());
        }
        const auto history = coordinator.last_error();
        const auto recovered = router.handle_disconnect_with_outcome();
        exact(recovered, {State::revoked, State::revoked, Pending::not_present, Publication::current_protected_state_confirmed,
            block_state ? Cleanup::complete : Cleanup::not_needed, Error::none});
        const auto state_after = raw_file(storage.state);
        assert(capture.reporter.report(recovered) == Disposition::complete);
        assert(coordinator.last_error() == history && raw_file(storage.state) == state_after);
        physical_empty_outbox(storage);
        assert(!std::filesystem::exists(storage.marker) && !router.handle_server_enable(raw_enable));
        assert(!router.poll_outbound_batch(now) && forwards == 1 && coordinator.outbox_size() == 0);
        assert(capture.spool.size() == (block_state ? std::size_t{1} : std::size_t{0}));
    }
    std::cout << "Native retained WAL, current expiry, fresh refusal marker and actual cleanup/recovery passed.\n";
}

void test_native_normal_suspension() {
    Storage storage("normal"); const auto now = now_ms(); const auto enable = enable_for(now);
    Coordinator coordinator([now] { return now; }); int forwards = 0;
    Router router(coordinator, [&](std::span<const std::byte>) { ++forwards; }); Capture capture;
    assert(router.handle_restart(storage.outbox) && router.handle_server_enable(nstu::exam::encode_telemetry_enable(enable)));
    assert(router.handle_agent_samples(std::vector{sample_for(now)}, now));
    const auto outbox = raw_file(storage.outbox);
    const auto suspended = router.handle_disconnect_with_outcome();
    exact(suspended, {State::enabled, State::suspended, Pending::not_present, Publication::current_protected_state_confirmed, Cleanup::not_needed, Error::none});
    const auto state = raw_file(storage.state);
    assert(capture.reporter.report(suspended) == Disposition::complete);
    assert(raw_file(storage.state) == state && raw_file(storage.outbox) == outbox);
    assert(capture.reporter.report(router.handle_disconnect_with_outcome()) == Disposition::complete);
    assert(capture.spool.empty() && capture.debug.empty() && forwards == 1);
    assert(!router.poll_outbound_batch(now) && coordinator.next_sequence() == 2 && coordinator.outbox_size() == 1);
    std::cout << "Native normal/repeated suspension is quiet and leaves retained state untouched.\n";
}

void test_sink_exceptions_retry_and_bounded_spool() {
    for (const bool throw_debug : {false, true}) {
        for (const bool throw_audit : {false, true}) {
            nstu::audit::Spool spool(1);
            unsigned debug_calls = 0, audit_calls = 0;
            std::uint64_t assigned = 0;
            Reporter reporter([&](const nstu::audit::Event& event) {
                ++audit_calls;
                if (throw_audit && audit_calls == 1) throw std::runtime_error("trusted sink fails before delegation");
                assigned = spool.emit(event); return assigned;
            }, [&](const std::string&) {
                ++debug_calls; if (throw_debug) throw std::runtime_error("trusted debug failure");
            });
            assert(reporter.report(failure()) == Disposition::observed_failure);
            assert(debug_calls == 1 && audit_calls == 1 && spool.size() == (throw_audit ? std::size_t{0} : std::size_t{1}));
            assert(reporter.report(typed({})) == Disposition::no_applicable_work);
            assert(reporter.report(typed({State::disabled, State::disabled, Pending::not_present,
                Publication::current_protected_state_confirmed, Cleanup::not_needed, Error::none})) == Disposition::no_applicable_work);
            assert(reporter.report(failure()) == Disposition::observed_failure);
            assert(debug_calls == 1 && audit_calls == (throw_audit ? 2u : 1u) && spool.size() == 1 && assigned == 1);
            const auto records = nstu::audit::decode_audit_chunk(nstu::audit::encode_audit_chunk(spool.peek_chunk()));
            assert(records && records->size() == 1 && records->front().sequence == assigned && records->front().event.detail == kFailure);
            const auto ack = nstu::audit::decode_audit_ack(nstu::audit::encode_audit_ack(assigned));
            assert(ack && *ack == assigned); spool.acknowledge(*ack); assert(spool.empty());
            assert(reporter.report(failure()) == Disposition::observed_failure && spool.empty());
            nstu::audit::Event unrelated; unrelated.component = "unrelated";
            (void)spool.emit(unrelated); (void)spool.emit(unrelated);
            assert(spool.size() == 1 && spool.dropped_count() == 1);
            const auto before = spool.peek_chunk().front().sequence;
            assert(reporter.report(failure()) == Disposition::observed_failure);
            assert(spool.peek_chunk().front().sequence == before && debug_calls == 1);
            assert(reporter.report({}) == Disposition::outcome_unavailable);
            assert(reporter.report(failure()) == Disposition::observed_failure && debug_calls == 3);
        }
    }
    nstu::audit::Spool shared;
    Reporter first([&](const nstu::audit::Event& event) { return shared.emit(event); }, [](const std::string&) {});
    Reporter second([&](const nstu::audit::Event& event) { return shared.emit(event); }, [](const std::string&) {});
    assert(first.report(failure()) == Disposition::observed_failure);
    assert(second.report(failure()) == Disposition::observed_failure && shared.size() == 2);
    std::cout << "Trusted sink/debug exceptions, real acceptance retry/ACK/eviction and independent epochs passed.\n";
}

void test_serialization_and_controlled_epoch_order() {
    nstu::audit::Spool spool;
    std::atomic<unsigned> active{0}, debug_calls{0}, audit_calls{0};
    Reporter reporter([&](const nstu::audit::Event& event) {
        assert(active.fetch_add(1) == 0); ++audit_calls;
        const auto assigned = spool.emit(event);
        assert(active.fetch_sub(1) == 1); return assigned;
    }, [&](const std::string&) {
        assert(active.fetch_add(1) == 0); ++debug_calls; assert(active.fetch_sub(1) == 1);
    });
    constexpr std::size_t count = 8;
    std::barrier start(static_cast<std::ptrdiff_t>(count + 1));
    const auto observation = failure(); std::vector<std::thread> threads;
    for (std::size_t i = 0; i < count; ++i) threads.emplace_back([&] {
        start.arrive_and_wait(); assert(reporter.report(observation) == Disposition::observed_failure);
    });
    start.arrive_and_wait(); for (auto& thread : threads) thread.join();
    assert(debug_calls == 1 && audit_calls == 1 && spool.size() == 1 && active == 0);

    std::mutex mutex; std::condition_variable cv;
    bool entered = false, second_ready = false, release = false;
    std::vector<std::string> order; nstu::audit::Spool ordered_spool;
    Reporter ordered([&](const nstu::audit::Event& event) {
        std::unique_lock lock(mutex); order.push_back(event.result);
        if (order.size() == 1) {
            entered = true; cv.notify_all();
            assert(cv.wait_for(lock, std::chrono::seconds(5), [&] { return release; }));
        }
        return ordered_spool.emit(event);
    }, [](const std::string&) {});
    std::thread a([&] { assert(ordered.report(observation) == Disposition::observed_failure); });
    { std::unique_lock lock(mutex); assert(cv.wait_for(lock, std::chrono::seconds(5), [&] { return entered; })); }
    std::thread b([&] {
        { std::lock_guard lock(mutex); second_ready = true; cv.notify_all(); }
        assert(ordered.report({}) == Disposition::outcome_unavailable);
    });
    {
        std::unique_lock lock(mutex); assert(cv.wait_for(lock, std::chrono::seconds(5), [&] { return second_ready; }));
        release = true; cv.notify_all();
    }
    a.join(); b.join();
    assert(ordered.report(observation) == Disposition::observed_failure);
    assert(order == std::vector<std::string>({"failed", "outcome_unavailable", "failed"}) && ordered_spool.size() == 3);
    std::cout << "Barrier-start identical observations and bounded handshake A-B-A mutex order passed.\n";
}
} // namespace

int main() {
    test_interface_and_current_notice();
    test_dispatch_and_all_current_fields();
    test_complete_predicates_and_nearby_failures();
    test_native_state_failure_and_recovery();
    test_native_pending_expiry_and_marker_recovery();
    test_native_normal_suspension();
    test_sink_exceptions_retry_and_bounded_spool();
    test_serialization_and_controlled_epoch_order();
    return 0;
}
