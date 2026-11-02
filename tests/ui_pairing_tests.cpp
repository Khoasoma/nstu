#include "nstu/client_pairing.hpp"
#include "nstu/control_plane.hpp"
#include "nstu/keyring.hpp"
#include "nstu/multicast.hpp"
#include "nstu/pairing.hpp"

#include <windows.h>

#include <array>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

using nstu::client::StudentConfirmation;
using nstu::client::StudentConfirmationChoice;
using nstu::client::StudentConfirmationDecision;
using nstu::client::evaluate_student_confirmation;
using nstu::client::student_confirmation_choice_text;
using nstu::client::student_confirmation_decision_text;
using nstu::client::PairingOutcome;
using nstu::pairing::format_short_authentication_string;

constexpr const char* kClientUuid = "7A8B9C0D-1E2F-3A4B-5C6D-7E8F9A0B1C2D";
constexpr const char* kClientHostname = "STUDENT-PC-01";
constexpr const char* kServerName = "Room 101";

void test_evaluate_student_confirmation_confirm() {
    // Confirmation without two actual codes cannot authorize enrollment.
    assert(evaluate_student_confirmation(true, StudentConfirmationChoice::confirmed) ==
           StudentConfirmationDecision::refuse);

    // Confirm with matching codes.
    assert(evaluate_student_confirmation(true, StudentConfirmationChoice::confirmed,
                                         "123 456", "123 456") ==
           StudentConfirmationDecision::save);
    assert(evaluate_student_confirmation(true, StudentConfirmationChoice::confirmed,
                                         "987654", "987654") ==
           StudentConfirmationDecision::save);
}

void test_evaluate_student_confirmation_reject() {
    // Explicit user rejection must always refuse.
    assert(evaluate_student_confirmation(true, StudentConfirmationChoice::rejected) ==
           StudentConfirmationDecision::refuse);
    assert(evaluate_student_confirmation(true, StudentConfirmationChoice::rejected,
                                         "123 456", "123 456") ==
           StudentConfirmationDecision::refuse);
}

void test_evaluate_student_confirmation_timeout() {
    // Timeout must always refuse.
    assert(evaluate_student_confirmation(true, StudentConfirmationChoice::timed_out) ==
           StudentConfirmationDecision::refuse);
    assert(evaluate_student_confirmation(true, StudentConfirmationChoice::timed_out,
                                         "123 456", "123 456") ==
           StudentConfirmationDecision::refuse);
}

void test_evaluate_student_confirmation_no_ui() {
    // If code cannot be displayed or session has no UI, refuse immediately (fail closed).
    assert(evaluate_student_confirmation(false, StudentConfirmationChoice::confirmed) ==
           StudentConfirmationDecision::refuse);
    assert(evaluate_student_confirmation(false, StudentConfirmationChoice::rejected) ==
           StudentConfirmationDecision::refuse);
    assert(evaluate_student_confirmation(false, StudentConfirmationChoice::timed_out) ==
           StudentConfirmationDecision::refuse);
    assert(evaluate_student_confirmation(false, StudentConfirmationChoice::no_ui) ==
           StudentConfirmationDecision::refuse);
    assert(evaluate_student_confirmation(true, StudentConfirmationChoice::no_ui) ==
           StudentConfirmationDecision::refuse);
}

void test_evaluate_student_confirmation_mismatched_code() {
    // Mismatched code choice from student.
    assert(evaluate_student_confirmation(true, StudentConfirmationChoice::mismatched_code) ==
           StudentConfirmationDecision::refuse);
    assert(evaluate_student_confirmation(true, StudentConfirmationChoice::mismatched_code,
                                         "123 456", "123 456") ==
           StudentConfirmationDecision::refuse);

    // Student claimed confirm but provided different digits.
    assert(evaluate_student_confirmation(true, StudentConfirmationChoice::confirmed,
                                         "123 456", "654 321") ==
           StudentConfirmationDecision::refuse);
    assert(evaluate_student_confirmation(true, StudentConfirmationChoice::confirmed,
                                         "123 456", "123456") ==
           StudentConfirmationDecision::refuse);
    assert(evaluate_student_confirmation(true, StudentConfirmationChoice::confirmed,
                                         "123 456", "") ==
           StudentConfirmationDecision::refuse);
    assert(evaluate_student_confirmation(true, StudentConfirmationChoice::confirmed,
                                         "", "123 456") ==
           StudentConfirmationDecision::refuse);
}

