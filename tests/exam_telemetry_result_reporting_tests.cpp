#include "nstu/detail/exam_telemetry_result_reporting.hpp"
#include "nstu/telemetry.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using Reporter = nstu::client::detail::ExamTelemetryResultReporter;
using Operation = nstu::client::detail::TelemetryResultOperation;
static_assert(std::is_same_v<std::underlying_type_t<Operation>, std::size_t>);
static_assert(static_cast<std::size_t>(Operation::startup) == 0);
static_assert(static_cast<std::size_t>(Operation::enable) == 1);
static_assert(static_cast<std::size_t>(Operation::samples) == 2);
static_assert(static_cast<std::size_t>(Operation::ack) == 3);
static_assert(static_cast<std::size_t>(Operation::maintenance) == 4);
static_assert(static_cast<std::size_t>(Operation::shutdown) == 5);
static_assert(static_cast<std::size_t>(Operation::count) == 6);
static_assert(noexcept(std::declval<Reporter&>().report(Operation::startup, false, std::string_view{})));
static_assert(!std::is_copy_constructible_v<Reporter> && !std::is_copy_assignable_v<Reporter>);
static_assert(!std::is_move_constructible_v<Reporter> && !std::is_move_assignable_v<Reporter>);

struct Capture {
    nstu::audit::Spool spool;
    std::vector<nstu::audit::Event> events;
    std::vector<std::string> debug;
    std::vector<char> order;
    Reporter reporter;
    Capture() : reporter([this](const nstu::audit::Event& event) {
        order.push_back('a'); events.push_back(event); return spool.emit(event);
    }, [this](const std::string& line) { order.push_back('d'); debug.push_back(line); }) {}
    void notice(std::size_t index, const std::string& action, const std::string& detail) const {
        const auto records = spool.peek_chunk();
        const auto decoded = nstu::audit::decode_audit_chunk(nstu::audit::encode_audit_chunk(records));
        assert(decoded && decoded->size() == records.size());
        assert(index < decoded->size() && index < events.size() && index < debug.size());
        const auto& event = (*decoded)[index].event;
        assert(event.category == nstu::audit::Category::exam && event.severity == nstu::audit::Severity::warning);
        assert(event.component == "exam_telemetry" && event.action == action && event.result == "failed");
        if (event.detail != detail) {
            std::cerr << "Literal receipt mismatch for " << action << ": expected [" << detail
                      << "] actual [" << event.detail << "]\n";
        }
        assert(event.operation_id == 0 && event.client_id == 0 && event.detail == detail);
        assert(event.detail.size() <= nstu::audit::kMaximumDetailBytes);
        assert(events[index].detail == detail && events[index].action == action && events[index].result == "failed");
        assert(events[index].category == event.category && events[index].severity == event.severity);
        assert(events[index].component == event.component && events[index].operation_id == 0 && events[index].client_id == 0);
        assert(debug[index] == "NSTU exam telemetry " + action + " failed: " + detail + "\n");
        assert(std::count(debug[index].begin(), debug[index].end(), '\n') == 1);
        for (std::size_t i = 0; i < records.size(); ++i) {
            assert((*decoded)[i].sequence == records[i].sequence);
            assert((*decoded)[i].timestamp_unix_milliseconds == records[i].timestamp_unix_milliseconds);
            assert((*decoded)[i].event.detail == records[i].event.detail);
        }
    }
};

