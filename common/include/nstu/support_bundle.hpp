#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace nstu::support {

inline constexpr std::size_t kMaximumDiagnosticEvents = 128;
inline constexpr std::size_t kMaximumEventTextBytes = 512;
inline constexpr std::size_t kMaximumSummaryFieldBytes = 128;

enum class Severity : std::uint8_t {
    information = 0,
    warning = 1,
    error = 2,
};

enum class BundleRole : std::uint8_t {
    teacher = 0,
    technician = 1,
    developer = 2,
};

[[nodiscard]] constexpr std::string_view severity_to_string(Severity severity) noexcept {
    switch (severity) {
    case Severity::information:
        return "information";
    case Severity::warning:
        return "warning";
    case Severity::error:
        return "error";
    }
    return "unknown";
}

struct SystemDiagnosticSummary {
    std::string application = "NSTU Server";
    std::string version = "0.1.0";
    std::string build_channel = "Release";
    std::string os_version;
    std::string operator_role = "Technician";
    std::string graphics_mode = "Direct3D 11";
    std::string network_adapter;
    std::uint32_t total_clients_known = 0;
    std::uint32_t active_clients_count = 0;
    std::uint32_t stale_clients_count = 0;
    std::uint32_t agent_connected_count = 0;
    std::uint32_t uwf_protected_count = 0;
};

struct DiagnosticEvent {
    std::string timestamp; // "HH:MM:SS"
    Severity severity = Severity::information;
    std::string component;
    std::string detail;
};

struct SupportBundle {
    SystemDiagnosticSummary summary;
    std::deque<DiagnosticEvent> events;
    bool canaries_verified = false;
};

enum class SupportBundleConsentDecision : std::uint8_t {
    permitted = 0,
    denied_no_consent = 1,
    denied_insufficient_role = 2,
};

[[nodiscard]] constexpr SupportBundleConsentDecision evaluate_support_bundle_export(
    BundleRole role, bool user_consented) noexcept {
    if (!user_consented) {
        return SupportBundleConsentDecision::denied_no_consent;
    }
    if (role == BundleRole::technician ||
        role == BundleRole::developer) {
        return SupportBundleConsentDecision::permitted;
    }
    return SupportBundleConsentDecision::denied_insufficient_role;
}

[[nodiscard]] constexpr SupportBundleConsentDecision evaluate_support_bundle_export(
    int role_id, bool user_consented) noexcept {
    if (role_id == 1 || role_id == 2) {
        return evaluate_support_bundle_export(
            role_id == 1 ? BundleRole::technician : BundleRole::developer,
            user_consented);
    }
    return evaluate_support_bundle_export(BundleRole::teacher, user_consented);
}

[[nodiscard]] std::string sanitize_diagnostic_text(
    std::string_view text,
    std::size_t maximum_bytes = kMaximumEventTextBytes);

[[nodiscard]] bool verify_bundle_canaries(std::string_view text) noexcept;

[[nodiscard]] SupportBundle create_sanitized_bundle(
    SystemDiagnosticSummary summary,
    const std::deque<DiagnosticEvent>& raw_events);

[[nodiscard]] std::string build_support_bundle_markdown(
    const SupportBundle& bundle);

} // namespace nstu::support
