#include "nstu/exam_telemetry_stop_reporting.hpp"
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
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using Reporter = nstu::client::ExamTelemetryStopReporter;
using Disposition = nstu::client::TelemetryStopReportDisposition;
using Snapshot = nstu::client::TelemetryStopDispatchResult;
using Dispatch = nstu::client::TelemetryStopDispatch;
using Outcome = nstu::client::TelemetryStopOutcome;
using App = nstu::client::TelemetryStopApplicability;
using Memory = nstu::client::TelemetryStopMemoryRevocation;
using Witness = nstu::client::TelemetryStopDurableWitness;
using Cleanup = nstu::client::TelemetryStopCleanup;
using Error = nstu::client::TelemetryStopError;
using Coordinator = nstu::client::ExamTelemetryServiceCoordinator;
using Router = nstu::client::ExamTelemetryRouter;
constexpr std::string_view kPrefix = "NSTU exam telemetry stop: ";

static_assert(noexcept(std::declval<Reporter&>().report(std::declval<const Snapshot&>())));
static_assert(std::is_same_v<decltype(std::declval<Reporter&>().report(std::declval<const Snapshot&>())), Disposition>);
static_assert(!std::is_copy_constructible_v<Reporter> && !std::is_copy_assignable_v<Reporter>);
static_assert(!std::is_move_constructible_v<Reporter> && !std::is_move_assignable_v<Reporter>);

Snapshot typed(Outcome outcome) { return {Dispatch::outcome_callback_invoked, outcome}; }
Outcome complete_outcome() {
    return {App::applicable, Memory::revoked_now, Witness::protected_state_confirmed, Cleanup::complete, Error::none};
}
Snapshot failure_a() {
    return typed({App::applicable, Memory::revoked_now, Witness::protected_state_confirmed,
                  Cleanup::pending, Error::cleanup_failed});
}

void assert_event(const nstu::audit::Event& event, const std::string& result, const std::string& detail) {
    assert(event.category == nstu::audit::Category::exam);
    assert(event.severity == nstu::audit::Severity::warning);
    assert(event.component == "exam_telemetry" && event.action == "explicit_stop");
    assert(event.result == result && event.detail == detail);
    assert(event.operation_id == 0 && event.client_id == 0);
    assert(event.component.size() <= nstu::audit::kMaximumFieldBytes);
    assert(event.action.size() <= nstu::audit::kMaximumFieldBytes);
    assert(event.result.size() <= nstu::audit::kMaximumFieldBytes);
    assert(event.detail.size() <= nstu::audit::kMaximumDetailBytes);
    assert(event.detail.find("<redacted>") == std::string::npos);
    assert(event.detail.find("...") == std::string::npos);
    assert(event.detail.find("durable_witness=") == std::string::npos);
    assert(event.detail.find_first_of("\t\r\n") == std::string::npos);
    assert(event.detail.find("  ") == std::string::npos);
    assert(nstu::audit::sanitize_event(event).detail == detail);
}

struct Capture {
    nstu::audit::Spool spool;
    std::vector<nstu::audit::Event> events;
    std::vector<std::string> debug;
    std::vector<char> order;
    Reporter reporter;
    Capture() : reporter([this](const nstu::audit::Event& event) {
        order.push_back('a');
        events.push_back(event);
        return spool.emit(event);
    }, [this](const std::string& line) { order.push_back('d'); debug.push_back(line); }) {}

    void last(const std::string& result, const std::string& detail) const {
        assert(!events.empty() && !debug.empty());
        assert_event(events.back(), result, detail);
        const auto expected = std::string(kPrefix) + result + " " + detail + "\n";
        assert(debug.back() == expected);
        assert(debug.back().size() <= kPrefix.size() + nstu::audit::kMaximumFieldBytes + 1 +
                                     nstu::audit::kMaximumDetailBytes + 1);
        assert(std::count(debug.back().begin(), debug.back().end(), '\n') == 1);
        const auto records = spool.peek_chunk();
        assert(!records.empty());
        assert_event(records.back().event, result, detail);
        const auto wire = nstu::audit::encode_audit_chunk(records);
        const auto decoded = nstu::audit::decode_audit_chunk(wire);
        assert(decoded && decoded->size() == records.size());
        for (std::size_t i = 0; i < records.size(); ++i) {
            assert((*decoded)[i].sequence == records[i].sequence);
            assert((*decoded)[i].timestamp_unix_milliseconds == records[i].timestamp_unix_milliseconds);
            assert_event((*decoded)[i].event, records[i].event.result, records[i].event.detail);
        }
    }
};

const std::string kFailureA = "dispatch=outcome_callback_invoked app=applicable memory=revoked_now "
    "witness=protected_state_confirmed cleanup=pending error=cleanup_failed";