void test_constructor_operations_and_invalid_indices() {
    bool emitter_rejected = false, debug_rejected = false;
    try { Reporter invalid({}, [](const std::string&) {}); }
    catch (const std::invalid_argument&) { emitter_rejected = true; }
    try { Reporter invalid([](const nstu::audit::Event&) { return std::uint64_t{1}; }, {}); }
    catch (const std::invalid_argument&) { debug_rejected = true; }
    assert(emitter_rejected && debug_rejected);
    Capture capture;
    for (const auto operation : {Operation::count, static_cast<Operation>(std::numeric_limits<std::size_t>::max())}) {
        capture.reporter.report(operation, false, "private diagnostic must not enter a sink");
        capture.reporter.report(operation, true, "ignored");
    }
    assert(capture.events.empty() && capture.debug.empty() && capture.spool.empty());
    const std::array<Operation, 6> operations{Operation::startup, Operation::enable, Operation::samples, Operation::ack, Operation::maintenance, Operation::shutdown};
    const std::array<const char*, 6> names{"startup", "enable", "samples", "ack", "maintenance", "shutdown"};
    for (std::size_t i = 0; i < operations.size(); ++i) {
        capture.reporter.report(operations[i], true, "ignored success detail");
        assert(capture.spool.size() == i);
        capture.reporter.report(operations[i], false, {});
        capture.notice(i, names[i], "operation failed");
        capture.reporter.report(operations[i], false, {});
        assert(capture.spool.size() == i + 1);
    }
    assert((capture.order == std::vector<char>{'a', 'd', 'a', 'd', 'a', 'd', 'a', 'd', 'a', 'd', 'a', 'd'}));
    std::cout << "Constructor, six original operation positions, invalid/count guards and exact audit-before-debug grammar passed.\n";
}

void test_failed_insertion_retries_before_acceptance() {
    nstu::audit::Spool spool;
    unsigned audits = 0, debug = 0;
    std::uint64_t assigned = 0;
    std::vector<char> order;
    Reporter reporter([&](const nstu::audit::Event& event) {
        ++audits; order.push_back('a');
        if (audits == 1) throw std::runtime_error("fails before real Spool delegation");
        assigned = spool.emit(event); return assigned;
    }, [&](const std::string& line) {
        ++debug; order.push_back('d');
        assert(line == "NSTU exam telemetry samples failed: pending telemetry enqueue persistence failed\n");
    });
    reporter.report(Operation::samples, false, "pending telemetry enqueue persistence failed");
    assert(audits == 1 && debug == 0 && spool.empty() && assigned == 0);
    reporter.report(Operation::samples, false, "pending telemetry enqueue persistence failed");
    assert(audits == 2 && debug == 1 && spool.size() == 1 && assigned == 1);
    reporter.report(Operation::samples, false, "pending telemetry enqueue persistence failed");
    assert(audits == 2 && debug == 1 && spool.size() == 1);
    assert((order == std::vector<char>{'a', 'a', 'd'}));
    const auto decoded = nstu::audit::decode_audit_chunk(nstu::audit::encode_audit_chunk(spool.peek_chunk()));
    assert(decoded && decoded->size() == 1 && (*decoded)[0].sequence == assigned);
    assert((*decoded)[0].event.action == "samples" && (*decoded)[0].event.result == "failed");
    assert((*decoded)[0].event.detail == "pending telemetry enqueue persistence failed");
    std::cout << "Actual callable throw-before-Spool, identical retry/assigned record and accepted suppression passed.\n";
}
void test_operation_epochs_and_empty_sanitized_keys() {
    Capture capture;
    capture.reporter.report(Operation::startup, false, "disk failure");
    capture.reporter.report(Operation::enable, false, "disk failure");
    capture.reporter.report(Operation::startup, false, "disk failure");
    assert(capture.spool.size() == 2);
    capture.reporter.report(Operation::enable, true, "ignored");
    capture.reporter.report(Operation::startup, false, "disk failure");
    assert(capture.spool.size() == 2);
    capture.reporter.report(Operation::enable, false, "disk failure");
    capture.reporter.report(Operation::startup, false, "pending conflict");
    capture.reporter.report(Operation::startup, false, "disk failure");
    assert(capture.spool.size() == 5);
    capture.notice(0, "startup", "disk failure"); capture.notice(1, "enable", "disk failure");
    capture.notice(2, "enable", "disk failure"); capture.notice(3, "startup", "pending conflict"); capture.notice(4, "startup", "disk failure");
    Capture empty;
    empty.reporter.report(Operation::maintenance, false, "\r\n\t ");
    empty.notice(0, "maintenance", "");
    empty.reporter.report(Operation::maintenance, false, "\n");
    assert(empty.spool.size() == 1);
    empty.reporter.report(Operation::maintenance, true, "ignored");
    empty.reporter.report(Operation::maintenance, false, "\t");
    empty.notice(1, "maintenance", "");
    empty.reporter.report(Operation::maintenance, false, {});
    empty.notice(2, "maintenance", "operation failed");
    assert(empty.spool.size() == 3);
    std::cout << "Success/change/A-B-A independence and first empty sanitized key acceptance passed.\n";
}