void test_sas_formatting() {
    assert(format_short_authentication_string("123456") == "123 456");
    assert(format_short_authentication_string("000999") == "000 999");
    assert(format_short_authentication_string("12345") == "12345");
    assert(format_short_authentication_string("") == "");
}

void test_choice_and_decision_text() {
    assert(std::string(student_confirmation_choice_text(
               StudentConfirmationChoice::confirmed)) == "confirmed");
    assert(std::string(student_confirmation_choice_text(
               StudentConfirmationChoice::rejected)) == "rejected");
    assert(std::string(student_confirmation_choice_text(
               StudentConfirmationChoice::mismatched_code)) == "mismatched code");
    assert(std::string(student_confirmation_choice_text(
               StudentConfirmationChoice::timed_out)) == "timed out");
    assert(std::string(student_confirmation_choice_text(
               StudentConfirmationChoice::no_ui)) == "no UI session");
    assert(std::string(student_confirmation_choice_text(
               StudentConfirmationChoice::waiting)) == "waiting");
    assert(std::string(nstu::client::pairing_outcome_text(
               PairingOutcome::student_refused)).find("student") != std::string::npos);

    assert(std::string(student_confirmation_decision_text(
               StudentConfirmationDecision::save)) == "save");
    assert(std::string(student_confirmation_decision_text(
               StudentConfirmationDecision::refuse)) == "refuse");
}

void test_fail_closed_without_code_observer() {
    nstu::discovery::PairingCandidate candidate{
        .address = "127.0.0.1",
        .port = 47001,
        .server_name = kServerName,
    };
    std::string error;
    // Without code observer, fail closed immediately.
    auto result = nstu::client::pair_with_server(
        candidate, kClientUuid, kClientHostname,
        nullptr, std::stop_token{}, {}, &error);
    assert(result.outcome == PairingOutcome::failed);
    assert(error.find("no UI session") != std::string::npos);
}

struct TestPairingRun {
    nstu::client::PairingAttemptResult result;
    std::string code;
    std::string server_name;
    std::string error;
};

std::vector<nstu::server::PendingPairing> wait_for_pending(
    const nstu::server::ServerControlPlane& server, std::size_t expected,
    int attempts = 100) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
        auto pending = server.pending_pairings();
        if (pending.size() == expected) {
            return pending;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return server.pending_pairings();
}

