#include "nstu/support_bundle.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <string_view>

namespace {

void test_redact_user_paths() {
    const std::string win_back = "Failed opening C:\\Users\\JohnDoe\\AppData\\Local\\Temp\\err.log";
    const std::string sanitized_back = nstu::support::sanitize_diagnostic_text(win_back);
    assert(sanitized_back.find("JohnDoe") == std::string::npos);
    assert(sanitized_back.find("<USERPROFILE>") != std::string::npos);

    const std::string win_fwd = "Loaded from C:/Users/JaneDoe/Documents/config.json";
    const std::string sanitized_fwd = nstu::support::sanitize_diagnostic_text(win_fwd);
    assert(sanitized_fwd.find("JaneDoe") == std::string::npos);
    assert(sanitized_fwd.find("<USERPROFILE>") != std::string::npos);

    const std::string unix_path = "Log saved to /home/student1/session.log";
    const std::string sanitized_unix = nstu::support::sanitize_diagnostic_text(unix_path);
    assert(sanitized_unix.find("student1") == std::string::npos);
    assert(sanitized_unix.find("<USERPROFILE>") != std::string::npos);
}

void test_redact_hex_keys() {
    const std::string hex_32 = "Pre-shared key 0123456789abcdef0123456789abcdef initialized.";
    const std::string sanitized_32 = nstu::support::sanitize_diagnostic_text(hex_32);
    assert(sanitized_32.find("0123456789abcdef0123456789abcdef") == std::string::npos);
    assert(sanitized_32.find("<REDACTED_KEY>") != std::string::npos);

    const std::string hex_64 = "Hash: e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855 verified.";
    const std::string sanitized_64 = nstu::support::sanitize_diagnostic_text(hex_64);
    assert(sanitized_64.find("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == std::string::npos);
    assert(sanitized_64.find("<REDACTED_KEY>") != std::string::npos);
}

void test_redact_credentials_and_tokens() {
    const std::string pwd = "Login attempt with password=SuperSecretPass123! from host.";
    const std::string sanitized_pwd = nstu::support::sanitize_diagnostic_text(pwd);
    assert(sanitized_pwd.find("SuperSecretPass123!") == std::string::npos);
    assert(sanitized_pwd.find("<REDACTED_SECRET>") != std::string::npos);

    const std::string bearer = "Auth header: bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9";
    const std::string sanitized_bearer = nstu::support::sanitize_diagnostic_text(bearer);
    assert(sanitized_bearer.find("eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9") == std::string::npos);
    assert(sanitized_bearer.find("<REDACTED_SECRET>") != std::string::npos);

    const std::string sas_code = "Pairing code: 948 215 presented to student.";
    const std::string sanitized_sas = nstu::support::sanitize_diagnostic_text(sas_code);
    assert(sanitized_sas.find("948 215") == std::string::npos);
    assert(sanitized_sas.find("<REDACTED_SAS>") != std::string::npos);
}

void test_redact_assessment_data() {
    const std::string answer = "Submit event: student_answer=Option_C_Final question_text=SampleQuestion";
    const std::string sanitized_answer = nstu::support::sanitize_diagnostic_text(answer);
    assert(sanitized_answer.find("Option_C_Final") == std::string::npos);
    assert(sanitized_answer.find("SampleQuestion") == std::string::npos);
    assert(sanitized_answer.find("<REDACTED_EXAM_DATA>") != std::string::npos);

    const std::string ticket = "Session started with exam_ticket=ticket_secret_12345.";
    const std::string sanitized_ticket = nstu::support::sanitize_diagnostic_text(ticket);
    assert(sanitized_ticket.find("ticket_secret_12345") == std::string::npos);
    assert(sanitized_ticket.find("<REDACTED_EXAM_DATA>") != std::string::npos);
}

void test_verify_bundle_canaries() {
    const std::string clean = "Normal operating message: Direct3D 11 initialized on Adapter 0.";
    assert(nstu::support::verify_bundle_canaries(clean));

    const std::string dirty = "Secret leaked: password=SuperSecretPass123!";
    assert(!nstu::support::verify_bundle_canaries(dirty));

    // Sanitization must successfully neutralise the canary
    const std::string sanitized = nstu::support::sanitize_diagnostic_text(dirty);
    assert(nstu::support::verify_bundle_canaries(sanitized));
}

void test_consent_and_role_decision_matrix() {
    using nstu::support::BundleRole;
    using nstu::support::SupportBundleConsentDecision;

    // 1. Without consent, all roles denied
    assert(nstu::support::evaluate_support_bundle_export(
               BundleRole::teacher, false) ==
           SupportBundleConsentDecision::denied_no_consent);
    assert(nstu::support::evaluate_support_bundle_export(
               BundleRole::technician, false) ==
           SupportBundleConsentDecision::denied_no_consent);
    assert(nstu::support::evaluate_support_bundle_export(
               BundleRole::developer, false) ==
           SupportBundleConsentDecision::denied_no_consent);

    // 2. With consent, teacher denied insufficient role
    assert(nstu::support::evaluate_support_bundle_export(
               BundleRole::teacher, true) ==
           SupportBundleConsentDecision::denied_insufficient_role);

    // 3. With consent, technician and developer permitted
    assert(nstu::support::evaluate_support_bundle_export(
               BundleRole::technician, true) ==
           SupportBundleConsentDecision::permitted);
    assert(nstu::support::evaluate_support_bundle_export(
               BundleRole::developer, true) ==
           SupportBundleConsentDecision::permitted);

    // 4. Also test integer overload
    assert(nstu::support::evaluate_support_bundle_export(0, true) ==
           SupportBundleConsentDecision::denied_insufficient_role);
    assert(nstu::support::evaluate_support_bundle_export(1, true) ==
           SupportBundleConsentDecision::permitted);
    assert(nstu::support::evaluate_support_bundle_export(2, true) ==
           SupportBundleConsentDecision::permitted);
}

void test_create_sanitized_bundle_and_markdown() {
    nstu::support::SystemDiagnosticSummary summary{
        .application = "NSTU Server",
        .version = "0.1.0",
        .build_channel = "Release",
        .os_version = "Windows 11 Pro 23H2",
        .operator_role = "Technician",
        .graphics_mode = "Direct3D 11 Hardware",
        .network_adapter = "Ethernet (192.168.1.50/24)",
        .total_clients_known = 30,
        .active_clients_count = 28,
        .stale_clients_count = 2,
        .agent_connected_count = 28,
        .uwf_protected_count = 28,
    };

    std::deque<nstu::support::DiagnosticEvent> raw_events;
    raw_events.push_back({
        .timestamp = "10:15:30",
        .severity = nstu::support::Severity::information,
        .component = "Discovery",
        .detail = "Beacon started on UDP port 47001",
    });
    raw_events.push_back({
        .timestamp = "10:15:35",
        .severity = nstu::support::Severity::warning,
        .component = "Auth",
        .detail = "Pairing attempt with sas=948 215 from C:\\Users\\JohnDoe\\AppData\\Local\\Temp",
    });

    const auto bundle = nstu::support::create_sanitized_bundle(summary, raw_events);
    assert(bundle.canaries_verified);
    assert(bundle.events.size() == 2);
    assert(bundle.events[1].detail.find("JohnDoe") == std::string::npos);
    assert(bundle.events[1].detail.find("948 215") == std::string::npos);

    const std::string md = nstu::support::build_support_bundle_markdown(bundle);
    assert(md.find("# NSTU Support & Diagnostic Bundle") != std::string::npos);
    assert(md.find("## System Diagnostics") != std::string::npos);
    assert(md.find("## Fleet Health Aggregates") != std::string::npos);
    assert(md.find("Total Known Clients | 30") != std::string::npos);
    assert(md.find("## Diagnostic Event Log") != std::string::npos);
    assert(md.find("Canary Verification: PASSED") != std::string::npos);
    assert(nstu::support::verify_bundle_canaries(md));
}

} // namespace

int main() {
    std::cout << "[test_redact_user_paths]\n";
    test_redact_user_paths();

    std::cout << "[test_redact_hex_keys]\n";
    test_redact_hex_keys();

    std::cout << "[test_redact_credentials_and_tokens]\n";
    test_redact_credentials_and_tokens();

    std::cout << "[test_redact_assessment_data]\n";
    test_redact_assessment_data();

    std::cout << "[test_verify_bundle_canaries]\n";
    test_verify_bundle_canaries();

    std::cout << "[test_consent_and_role_decision_matrix]\n";
    test_consent_and_role_decision_matrix();

    std::cout << "[test_create_sanitized_bundle_and_markdown]\n";
    test_create_sanitized_bundle_and_markdown();

    std::cout << "All support bundle tests passed successfully!\n";
    return 0;
}