void test_construction_dispatch_and_missing_controls() {
    bool rejected_emitter = false;
    bool rejected_debug = false;
    try { Reporter reporter({}, [](const std::string&) {}); }
    catch (const std::invalid_argument&) { rejected_emitter = true; }
    try { Reporter reporter([](const nstu::audit::Event&) { return std::uint64_t{1}; }, {}); }
    catch (const std::invalid_argument&) { rejected_debug = true; }
    assert(rejected_emitter && rejected_debug);
    nstu::audit::Spool noop_spool;
    Reporter noop([&](const nstu::audit::Event& event) { return noop_spool.emit(event); }, [](const std::string&) {});
    assert(noop.report(failure_a()) == Disposition::observed_failure && noop_spool.size() == 1);

    Capture capture;
    Router absent;
    assert(capture.reporter.report(absent.handle_explicit_stop_with_outcome()) == Disposition::outcome_unavailable);
    capture.last("outcome_unavailable", "dispatch=not_configured outcome=unavailable");
    // Absent/legacy dispatch ignores even forged attached completion and unknown data.
    assert(capture.reporter.report({Dispatch::not_configured, complete_outcome()}) == Disposition::outcome_unavailable);
    assert(capture.events.size() == 1);
    auto unknown_attached = complete_outcome();
    unknown_attached.error = static_cast<Error>(255);
    assert(capture.reporter.report({Dispatch::not_configured, unknown_attached}) == Disposition::outcome_unavailable);
    assert(capture.events.size() == 1);
    unsigned legacy_calls = 0;
    nstu::client::ExamTelemetryRouterCallbacks callbacks;
    callbacks.on_explicit_stop = [&] { ++legacy_calls; };
    Router legacy(std::move(callbacks));
    assert(capture.reporter.report(legacy.handle_explicit_stop_with_outcome()) == Disposition::outcome_unavailable);
    assert(legacy_calls == 1);
    capture.last("outcome_unavailable", "dispatch=legacy_callback_invoked outcome=unavailable");
    assert(capture.reporter.report({Dispatch::legacy_callback_invoked, complete_outcome()}) == Disposition::outcome_unavailable);
    assert(capture.events.size() == 2);
    assert(capture.reporter.report({Dispatch::legacy_callback_invoked, unknown_attached}) == Disposition::outcome_unavailable);
    assert(capture.events.size() == 2);
    assert(capture.reporter.report({Dispatch::outcome_callback_invoked, std::nullopt}) == Disposition::outcome_unavailable);
    capture.last("outcome_unavailable", "dispatch=outcome_callback_invoked outcome=unavailable");
    assert(capture.reporter.report({static_cast<Dispatch>(255), std::nullopt}) == Disposition::observed_failure);
    capture.last("failed", "dispatch=unknown outcome=unavailable");
    assert(capture.reporter.report({static_cast<Dispatch>(255), complete_outcome()}) == Disposition::observed_failure);
    capture.last("failed", "dispatch=unknown app=applicable memory=revoked_now witness=protected_state_confirmed cleanup=complete error=none");
    assert(capture.order == std::vector<char>({'d','a','d','a','d','a','d','a','d','a'}));
    std::cout << "Construction, absent/legacy routers, ignored attachments, missing and unknown dispatch passed.\n";
}

void test_known_unknown_classification_and_forwarding() {
    // Independent one-field controls, not native storage evidence or a tuple validator.
    const std::array<Outcome, 10> controls{{
        {App::closed, Memory::revoked_now, Witness::protected_state_confirmed, Cleanup::complete, Error::none},
        {App::unbound, Memory::revoked_now, Witness::protected_state_confirmed, Cleanup::complete, Error::none},
        {App::applicable, Memory::not_applicable, Witness::protected_state_confirmed, Cleanup::complete, Error::none},
        {App::applicable, Memory::revoked_now, Witness::not_attempted, Cleanup::complete, Error::none},
        {App::applicable, Memory::revoked_now, Witness::refusal_marker_confirmed, Cleanup::complete, Error::none},
        {App::applicable, Memory::revoked_now, Witness::unconfirmed, Cleanup::complete, Error::none},
        {App::applicable, Memory::revoked_now, Witness::protected_state_confirmed, Cleanup::not_needed, Error::none},
        {App::applicable, Memory::revoked_now, Witness::protected_state_confirmed, Cleanup::complete, Error::protected_state_failed},
        {App::applicable, Memory::revoked_now, Witness::protected_state_confirmed, Cleanup::complete, Error::revocation_witness_unconfirmed},
        {App::applicable, Memory::revoked_now, Witness::protected_state_confirmed, Cleanup::complete, Error::cleanup_failed}
    }};
    const std::array<std::string, 10> details{{
        "app=closed memory=revoked_now witness=protected_state_confirmed cleanup=complete error=none",
        "app=unbound memory=revoked_now witness=protected_state_confirmed cleanup=complete error=none",
        "app=applicable memory=not_applicable witness=protected_state_confirmed cleanup=complete error=none",
        "app=applicable memory=revoked_now witness=not_attempted cleanup=complete error=none",
        "app=applicable memory=revoked_now witness=refusal_marker_confirmed cleanup=complete error=none",
        "app=applicable memory=revoked_now witness=unconfirmed cleanup=complete error=none",
        "app=applicable memory=revoked_now witness=protected_state_confirmed cleanup=not_needed error=none",
        "app=applicable memory=revoked_now witness=protected_state_confirmed cleanup=complete error=protected_state_failed",
        "app=applicable memory=revoked_now witness=protected_state_confirmed cleanup=complete error=revocation_witness_unconfirmed",
        "app=applicable memory=revoked_now witness=protected_state_confirmed cleanup=complete error=cleanup_failed"
    }};
    Capture capture;
    for (std::size_t i = 0; i < controls.size(); ++i) {
        assert(capture.reporter.report(typed(controls[i])) == Disposition::observed_failure);
        capture.last("failed", "dispatch=outcome_callback_invoked " + details[i]);
    }
    const std::array<Outcome, 5> unknowns{{
        {static_cast<App>(255), Memory::revoked_now, Witness::protected_state_confirmed, Cleanup::complete, Error::none},
        {App::applicable, static_cast<Memory>(255), Witness::protected_state_confirmed, Cleanup::complete, Error::none},
        {App::applicable, Memory::revoked_now, static_cast<Witness>(255), Cleanup::complete, Error::none},
        {App::applicable, Memory::revoked_now, Witness::protected_state_confirmed, static_cast<Cleanup>(255), Error::none},
        {App::applicable, Memory::revoked_now, Witness::protected_state_confirmed, Cleanup::complete, static_cast<Error>(255)}
    }};
    const std::array<std::string, 5> unknown_details{{
        "app=unknown memory=revoked_now witness=protected_state_confirmed cleanup=complete error=none",
        "app=applicable memory=unknown witness=protected_state_confirmed cleanup=complete error=none",
        "app=applicable memory=revoked_now witness=unknown cleanup=complete error=none",
        "app=applicable memory=revoked_now witness=protected_state_confirmed cleanup=unknown error=none",
        "app=applicable memory=revoked_now witness=protected_state_confirmed cleanup=complete error=unknown"
    }};
    for (std::size_t i = 0; i < unknowns.size(); ++i) {
        assert(capture.reporter.report(typed(unknowns[i])) == Disposition::observed_failure);
        capture.last("failed", "dispatch=outcome_callback_invoked " + unknown_details[i]);
    }
    assert(capture.events.size() == controls.size() + unknowns.size());
    std::cout << "All five fields, known witness/error sanitation, unknown controls and real wire roundtrips passed.\n";
}