void test_literal_privacy_and_owned_canonical_keys() {
    const std::array<std::pair<const char*, const char*>, 6> rows{{
        {"bad state password=hunter2 private answer", "bad state <redacted>"},
        {"retry 10.0.0.5 host", "retry <redacted> host"},
        {"owner name@example.test failed", "owner <redacted> failed"},
        {"C:/Users/test/secret.pem", "<redacted>"},
        {"SAS=123456 private clipboard", "<redacted>"},
        {"disk\tfailure", "disk failure"}
    }};
    for (const auto& [input, expected] : rows) {
        Capture capture;
        capture.reporter.report(Operation::samples, false, input);
        capture.notice(0, "samples", expected);
        const auto encoded = nstu::audit::encode_audit_chunk(capture.spool.peek_chunk());
        const std::string wire(reinterpret_cast<const char*>(encoded.data()), encoded.size());
        for (const auto canary : {"hunter2", "10.0.0.5", "name@example.test", "secret.pem", "123456", "private answer", "private clipboard"}) {
            assert(wire.find(canary) == std::string::npos);
            assert(capture.debug[0].find(canary) == std::string::npos);
            assert(capture.events[0].detail.find(canary) == std::string::npos);
        }
        std::cout << capture.debug[0];
    }
    Capture canonical;
    {
        std::string transient = "disk\tfailure";
        canonical.reporter.report(Operation::ack, false, transient);
        transient.assign(200, 'z');
    }
    canonical.notice(0, "ack", "disk failure");
    canonical.reporter.report(Operation::ack, false, "disk failure");
    assert(canonical.spool.size() == 1); // Key owns canonical text, not the input view.
    Capture bounded;
    std::string long_diagnostic;
    for (unsigned i = 0; i < 200; ++i) long_diagnostic += "disk ";
    bounded.reporter.report(Operation::startup, false, long_diagnostic);
    assert(bounded.events.size() == 1 && bounded.events[0].detail.size() <= nstu::audit::kMaximumDetailBytes);
    assert(bounded.events[0].detail.ends_with("...") && bounded.events[0].detail.find_first_of("\r\n\t") == std::string::npos);
    const auto decoded = nstu::audit::decode_audit_chunk(nstu::audit::encode_audit_chunk(bounded.spool.peek_chunk()));
    assert(decoded && decoded->size() == 1 && (*decoded)[0].event.detail == bounded.events[0].detail);
    assert(bounded.debug[0] == "NSTU exam telemetry startup failed: " + bounded.events[0].detail + "\n");
    bounded.reporter.report(Operation::startup, false, bounded.events[0].detail);
    assert(bounded.spool.size() == 1);
    std::cout << "Literal privacy/metadata/Spool receipts and bounded owned canonical keys passed.\n";
}