void test_pair_with_server_student_confirmation() {
    nstu::net::WinsockRuntime winsock;
    assert(winsock.ready());

    const std::array<std::byte, 4> entropy{
        std::byte{9}, std::byte{8}, std::byte{7}, std::byte{6}};
    wchar_t temp_dir[MAX_PATH]{};
    assert(GetTempPathW(MAX_PATH, temp_dir) != 0);
    const auto keyring_path = std::filesystem::path(temp_dir) /
        (L"nstu-test-pairing-ui-" + std::to_wstring(GetCurrentProcessId()) + L".bin");

    nstu::security::KeyStore key_store;
    nstu::server::ClientRegistry registry;
    nstu::server::ServerControlPlane server(registry, key_store);
    nstu::server::ServerControlPlaneConfig config;
    config.port = 0;
    config.keyring_path = keyring_path.wstring();
    config.keyring_entropy.assign(entropy.begin(), entropy.end());
    std::string error;
    assert(server.start(std::move(config), &error));
    server.set_pairing_window(true, kServerName);

    const nstu::discovery::PairingCandidate candidate{
        .address = "127.0.0.1",
        .port = server.local_port(),
        .server_name = kServerName,
    };

    // No local confirmation hook must not enroll, even when the code is observed.
    {
        nstu::client::PairingAttemptOptions opts;
        opts.approval_timeout = std::chrono::seconds(5);
        TestPairingRun run;
        std::jthread worker([&](std::stop_token token) {
            run.result = nstu::client::pair_with_server(
                candidate, kClientUuid, kClientHostname,
                [&run](const std::string& code, const std::string& sname) {
                    run.code = code;
                    run.server_name = sname;
                }, token, opts, &run.error);
        });
        const auto pending = wait_for_pending(server, 1);
        if (!pending.empty()) {
            // Let the round-1 auto-approve path finish so the regression fails cleanly.
            assert(server.approve_pairing(pending[0].pairing_id, &error));
        }
        worker.join();
        assert(!run.code.empty());
        assert(run.result.outcome == PairingOutcome::failed);
        assert(run.result.config.key_id == 0);
        assert(run.result.config.pre_shared_key.empty());
        assert(pending.empty());
        assert(key_store.key_count() == 0);
        assert(run.error == "student confirmation unavailable");
    }

    // Case 1: Student rejects.
    {
        nstu::client::PairingAttemptOptions opts;
        opts.approval_timeout = std::chrono::seconds(5);
        opts.connect_timeout_ms = 2000;
        opts.heartbeat_interval_ms = 200;
        opts.student_confirmation = [](const std::string&, const std::string&,
                                       auto, std::stop_token) {
            return StudentConfirmation{StudentConfirmationChoice::rejected, {}};
        };

        TestPairingRun run;
        run.result = nstu::client::pair_with_server(
            candidate, kClientUuid, kClientHostname,
            [&run](const std::string& code, const std::string& sname) {
                run.code = code;
                run.server_name = sname;
            },
            std::stop_token{}, opts, &run.error);

        assert(run.result.outcome == PairingOutcome::student_refused);
        assert(run.result.config.key_id == 0);
        assert(run.result.config.pre_shared_key.empty());
        assert(key_store.key_count() == 0);
        assert(!run.code.empty());
        assert(run.error.find("student") != std::string::npos);
    }

    // Case 2: Student times out.
    {
        nstu::client::PairingAttemptOptions opts;
        opts.approval_timeout = std::chrono::seconds(5);
        opts.connect_timeout_ms = 2000;
        opts.heartbeat_interval_ms = 200;
        opts.student_confirmation = [](const std::string&, const std::string&,
                                       auto, std::stop_token) {
            return StudentConfirmation{StudentConfirmationChoice::timed_out, {}};
        };

        TestPairingRun run;
        run.result = nstu::client::pair_with_server(
            candidate, kClientUuid, kClientHostname,
            [&run](const std::string& code, const std::string& sname) {
                run.code = code;
                run.server_name = sname;
            },
            std::stop_token{}, opts, &run.error);

        assert(run.result.outcome == PairingOutcome::timed_out);
        assert(run.result.config.key_id == 0);
        assert(run.result.config.pre_shared_key.empty());
        assert(key_store.key_count() == 0);
        assert(!run.code.empty());
        assert(run.error == nstu::client::pairing_outcome_text(PairingOutcome::timed_out));
    }

    // A callback may not claim success with another code or no code.
    for (const std::string& confirmed_code : {std::string("000 000"), std::string{}}) {
        nstu::client::PairingAttemptOptions opts;
        opts.student_confirmation = [confirmed_code](const std::string&,
                const std::string&, auto, std::stop_token) {
            return StudentConfirmation{StudentConfirmationChoice::confirmed,
                                       confirmed_code};
        };
        TestPairingRun run;
        run.result = nstu::client::pair_with_server(
            candidate, kClientUuid, kClientHostname,
            [&run](const std::string& code, const std::string&) {
                run.code = code;
            }, std::stop_token{}, opts, &run.error);
        assert(!run.code.empty());
        assert(run.result.outcome == PairingOutcome::student_refused);
        assert(run.result.config.key_id == 0);
        assert(run.result.config.pre_shared_key.empty());
        assert(key_store.key_count() == 0);
    }

    // A waiting confirmation has a bounded deadline and cannot return a key.
    {
        nstu::client::PairingAttemptOptions opts;
        opts.approval_timeout = std::chrono::seconds(1);
        int slices = 0;
        opts.student_confirmation = [&slices](const std::string&,
                const std::string&, std::chrono::steady_clock::time_point deadline,
                std::stop_token) {
            ++slices;
            std::this_thread::sleep_until(deadline);
            return StudentConfirmation{StudentConfirmationChoice::waiting, {}};
        };
        TestPairingRun run;
        run.result = nstu::client::pair_with_server(
            candidate, kClientUuid, kClientHostname,
            [&run](const std::string& code, const std::string&) { run.code = code; },
            std::stop_token{}, opts, &run.error);
        assert(slices >= 2);
        assert(run.result.outcome == PairingOutcome::timed_out);
        assert(run.result.config.key_id == 0);
        assert(run.result.config.pre_shared_key.empty());
        assert(key_store.key_count() == 0);
    }

    // The offered stage must accept keepalives before local approval.
    {
        nstu::client::PairingAttemptOptions opts;
        opts.approval_timeout = std::chrono::seconds(17);
        opts.heartbeat_interval_ms = 200;
        const auto began = std::chrono::steady_clock::now();
        opts.student_confirmation = [began](const std::string& code,
                const std::string&, std::chrono::steady_clock::time_point slice,
                std::stop_token) {
            if (std::chrono::steady_clock::now() - began >=
                std::chrono::seconds(16)) {
                return StudentConfirmation{StudentConfirmationChoice::confirmed, code};
            }
            std::this_thread::sleep_until(slice);
            return StudentConfirmation{StudentConfirmationChoice::waiting, {}};
        };
        TestPairingRun run;
        std::jthread worker([&](std::stop_token token) {
            run.result = nstu::client::pair_with_server(
                candidate, kClientUuid, kClientHostname,
                [&run](const std::string& code, const std::string&) { run.code = code; },
                token, opts, &run.error);
        });
        const auto pending = wait_for_pending(server, 1, 900);
        assert(pending.size() == 1);
        assert(server.approve_pairing(pending[0].pairing_id, &error));
        worker.join();
        assert(run.result.outcome == PairingOutcome::enrolled);
        assert(run.result.config.key_id != 0);
        assert(key_store.key_count() == 1);
    }

    // Cancellation during confirmation overrides even a confirmed response.
    {
        std::stop_source stop;
        nstu::client::PairingAttemptOptions opts;
        opts.student_confirmation = [&stop](const std::string& code,
                const std::string&, auto, std::stop_token token) {
            assert(!token.stop_requested());
            stop.request_stop();
            return StudentConfirmation{StudentConfirmationChoice::confirmed, code};
        };
        TestPairingRun run;
        run.result = nstu::client::pair_with_server(
            candidate, kClientUuid, kClientHostname,
            [&run](const std::string& code, const std::string&) { run.code = code; },
            stop.get_token(), opts, &run.error);
        assert(!run.code.empty());
        assert(run.result.outcome == PairingOutcome::failed);
        assert(run.result.config.key_id == 0);
        assert(run.result.config.pre_shared_key.empty());
        assert(run.error == "pairing was cancelled");
        assert(key_store.key_count() == 1);
    }

    // Case 3: Student confirms codes match -> server receives request and approves.
    {
        nstu::client::PairingAttemptOptions opts;
        opts.approval_timeout = std::chrono::seconds(10);
        opts.connect_timeout_ms = 2000;
        opts.heartbeat_interval_ms = 200;
        opts.student_confirmation = [](const std::string& code, const std::string&,
                                       auto, std::stop_token) {
            return StudentConfirmation{StudentConfirmationChoice::confirmed, code};
        };

        TestPairingRun run;
        std::jthread worker([&](std::stop_token token) {
            run.result = nstu::client::pair_with_server(
                candidate, kClientUuid, kClientHostname,
                [&run](const std::string& code, const std::string& sname) {
                    run.code = code;
                    run.server_name = sname;
                },
                std::move(token), opts, &run.error);
        });

        const auto pending = wait_for_pending(server, 1);
        assert(pending.size() == 1);
        assert(server.approve_pairing(pending[0].pairing_id, &error));
        worker.join();

        assert(run.result.outcome == PairingOutcome::enrolled);
        assert(run.result.config.key_id != 0);
    }

    server.set_pairing_window(false);
    server.stop();
    (void)DeleteFileW(keyring_path.c_str());
}

} // namespace

int main() {
    test_evaluate_student_confirmation_confirm();
    test_evaluate_student_confirmation_reject();
    test_evaluate_student_confirmation_timeout();
    test_evaluate_student_confirmation_no_ui();
    test_evaluate_student_confirmation_mismatched_code();
    test_sas_formatting();
    test_choice_and_decision_text();
    test_fail_closed_without_code_observer();
    test_pair_with_server_student_confirmation();
    return 0;
}