std::uint64_t now_ms() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}
struct Storage {
    std::filesystem::path root;
    std::filesystem::path outbox;
    std::filesystem::path state;
    std::filesystem::path marker;
    explicit Storage(const std::string& tag) {
        root = std::filesystem::temp_directory_path() / ("nstu-stop-report-" + tag + "-" +
            std::to_string(GetCurrentProcessId()) + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        assert(std::filesystem::create_directories(root));
        outbox = root / "telemetry.bin";
        state = outbox.wstring() + L".coord_state";
        marker = state.wstring() + L".blocked";
    }
    ~Storage() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
        assert(!error);
    }
};
class LockedFile {
public:
    explicit LockedFile(const std::filesystem::path& path)
        : handle_(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)) {
        assert(handle_ != INVALID_HANDLE_VALUE);
    }
    ~LockedFile() { assert(CloseHandle(handle_)); }
    LockedFile(const LockedFile&) = delete;
    LockedFile& operator=(const LockedFile&) = delete;
private:
    HANDLE handle_;
};

std::vector<std::byte> read_file(const std::filesystem::path& path) {
    const auto attributes = GetFileAttributesW(path.c_str());
    assert(attributes != INVALID_FILE_ATTRIBUTES);
    assert((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0);
    const auto size = std::filesystem::file_size(path);
    assert(size <= 4096);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    std::ifstream input(path, std::ios::binary);
    assert(input.is_open());
    if (!bytes.empty()) input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    assert(input.good());
    return bytes;
}
void create_marker(const std::filesystem::path& path, bool empty) {
    const auto handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    assert(handle != INVALID_HANDLE_VALUE);
    const std::array<std::byte, 3> sentinel{std::byte{0x24}, std::byte{0x68}, std::byte{0xac}};
    if (!empty) {
        DWORD written = 0;
        assert(WriteFile(handle, sentinel.data(), static_cast<DWORD>(sentinel.size()), &written, nullptr));
        assert(written == sentinel.size());
    }
    assert(FlushFileBuffers(handle));
    assert(CloseHandle(handle));
}
std::vector<std::byte> protected_payload(const Storage& storage) {
    assert(!read_file(storage.state).empty());
    constexpr std::string_view entropy = "NSTU-Exam-Telemetry-Service-Coordinator-v1";
    auto payload = nstu::security::load_machine_secret(storage.state.wstring(),
        {reinterpret_cast<const std::byte*>(entropy.data()), entropy.size()});
    assert(payload.size() > 40);
    const auto content_size = payload.size() - 32;
    const auto hash = nstu::security::sha256(std::span<const std::byte>(payload).first(content_size));
    assert(hash && std::equal(hash->begin(), hash->end(), payload.begin() + static_cast<std::ptrdiff_t>(content_size)));
    payload.resize(content_size);
    assert(payload[0] == std::byte{0x53} && payload[1] == std::byte{0x4d});
    assert(payload[2] == std::byte{0x4c} && payload[3] == std::byte{0x54});
    assert(payload[4] == std::byte{3} && payload[5] == std::byte{0});
    return payload;
}
nstu::exam::TelemetryEnable enable_for(std::uint64_t now) {
    nstu::exam::TelemetryEnable enable;
    enable.package_id = "stop-report-private-package";
    enable.package_digest[0] = std::byte{1};
    enable.client_id[0] = std::byte{2};
    enable.session_id[0] = std::byte{3};
    enable.candidate_id = "stop-report-private-candidate";
    enable.server_time_unix_milliseconds = now;
    return enable;
}
nstu::exam::TelemetrySample sample_for(std::uint64_t now) {
    nstu::exam::TelemetrySample sample;
    sample.kind = nstu::exam::TelemetrySampleKind::question_dwell;
    sample.source = nstu::exam::TelemetrySource::browser;
    sample.collector_id = 41;
    sample.question_id = "private-question";
    sample.question_revision = 1;
    sample.monotonic_elapsed_milliseconds = 2000;
    sample.duration_milliseconds = 1000;
    sample.client_time_unix_milliseconds = now;
    sample.count = 1;
    sample.reason = nstu::exam::TelemetryReason::navigation_button;
    return sample;
}
std::size_t sequence_offset(const nstu::exam::TelemetryEnable& enable) {
    return 96 + enable.package_id.size() + enable.candidate_id.size();
}
void assert_revoked_state(const Storage& storage, const nstu::exam::TelemetryEnable& enable) {
    const auto payload = protected_payload(storage);
    const auto offset = sequence_offset(enable);
    assert(offset + 13 <= payload.size());
    assert(payload[offset + 12] == static_cast<std::byte>(nstu::client::TelemetryLifecycleState::revoked));
}
void assert_outcome(const Snapshot& snapshot, Memory memory, Witness witness, Cleanup cleanup, Error error) {
    assert(snapshot.dispatch == Dispatch::outcome_callback_invoked && snapshot.outcome);
    assert(snapshot.outcome->applicability == App::applicable);
    assert(snapshot.outcome->memory_revocation == memory);
    assert(snapshot.outcome->durable_witness == witness);
    assert(snapshot.outcome->cleanup == cleanup && snapshot.outcome->error == error);
}
void assert_no_upload(Router& router, Coordinator& coordinator, std::uint64_t now) {
    assert(coordinator.lifecycle_state() == nstu::client::TelemetryLifecycleState::revoked);
    assert(!coordinator.next_batch_to_send(now));
    assert(!router.poll_outbound_batch(now));
    assert(!router.handle_agent_samples(std::vector{sample_for(now)}, now));
}
void assert_old_refused(const Storage& storage, const nstu::exam::TelemetryEnable& enable, std::uint64_t now,
                        bool open_expected) {
    Coordinator reconstructed([now] { return now; });
    unsigned forwarding = 0;
    Router router(reconstructed, [&](std::span<const std::byte>) { ++forwarding; });
    assert(router.handle_restart(storage.outbox) == open_expected);
    assert(!router.handle_server_enable(enable, nstu::exam::encode_telemetry_enable(enable)));
    assert(!router.poll_outbound_batch(now));
    assert(forwarding == 0);
    if (open_expected) {
        assert(reconstructed.lifecycle_state() == nstu::client::TelemetryLifecycleState::revoked);
        assert(reconstructed.outbox_size() == 0);
    }
}
void setup(Router& router, Coordinator& coordinator, const Storage& storage,
           const nstu::exam::TelemetryEnable& enable, std::uint64_t now) {
    assert(router.handle_restart(storage.outbox));
    assert(router.handle_server_enable(enable, nstu::exam::encode_telemetry_enable(enable)));
    assert(router.handle_agent_samples(std::vector{sample_for(now)}, now));
    assert(coordinator.outbox_size() == 1);
}
void recovered(Capture& capture, Router& router, Coordinator& coordinator, const Storage& storage,
               const nstu::exam::TelemetryEnable& enable, std::uint64_t now) {
    const auto history = coordinator.last_error();
    assert(!history.empty());
    assert(router.handle_maintenance(now));
    const auto clean = router.handle_explicit_stop_with_outcome();
    assert_outcome(clean, Memory::already_revoked, Witness::protected_state_confirmed, Cleanup::complete, Error::none);
    const auto notices = capture.events.size();
    assert(capture.reporter.report(clean) == Disposition::complete && capture.events.size() == notices);
    assert(coordinator.last_error() == history);
    assert_no_upload(router, coordinator, now);
    assert(coordinator.outbox_size() == 0 && !std::filesystem::exists(storage.marker));
    assert_revoked_state(storage, enable);
    coordinator.close();
    assert_old_refused(storage, enable, now, true);
}

void test_real_quiet_and_reset_epochs() {
    Storage storage("quiet");
    const auto now = now_ms();
    const auto enable = enable_for(now);
    Coordinator coordinator([now] { return now; });
    Router router(coordinator, [](std::span<const std::byte>) {});
    const auto closed = router.handle_explicit_stop_with_outcome();
    assert(closed.outcome && closed.outcome->applicability == App::closed);
    Capture capture;
    assert(capture.reporter.report(closed) == Disposition::no_applicable_work);
    assert(router.handle_restart(storage.outbox));
    const auto state_before = read_file(storage.state);
    const auto payload_before = read_file(storage.outbox);
    const auto unbound = router.handle_explicit_stop_with_outcome();
    assert(unbound.outcome && unbound.outcome->applicability == App::unbound);
    assert(capture.reporter.report(unbound) == Disposition::no_applicable_work);
    assert(capture.events.empty() && capture.debug.empty());
    assert(read_file(storage.state) == state_before && read_file(storage.outbox) == payload_before);
    assert(capture.reporter.report(failure_a()) == Disposition::observed_failure);
    capture.last("failed", kFailureA);
    assert(capture.reporter.report(closed) == Disposition::no_applicable_work);
    assert(capture.reporter.report(unbound) == Disposition::no_applicable_work);
    assert(capture.reporter.report(failure_a()) == Disposition::observed_failure && capture.events.size() == 1);
    assert(router.handle_server_enable(enable, nstu::exam::encode_telemetry_enable(enable)));
    assert(router.handle_agent_samples(std::vector{sample_for(now)}, now));
    const auto clean = router.handle_explicit_stop_with_outcome();
    assert_outcome(clean, Memory::revoked_now, Witness::protected_state_confirmed, Cleanup::complete, Error::none);
    assert(capture.reporter.report(clean) == Disposition::complete && capture.events.size() == 1);
    assert_revoked_state(storage, enable);
    assert_no_upload(router, coordinator, now);
    const auto repeated = router.handle_explicit_stop_with_outcome();
    assert_outcome(repeated, Memory::already_revoked, Witness::protected_state_confirmed, Cleanup::complete, Error::none);
    assert(capture.reporter.report(repeated) == Disposition::complete);
    assert(capture.reporter.report(failure_a()) == Disposition::observed_failure && capture.events.size() == 2);
    capture.last("failed", kFailureA);
    coordinator.close();
    assert_old_refused(storage, enable, now, true);
    std::cout << "Real closed/unbound/normal/repeated stop facts, quiet preservation and complete reset passed.\n";
}

void test_native_marker_fallback() {
    for (const bool lock_payload : {false, true}) {
        Storage storage(lock_payload ? "fallback-payload" : "fallback");
        const auto now = now_ms();
        const auto enable = enable_for(now);
        Coordinator coordinator([now] { return now; });
        Router router(coordinator, [](std::span<const std::byte>) {});
        setup(router, coordinator, storage, enable, now);
        const auto state_before = read_file(storage.state);
        const auto payload_before = read_file(storage.outbox);
        Capture capture;
        {
            LockedFile state_lock(storage.state);
            std::unique_ptr<LockedFile> payload_lock;
            if (lock_payload) payload_lock = std::make_unique<LockedFile>(storage.outbox);
            const auto snapshot = router.handle_explicit_stop_with_outcome();
            assert_outcome(snapshot, Memory::revoked_now, Witness::refusal_marker_confirmed, Cleanup::pending, Error::protected_state_failed);
            assert(capture.reporter.report(snapshot) == Disposition::observed_failure);
            capture.last("failed", "dispatch=outcome_callback_invoked app=applicable memory=revoked_now "
                "witness=refusal_marker_confirmed cleanup=pending error=protected_state_failed");
            assert(read_file(storage.state) == state_before);
            assert(read_file(storage.marker) == std::vector<std::byte>{std::byte{1}});
            if (lock_payload) assert(read_file(storage.outbox) == payload_before);
            else { nstu::exam::TelemetryOutbox inspect; assert(inspect.open(storage.outbox, now, nullptr)); assert(inspect.size() == 0); }
            assert_no_upload(router, coordinator, now);
            assert_old_refused(storage, enable, now, false);
        }
        recovered(capture, router, coordinator, storage, enable, now);
        assert(capture.events.size() == 1);
        std::cout << "Native state sharing failure, confirmed marker, payload lock=" << lock_payload << " and recovery passed.\n";
    }
}

void test_native_existing_markers() {
    for (const bool empty : {false, true}) {
        Storage storage(empty ? "marker-empty" : "marker-nonempty");
        const auto now = now_ms();
        const auto enable = enable_for(now);
        Coordinator coordinator([now] { return now; });
        Router router(coordinator, [](std::span<const std::byte>) {});
        setup(router, coordinator, storage, enable, now);
        create_marker(storage.marker, empty);
        const auto marker_before = read_file(storage.marker);
        const auto state_before = read_file(storage.state);
        const auto payload_before = read_file(storage.outbox);
        Capture capture;
        {
            LockedFile state_lock(storage.state);
            LockedFile payload_lock(storage.outbox);
            const auto snapshot = router.handle_explicit_stop_with_outcome();
            assert_outcome(snapshot, Memory::revoked_now, Witness::unconfirmed, Cleanup::pending, Error::revocation_witness_unconfirmed);
            assert(capture.reporter.report(snapshot) == Disposition::observed_failure);
            capture.last("failed", "dispatch=outcome_callback_invoked app=applicable memory=revoked_now "
                "witness=unconfirmed cleanup=pending error=revocation_witness_unconfirmed");
            assert(read_file(storage.marker) == marker_before);
            assert(read_file(storage.state) == state_before && read_file(storage.outbox) == payload_before);
            assert_no_upload(router, coordinator, now);
            assert_old_refused(storage, enable, now, false);
            assert(read_file(storage.marker) == marker_before);
        }
        recovered(capture, router, coordinator, storage, enable, now);
        std::cout << "Native existing marker empty=" << empty << " unchanged, uncertainty, purge failure and reconstruction passed.\n";
    }
}

void test_native_cleanup_and_changed_memory() {
    for (const bool lock_marker : {false, true}) {
        Storage storage(lock_marker ? "marker-removal" : "payload-purge");
        const auto now = now_ms();
        const auto enable = enable_for(now);
        Coordinator coordinator([now] { return now; });
        Router router(coordinator, [](std::span<const std::byte>) {});
        setup(router, coordinator, storage, enable, now);
        const auto payload_before = read_file(storage.outbox);
        if (lock_marker) create_marker(storage.marker, false);
        Capture capture;
        {
            LockedFile cleanup_lock(lock_marker ? storage.marker : storage.outbox);
            const auto first = router.handle_explicit_stop_with_outcome();
            assert_outcome(first, Memory::revoked_now, Witness::protected_state_confirmed, Cleanup::pending, Error::cleanup_failed);
            assert(capture.reporter.report(first) == Disposition::observed_failure);
            capture.last("failed", kFailureA);
            assert_revoked_state(storage, enable);
            if (lock_marker) {
                assert(read_file(storage.marker) == std::vector<std::byte>({std::byte{0x24}, std::byte{0x68}, std::byte{0xac}}));
                nstu::exam::TelemetryOutbox inspect; assert(inspect.open(storage.outbox, now, nullptr)); assert(inspect.size() == 0);
            } else assert(read_file(storage.outbox) == payload_before);
            assert_no_upload(router, coordinator, now);
            const auto second = router.handle_explicit_stop_with_outcome();
            assert_outcome(second, Memory::already_revoked, Witness::protected_state_confirmed, Cleanup::pending, Error::cleanup_failed);
            assert(capture.reporter.report(second) == Disposition::observed_failure);
            capture.last("failed", "dispatch=outcome_callback_invoked app=applicable memory=already_revoked "
                "witness=protected_state_confirmed cleanup=pending error=cleanup_failed");
            assert(capture.reporter.report(second) == Disposition::observed_failure && capture.events.size() == 2);
        }
        recovered(capture, router, coordinator, storage, enable, now);
        assert(capture.events.size() == 2);
        std::cout << "Native cleanup sharing failure marker=" << lock_marker << ", changed memory epoch and sticky history recovery passed.\n";
    }
}

void test_native_second_state_write_mixed_witness() {
    Storage storage("second-state-write");
    const auto now = now_ms();
    const auto aged = now + nstu::exam::kTelemetryRetentionMilliseconds;
    const auto enable = enable_for(now);
    bool armed = false;
    unsigned clock_calls = 0;
    std::unique_ptr<LockedFile> state_lock;
    Coordinator coordinator([&] {
        if (!armed) return now;
        ++clock_calls;
        if (clock_calls == 1) return std::uint64_t{0};
        assert(clock_calls == 2);
        state_lock = std::make_unique<LockedFile>(storage.state);
        return aged;
    });
    Router router(coordinator, [](std::span<const std::byte>) {});
    setup(router, coordinator, storage, enable, now);
    router.handle_disconnect();
    assert(!router.handle_agent_samples(std::vector{sample_for(now)}, now));
    assert(coordinator.not_authorized_sample_count() == 1);
    assert(router.handle_server_enable(enable, nstu::exam::encode_telemetry_enable(enable)));
    const auto before = protected_payload(storage);
    const auto offset = sequence_offset(enable);
    assert(before[offset + 12] == static_cast<std::byte>(nstu::client::TelemetryLifecycleState::enabled));
    assert(before[offset + 29] == std::byte{1});
    const auto payload_before = read_file(storage.outbox);
    armed = true;
    const auto snapshot = router.handle_explicit_stop_with_outcome();
    armed = false;
    assert(clock_calls == 2 && state_lock);
    assert_outcome(snapshot, Memory::revoked_now, Witness::protected_state_confirmed, Cleanup::pending, Error::protected_state_failed);
    Capture capture;
    assert(capture.reporter.report(snapshot) == Disposition::observed_failure);
    capture.last("failed", "dispatch=outcome_callback_invoked app=applicable memory=revoked_now "
        "witness=protected_state_confirmed cleanup=pending error=protected_state_failed");
    const auto published = protected_payload(storage);
    assert(published[offset + 12] == static_cast<std::byte>(nstu::client::TelemetryLifecycleState::revoked));
    assert(std::equal(before.begin() + static_cast<std::ptrdiff_t>(offset + 13),
                      before.begin() + static_cast<std::ptrdiff_t>(offset + 45),
                      published.begin() + static_cast<std::ptrdiff_t>(offset + 13)));
    assert(coordinator.not_authorized_sample_count() == 0);
    assert(coordinator.dropped_sample_count() == 0 && coordinator.dropped_batch_count() == 0);
    assert(read_file(storage.outbox) == payload_before && !std::filesystem::exists(storage.marker));
    assert(coordinator.lifecycle_state() == nstu::client::TelemetryLifecycleState::revoked);
    assert(!coordinator.next_batch_to_send(aged) && !router.poll_outbound_batch(aged));
    state_lock.reset();
    // Read-only upload probes preserve the old counter window being inspected.
    const auto history = coordinator.last_error();
    assert(router.handle_maintenance(aged));
    const auto repaired = protected_payload(storage);
    assert(std::all_of(repaired.begin() + static_cast<std::ptrdiff_t>(offset + 13),
                       repaired.begin() + static_cast<std::ptrdiff_t>(offset + 45),
                       [](std::byte byte) { return byte == std::byte{0}; }));
    const auto clean = router.handle_explicit_stop_with_outcome();
    assert_outcome(clean, Memory::already_revoked, Witness::protected_state_confirmed, Cleanup::complete, Error::none);
    assert(capture.reporter.report(clean) == Disposition::complete && capture.events.size() == 1);
    assert(coordinator.last_error() == history && !history.empty());
    assert(!router.poll_outbound_batch(aged) && coordinator.outbox_size() == 0);
    coordinator.close();
    assert_old_refused(storage, enable, aged, true);
    std::cout << "Native second state write failed after confirmed publication; mixed witness/error, bytes and recovery passed.\n";
}

void test_sink_failures_quiet_preservation_and_acceptance() {
    Storage storage("sink-quiet");
    Coordinator coordinator;
    Router router(coordinator, [](std::span<const std::byte>) {});
    const auto closed = router.handle_explicit_stop_with_outcome();
    assert(router.handle_restart(storage.outbox));
    const auto unbound = router.handle_explicit_stop_with_outcome();
    for (const bool debug_throws : {false, true}) {
        nstu::audit::Spool spool;
        unsigned debug_calls = 0;
        unsigned audit_calls = 0;
        std::uint64_t assigned = 0;
        std::vector<char> order;
        Reporter reporter([&](const nstu::audit::Event& event) {
            order.push_back('a');
            if (++audit_calls == 1) throw std::runtime_error("controlled predelegation failure");
            assigned = spool.emit(event);
            return assigned;
        }, [&](const std::string& line) {
            assert(line == std::string(kPrefix) + "failed " + kFailureA + "\n");
            order.push_back('d');
            ++debug_calls;
            if (debug_throws) throw std::runtime_error("controlled debug failure");
        });
        assert(reporter.report(failure_a()) == Disposition::observed_failure);
        assert(spool.empty() && debug_calls == 1 && audit_calls == 1);
        assert(reporter.report(closed) == Disposition::no_applicable_work);
        assert(reporter.report(unbound) == Disposition::no_applicable_work);
        assert(reporter.report(failure_a()) == Disposition::observed_failure);
        assert(spool.size() == 1 && assigned == 1 && debug_calls == 1 && audit_calls == 2);
        assert(reporter.report(failure_a()) == Disposition::observed_failure);
        assert(debug_calls == 1 && audit_calls == 2 && spool.size() == 1);
        assert(order == std::vector<char>({'d', 'a', 'a'}));
        const auto records = spool.peek_chunk();
        assert(records.size() == 1 && records[0].sequence == assigned);
        assert_event(records[0].event, "failed", kFailureA);
    }
    nstu::audit::Spool spool;
    unsigned debug_calls = 0;
    unsigned audit_calls = 0;
    Reporter debug_failure([&](const nstu::audit::Event& event) { ++audit_calls; return spool.emit(event); },
        [&](const std::string&) { ++debug_calls; throw std::runtime_error("debug only"); });
    assert(debug_failure.report(failure_a()) == Disposition::observed_failure);
    assert(debug_failure.report(failure_a()) == Disposition::observed_failure);
    assert(debug_calls == 1 && audit_calls == 1 && spool.size() == 1);
    // A different current classification also survives both sink exceptions.
    Reporter unavailable_failure([](const nstu::audit::Event&) -> std::uint64_t { throw std::runtime_error("emit"); },
        [](const std::string&) { throw std::runtime_error("debug"); });
    assert(unavailable_failure.report({}) == Disposition::outcome_unavailable);
    std::cout << "Debug-first, emitter retry on next observation, combined throws and quiet flag preservation passed.\n";
}

void test_spool_ack_eviction_instances_and_epochs() {
    Capture capture;
    const auto a = failure_a();
    auto b = a;
    b.outcome->memory_revocation = Memory::already_revoked;
    assert(capture.reporter.report(a) == Disposition::observed_failure);
    assert(capture.reporter.report(b) == Disposition::observed_failure);
    assert(capture.reporter.report(a) == Disposition::observed_failure);
    assert(capture.events.size() == 3 && capture.debug.size() == 3);
    capture.last("failed", kFailureA);
    const auto last_sequence = capture.spool.peek_chunk().back().sequence;
    const auto ack = nstu::audit::decode_audit_ack(nstu::audit::encode_audit_ack(last_sequence));
    assert(ack && *ack == last_sequence);
    capture.spool.acknowledge(*ack);
    assert(capture.spool.empty());
    assert(capture.reporter.report(a) == Disposition::observed_failure && capture.spool.empty());
    assert(capture.events.size() == 3 && capture.debug.size() == 3);
    Reporter independent([&](const nstu::audit::Event& event) { return capture.spool.emit(event); }, [](const std::string&) {});
    assert(independent.report(a) == Disposition::observed_failure && capture.spool.size() == 1);
    assert(capture.reporter.report(a) == Disposition::observed_failure && capture.spool.size() == 1);

    nstu::audit::Spool small(2);
    unsigned calls = 0;
    Reporter reporter([&](const nstu::audit::Event& event) { ++calls; return small.emit(event); }, [](const std::string&) {});
    assert(reporter.report(a) == Disposition::observed_failure);
    auto unrelated = capture.events.front();
    unrelated.action = "independent_control";
    const auto second = small.emit(unrelated);
    const auto third = small.emit(unrelated);
    assert(small.size() == 2 && small.dropped_count() == 1 && third == second + 1);
    const auto retained = small.peek_chunk();
    assert(retained[0].sequence == second && retained[1].sequence == third);
    assert(reporter.report(a) == Disposition::observed_failure && calls == 1);
    assert(small.size() == 2 && small.dropped_count() == 1);
    const auto decoded = nstu::audit::decode_audit_chunk(nstu::audit::encode_audit_chunk(retained));
    assert(decoded && decoded->size() == 2 && (*decoded)[0].sequence == second && (*decoded)[1].sequence == third);
    small.acknowledge(third);
    assert(small.empty());
    assert(reporter.report(a) == Disposition::observed_failure && small.empty() && calls == 1);
    std::cout << "A-B-A epochs, independent instances, real ACK and drop-oldest without unsuppression passed.\n";
}

void test_concurrency_and_controlled_lock_order() {
    // All workers classify the same immutable native failure snapshot.
    Storage storage("concurrency");
    const auto now = now_ms();
    const auto enable = enable_for(now);
    Coordinator coordinator([now] { return now; });
    Router router(coordinator, [](std::span<const std::byte>) {});
    setup(router, coordinator, storage, enable, now);
    Snapshot a;
    {
        LockedFile lock(storage.outbox);
        a = router.handle_explicit_stop_with_outcome();
        assert_outcome(a, Memory::revoked_now, Witness::protected_state_confirmed, Cleanup::pending, Error::cleanup_failed);
    }
    nstu::audit::Spool spool;
    std::atomic<unsigned> active{0};
    unsigned debug_calls = 0;
    unsigned audit_calls = 0;
    std::vector<char> order;
    Reporter reporter([&](const nstu::audit::Event& event) {
        assert(active.fetch_add(1) == 0);
        ++audit_calls;
        order.push_back('a');
        const auto sequence = spool.emit(event);
        assert(active.fetch_sub(1) == 1);
        return sequence;
    }, [&](const std::string&) {
        assert(active.fetch_add(1) == 0);
        ++debug_calls;
        order.push_back('d');
        assert(active.fetch_sub(1) == 1);
    });
    constexpr unsigned workers = 8;
    std::barrier start(static_cast<std::ptrdiff_t>(workers));
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < workers; ++i) threads.emplace_back([&] {
        start.arrive_and_wait();
        assert(reporter.report(a) == Disposition::observed_failure);
    });
    for (auto& thread : threads) thread.join();
    assert(spool.size() == 1 && debug_calls == 1 && audit_calls == 1 && active == 0);
    assert(order == std::vector<char>({'d', 'a'}));

    auto b = a;
    b.outcome->memory_revocation = Memory::already_revoked;
    std::mutex control;
    std::condition_variable changed;
    bool first_inside = false;
    bool second_started = false;
    bool release = false;
    Capture observed;
    unsigned blocking_calls = 0;
    Reporter ordered([&](const nstu::audit::Event& event) { return observed.spool.emit(event); }, [&](const std::string& line) {
        std::unique_lock lock(control);
        observed.debug.push_back(line);
        if (++blocking_calls == 1) {
            first_inside = true;
            changed.notify_all();
            changed.wait(lock, [&] { return release; });
        }
    });
    std::thread first([&] { assert(ordered.report(a) == Disposition::observed_failure); });
    { std::unique_lock lock(control); changed.wait(lock, [&] { return first_inside; }); }
    std::thread second_thread([&] {
        { std::lock_guard lock(control); second_started = true; changed.notify_all(); }
        assert(ordered.report(b) == Disposition::observed_failure);
    });
    {
        std::unique_lock lock(control);
        changed.wait(lock, [&] { return second_started; });
        assert(observed.debug.size() == 1 && observed.spool.empty());
        release = true;
        changed.notify_all();
    }
    first.join();
    second_thread.join();
    assert(ordered.report(a) == Disposition::observed_failure);
    const auto records = observed.spool.peek_chunk();
    assert(records.size() == 3 && observed.debug.size() == 3);
    assert(records[0].sequence + 1 == records[1].sequence && records[1].sequence + 1 == records[2].sequence);
    assert(records[0].event.detail == kFailureA && records[2].event.detail == kFailureA);
    assert(records[1].event.detail == "dispatch=outcome_callback_invoked app=applicable memory=already_revoked "
        "witness=protected_state_confirmed cleanup=pending error=cleanup_failed");
    assert(router.handle_maintenance(now));
    coordinator.close();
    std::cout << "Barrier-start identical native snapshots and controlled mutex acquisition A-B-A order passed without sleeps.\n";
}
} // namespace

int main() {
    test_construction_dispatch_and_missing_controls();
    test_known_unknown_classification_and_forwarding();
    test_real_quiet_and_reset_epochs();
    test_native_marker_fallback();
    test_native_existing_markers();
    test_native_cleanup_and_changed_memory();
    test_native_second_state_write_mixed_witness();
    test_sink_failures_quiet_preservation_and_acceptance();
    test_spool_ack_eviction_instances_and_epochs();
    test_concurrency_and_controlled_lock_order();
    std::cout << "STOP-REPORTING-FOUNDATION-1 contract tests passed.\n";
    return 0;
}