void test_audit_debug_failures_and_memory_acceptance() {
    for (const bool throw_audit : {false, true}) {
        for (const bool throw_debug : {false, true}) {
            nstu::audit::Spool spool;
            unsigned audits = 0, debug = 0;
            Reporter reporter([&](const nstu::audit::Event& event) {
                ++audits;
                if (throw_audit && audits == 1) throw std::runtime_error("fails before delegation");
                return spool.emit(event);
            }, [&](const std::string&) { ++debug; if (throw_debug) throw std::runtime_error("debug fails after insertion"); });
            reporter.report(Operation::startup, false, "restore failed");
            assert(audits == 1 && debug == (throw_audit ? 0u : 1u) && spool.size() == (throw_audit ? std::size_t{0} : std::size_t{1}));
            reporter.report(Operation::startup, false, "restore failed");
            assert(audits == (throw_audit ? 2u : 1u) && debug == 1 && spool.size() == 1);
            reporter.report(Operation::startup, false, "restore failed");
            assert(audits == (throw_audit ? 2u : 1u) && debug == 1);
            const auto records = spool.peek_chunk(); assert(records.size() == 1 && records[0].sequence == 1);
            assert(records[0].event.action == "startup" && records[0].event.detail == "restore failed");
            const auto ack = nstu::audit::decode_audit_ack(nstu::audit::encode_audit_ack(records[0].sequence));
            assert(ack && *ack == 1); spool.acknowledge(*ack);
            reporter.report(Operation::startup, false, "restore failed");
            assert(spool.empty() && debug == 1 && audits == (throw_audit ? 2u : 1u));
        }
    }
    nstu::audit::Spool tiny(1); unsigned attempts = 0;
    Reporter reporter([&](const nstu::audit::Event& event) { ++attempts; return tiny.emit(event); }, [](const std::string&) {});
    reporter.report(Operation::startup, false, "same failure");
    reporter.report(Operation::enable, false, "same failure");
    assert(attempts == 2 && tiny.size() == 1 && tiny.dropped_count() == 1 && tiny.peek_chunk()[0].sequence == 2);
    reporter.report(Operation::startup, false, "same failure");
    reporter.report(Operation::enable, false, "same failure");
    assert(attempts == 2 && tiny.peek_chunk()[0].event.action == "enable");
    nstu::audit::Event unrelated; unrelated.component = "unrelated";
    assert(tiny.emit(unrelated) == 3 && tiny.dropped_count() == 2);
    reporter.report(Operation::startup, false, "same failure");
    assert(attempts == 2 && tiny.peek_chunk()[0].sequence == 3);
    tiny.acknowledge(3); assert(tiny.empty());
    reporter.report(Operation::enable, false, "same failure");
    assert(attempts == 2 && tiny.empty());
    reporter.report(Operation::startup, true, {});
    reporter.report(Operation::startup, false, "same failure");
    assert(attempts == 3 && tiny.peek_chunk()[0].sequence == 4);
    nstu::audit::Spool shared;
    Reporter first([&](const nstu::audit::Event& event) { return shared.emit(event); }, [](const std::string&) {});
    Reporter second([&](const nstu::audit::Event& event) { return shared.emit(event); }, [](const std::string&) {});
    first.report(Operation::samples, false, "same failure"); second.report(Operation::samples, false, "same failure");
    assert(shared.size() == 2 && shared.peek_chunk()[1].sequence == 2);
    std::cout << "Audit-before-debug throws, post-accept debug suppression, real ACK/eviction and independent instances passed.\n";
}

void test_serialized_observations_and_controlled_epochs() {
    nstu::audit::Spool spool;
    std::atomic<unsigned> active{0}, audits{0}, debug{0};
    Reporter reporter([&](const nstu::audit::Event& event) {
        assert(active.fetch_add(1) == 0); ++audits;
        const auto assigned = spool.emit(event); assert(active.fetch_sub(1) == 1); return assigned;
    }, [&](const std::string&) { assert(active.fetch_add(1) == 0); ++debug; assert(active.fetch_sub(1) == 1); });
    constexpr std::size_t count = 8;
    std::barrier start(static_cast<std::ptrdiff_t>(count + 1));
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < count; ++i) threads.emplace_back([&] {
        start.arrive_and_wait(); reporter.report(Operation::maintenance, false, "disk failure");
    });
    start.arrive_and_wait(); for (auto& thread : threads) thread.join();
    assert(audits == 1 && debug == 1 && active == 0 && spool.size() == 1);
    std::mutex mutex; std::condition_variable cv;
    bool entered = false, second_ready = false, release = false;
    std::vector<std::string> details; nstu::audit::Spool ordered_spool;
    Reporter ordered([&](const nstu::audit::Event& event) {
        std::unique_lock lock(mutex); details.push_back(event.detail);
        if (details.size() == 1) {
            entered = true; cv.notify_all(); assert(cv.wait_for(lock, std::chrono::seconds(5), [&] { return release; }));
        }
        return ordered_spool.emit(event);
    }, [](const std::string&) {});
    std::thread a([&] { ordered.report(Operation::startup, false, "disk failure"); });
    { std::unique_lock lock(mutex); assert(cv.wait_for(lock, std::chrono::seconds(5), [&] { return entered; })); }
    std::thread b([&] {
        { std::lock_guard lock(mutex); second_ready = true; cv.notify_all(); }
        ordered.report(Operation::startup, false, "different failure");
    });
    {
        std::unique_lock lock(mutex); assert(cv.wait_for(lock, std::chrono::seconds(5), [&] { return second_ready; }));
        release = true; cv.notify_all();
    }
    a.join(); b.join(); ordered.report(Operation::startup, false, "disk failure");
    assert((details == std::vector<std::string>{"disk failure", "different failure", "disk failure"}));
    assert(ordered_spool.size() == 3 && ordered_spool.peek_chunk()[2].sequence == 3);
    std::cout << "Barrier-start identical observations and bounded serialized A-B-A passed.\n";
}
void test_shared_labeled_sas_guard() {
    const std::array<const char*, 12> labeled{{
        "sAs=123456 private clipboard", "SAS:123456", "SAS 123456",
        "SAS_CODE = 123456", "PAIRING-CODE : 123456", "[SAS=123456]",
        "\"SAS=123456\"", "[SAS] 123456", "sas-code:123456",
        "pairing_code=123456", "PairingCode 123456", "SAS : 123456"
    }};
    for (const auto input : labeled) {
        const auto public_text = nstu::telemetry::sanitize_public_text(input, nstu::audit::kMaximumDetailBytes);
        const auto audit_text = nstu::audit::sanitize_audit_field(input, nstu::audit::kMaximumDetailBytes);
        if (public_text != "<redacted>") std::cerr << "Labeled SAS public guard failed: [" << public_text << "]\n";
        assert(public_text == "<redacted>" && audit_text == "<redacted>");
        Capture capture;
        capture.reporter.report(Operation::shutdown, false, input);
        capture.notice(0, "shutdown", "<redacted>");
        const auto encoded = nstu::audit::encode_audit_chunk(capture.spool.peek_chunk());
        const std::string wire(reinterpret_cast<const char*>(encoded.data()), encoded.size());
        assert(wire.find("123456") == std::string::npos && capture.debug[0].find("123456") == std::string::npos);
        assert((capture.order == std::vector<char>{'a', 'd'}));
    }
    const std::array<const char*, 6> ordinary{{
        "sassy=123456", "sascodebook=123456", "pairingcodes=123456",
        "samples=123456", "123456", "sasquatch"
    }};
    for (const auto input : ordinary) {
        assert(nstu::telemetry::sanitize_public_text(input, nstu::audit::kMaximumDetailBytes) == input);
        assert(nstu::audit::sanitize_audit_field(input, nstu::audit::kMaximumDetailBytes) == input);
        Capture capture; capture.reporter.report(Operation::samples, false, input); capture.notice(0, "samples", input);
    }
    Capture equivalent;
    equivalent.reporter.report(Operation::enable, false, "SAS=123456");
    equivalent.notice(0, "enable", "<redacted>");
    equivalent.reporter.report(Operation::enable, false, "SAS=654321");
    equivalent.reporter.report(Operation::enable, false, "pairing_code=654321");
    assert(equivalent.spool.size() == 1 && equivalent.debug.size() == 1);
    std::cout << "Shared public/audit/private-callable labeled SAS syntax, ordinary controls and canonical secret-key equivalence passed.\n";
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "sas-canonical-guard") {
        test_shared_labeled_sas_guard();
        return 0;
    }
    test_constructor_operations_and_invalid_indices();
    test_failed_insertion_retries_before_acceptance();
    test_operation_epochs_and_empty_sanitized_keys();
    test_literal_privacy_and_owned_canonical_keys();
    test_audit_debug_failures_and_memory_acceptance();
    test_serialized_observations_and_controlled_epochs();
    test_shared_labeled_sas_guard();
    return 0;
}
