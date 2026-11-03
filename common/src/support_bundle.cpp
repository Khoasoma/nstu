#include "nstu/support_bundle.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace nstu::support {

namespace {

bool is_hex_char(char c) noexcept {
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

std::string to_lower_string(std::string_view str) {
    std::string result;
    result.reserve(str.size());
    for (char c : str) {
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return result;
}

// Redact Windows and UNIX user profile paths: C:\Users\<name>\... or /home/<name>/...
std::string redact_user_paths(std::string text) {
    // 1. Windows: "Users\\" or "Users/"
    static constexpr std::string_view kWinPrefixes[] = {"users\\", "users/"};
    for (const auto prefix : kWinPrefixes) {
        std::size_t pos = 0;
        while (pos < text.size()) {
            const std::string lower = to_lower_string(text);
            const std::size_t found = lower.find(prefix, pos);
            if (found == std::string::npos) {
                break;
            }
            // Find start of drive or path (e.g. "C:\" before "Users\")
            std::size_t path_start = found;
            if (path_start >= 3 && (text[path_start - 1] == '\\' || text[path_start - 1] == '/')) {
                if (text[path_start - 2] == ':') {
                    path_start -= 3; // "C:\"
                }
            }
            const std::size_t user_start = found + prefix.size();
            std::size_t user_end = user_start;
            while (user_end < text.size() && text[user_end] != '\\' && text[user_end] != '/' &&
                   text[user_end] != ' ' && text[user_end] != '"' && text[user_end] != '\'') {
                ++user_end;
            }
            if (user_end > user_start) {
                text.replace(path_start, user_end - path_start, "<USERPROFILE>");
                pos = path_start + 13; // length of "<USERPROFILE>"
            } else {
                pos = user_start + 1;
            }
        }
    }

    // 2. UNIX: "/home/"
    static constexpr std::string_view kUnixPrefix = "/home/";
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::string lower = to_lower_string(text);
        const std::size_t found = lower.find(kUnixPrefix, pos);
        if (found == std::string::npos) {
            break;
        }
        const std::size_t user_start = found + kUnixPrefix.size();
        std::size_t user_end = user_start;
        while (user_end < text.size() && text[user_end] != '/' &&
               text[user_end] != ' ' && text[user_end] != '"' && text[user_end] != '\'') {
            ++user_end;
        }
        if (user_end > user_start) {
            text.replace(found, user_end - found, "<USERPROFILE>");
            pos = found + 13;
        } else {
            pos = user_start + 1;
        }
    }
    return text;
}

// Redact hexadecimal keys (hashes, tokens, PSKs >= 32 chars)
std::string redact_hex_keys(std::string text) {
    std::size_t pos = 0;
    while (pos < text.size()) {
        if (is_hex_char(text[pos])) {
            const std::size_t start = pos;
            while (pos < text.size() && is_hex_char(text[pos])) {
                ++pos;
            }
            const std::size_t length = pos - start;
            if (length >= 32) {
                text.replace(start, length, "<REDACTED_KEY>");
                pos = start + 14; // length of "<REDACTED_KEY>"
            }
        } else {
            ++pos;
        }
    }
    return text;
}

// Redact tagged key-value secrets
std::string redact_tagged_secrets(std::string text) {
    static constexpr std::string_view kSecretPrefixes[] = {
        "password=", "password:", "pwd=",
        "bearer ",
        "token=", "token:",
        "secret=", "secret:",
        "psk=", "psk:",
        "sas=", "sas:",
        "student_answer=", "student_name=",
        "exam_ticket=", "ticket=", "question_text="
    };

    for (const auto prefix : kSecretPrefixes) {
        std::size_t pos = 0;
        while (pos < text.size()) {
            const std::string lower = to_lower_string(text);
            const std::size_t found = lower.find(prefix, pos);
            if (found == std::string::npos) {
                break;
            }
            const bool left_bound = (found == 0 ||
                                     (!std::isalnum(static_cast<unsigned char>(text[found - 1])) &&
                                      text[found - 1] != '_'));
            if (!left_bound) {
                pos = found + prefix.size();
                continue;
            }
            const std::size_t val_start = found + prefix.size();
            std::size_t val_end = val_start;
            while (val_end < text.size() &&
                   text[val_end] != ' ' && text[val_end] != '\n' &&
                   text[val_end] != '\r' && text[val_end] != '\t' &&
                   text[val_end] != ';' && text[val_end] != ',' &&
                   text[val_end] != '&' && text[val_end] != '"') {
                ++val_end;
            }
            if (val_end > val_start) {
                std::string tag = "<REDACTED_SECRET>";
                if (prefix.find("student") != std::string_view::npos ||
                    prefix.find("exam") != std::string_view::npos ||
                    prefix.find("ticket") != std::string_view::npos ||
                    prefix.find("question") != std::string_view::npos) {
                    tag = "<REDACTED_EXAM_DATA>";
                } else if (prefix.find("sas") != std::string_view::npos) {
                    tag = "<REDACTED_SAS>";
                }
                text.replace(val_start, val_end - val_start, tag);
                pos = val_start + tag.size();
            } else {
                pos = val_start + 1;
            }
        }
    }

    // Redact 6-digit SAS codes formatted as "XXX YYY"
    for (std::size_t i = 0; i + 7 <= text.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(text[i])) &&
            std::isdigit(static_cast<unsigned char>(text[i + 1])) &&
            std::isdigit(static_cast<unsigned char>(text[i + 2])) &&
            text[i + 3] == ' ' &&
            std::isdigit(static_cast<unsigned char>(text[i + 4])) &&
            std::isdigit(static_cast<unsigned char>(text[i + 5])) &&
            std::isdigit(static_cast<unsigned char>(text[i + 6]))) {
            // Ensure word boundary
            const bool left_bound = (i == 0 || !std::isalnum(static_cast<unsigned char>(text[i - 1])));
            const bool right_bound = (i + 7 == text.size() || !std::isalnum(static_cast<unsigned char>(text[i + 7])));
            if (left_bound && right_bound) {
                text.replace(i, 7, "<REDACTED_SAS>");
                i += 13;
            }
        }
    }

    return text;
}

} // namespace

std::string sanitize_diagnostic_text(
    std::string_view text,
    std::size_t maximum_bytes) {
    if (text.empty()) {
        return {};
    }

    // Clean control characters
    std::string cleaned;
    cleaned.reserve(text.size());
    for (char c : text) {
        const auto uc = static_cast<unsigned char>(c);
        if (uc < 0x20 && c != '\n' && c != '\r' && c != '\t') {
            cleaned.push_back(' ');
        } else {
            cleaned.push_back(c);
        }
    }

    // Run sanitization passes
    cleaned = redact_user_paths(std::move(cleaned));
    cleaned = redact_hex_keys(std::move(cleaned));
    cleaned = redact_tagged_secrets(std::move(cleaned));

    // Clamp length
    if (cleaned.size() > maximum_bytes) {
        cleaned.resize(maximum_bytes > 3 ? maximum_bytes - 3 : maximum_bytes);
        cleaned += "...";
    }

    return cleaned;
}

bool verify_bundle_canaries(std::string_view text) noexcept {
    // Canary strings that must NEVER appear in sanitized diagnostic output
    static constexpr std::string_view kCanarySignatures[] = {
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
        "948 215",
        "SuperSecretPass123!",
        "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9",
        "Option_C_Final",
        "JohnDoe",
    };

    const std::string lower = to_lower_string(text);
    for (const auto canary : kCanarySignatures) {
        const std::string lower_canary = to_lower_string(canary);
        if (lower.find(lower_canary) != std::string::npos) {
            return false; // FAIL: Canary leaked into output!
        }
    }
    return true;
}

SupportBundle create_sanitized_bundle(
    SystemDiagnosticSummary summary,
    const std::deque<DiagnosticEvent>& raw_events) {
    SupportBundle bundle;
    bundle.summary.application = sanitize_diagnostic_text(summary.application, kMaximumSummaryFieldBytes);
    bundle.summary.version = sanitize_diagnostic_text(summary.version, kMaximumSummaryFieldBytes);
    bundle.summary.build_channel = sanitize_diagnostic_text(summary.build_channel, kMaximumSummaryFieldBytes);
    bundle.summary.os_version = sanitize_diagnostic_text(summary.os_version, kMaximumSummaryFieldBytes);
    bundle.summary.operator_role = sanitize_diagnostic_text(summary.operator_role, kMaximumSummaryFieldBytes);
    bundle.summary.graphics_mode = sanitize_diagnostic_text(summary.graphics_mode, kMaximumSummaryFieldBytes);
    bundle.summary.network_adapter = sanitize_diagnostic_text(summary.network_adapter, kMaximumSummaryFieldBytes);
    bundle.summary.total_clients_known = summary.total_clients_known;
    bundle.summary.active_clients_count = summary.active_clients_count;
    bundle.summary.stale_clients_count = summary.stale_clients_count;
    bundle.summary.agent_connected_count = summary.agent_connected_count;
    bundle.summary.uwf_protected_count = summary.uwf_protected_count;

    const std::size_t start_idx = raw_events.size() > kMaximumDiagnosticEvents
                                      ? raw_events.size() - kMaximumDiagnosticEvents
                                      : 0;

    for (std::size_t i = start_idx; i < raw_events.size(); ++i) {
        const auto& ev = raw_events[i];
        bundle.events.push_back(DiagnosticEvent{
            .timestamp = sanitize_diagnostic_text(ev.timestamp, 32),
            .severity = ev.severity,
            .component = sanitize_diagnostic_text(ev.component, 64),
            .detail = sanitize_diagnostic_text(ev.detail, kMaximumEventTextBytes),
        });
    }

    // Verify against canary suite
    const std::string preview = build_support_bundle_markdown(bundle);
    bundle.canaries_verified = verify_bundle_canaries(preview);

    return bundle;
}

std::string build_support_bundle_markdown(const SupportBundle& bundle) {
    std::ostringstream out;
    out << "# NSTU Support & Diagnostic Bundle\n\n"
        << "> [!NOTE]\n"
        << "> This bundle contains sanitized system diagnostics for support and troubleshooting.\n"
        << "> Passwords, cryptographic keys, enrollment secrets, student identities, and exam answers are strictly excluded.\n\n"
        << "## System Diagnostics\n"
        << "| Attribute | Value |\n"
        << "|---|---|\n"
        << "| Application | " << bundle.summary.application << " |\n"
        << "| Version | " << bundle.summary.version << " |\n"
        << "| Build Channel | " << bundle.summary.build_channel << " |\n"
        << "| OS Version | " << (bundle.summary.os_version.empty() ? "N/A" : bundle.summary.os_version) << " |\n"
        << "| Operator Role | " << bundle.summary.operator_role << " |\n"
        << "| Graphics Mode | " << bundle.summary.graphics_mode << " |\n"
        << "| Network Adapter | " << (bundle.summary.network_adapter.empty() ? "N/A" : bundle.summary.network_adapter) << " |\n\n"
        << "## Fleet Health Aggregates\n"
        << "| Metric | Count |\n"
        << "|---|---|\n"
        << "| Total Known Clients | " << bundle.summary.total_clients_known << " |\n"
        << "| Active Clients | " << bundle.summary.active_clients_count << " |\n"
        << "| Stale Clients | " << bundle.summary.stale_clients_count << " |\n"
        << "| Agent Connected Clients | " << bundle.summary.agent_connected_count << " |\n"
        << "| UWF Protected Clients | " << bundle.summary.uwf_protected_count << " |\n\n"
        << "## Diagnostic Event Log\n";

    if (bundle.events.empty()) {
        out << "_No diagnostic events recorded._\n\n";
    } else {
        out << "| Timestamp | Severity | Component | Detail |\n"
            << "|---|---|---|---|\n";
        for (const auto& ev : bundle.events) {
            out << "| " << ev.timestamp << " | "
                << severity_to_string(ev.severity) << " | "
                << ev.component << " | "
                << ev.detail << " |\n";
        }
        out << "\n";
    }

    out << "## Sanitization Verification\n"
        << "- Canary Verification: "
        << (bundle.canaries_verified ? "PASSED (Clean)" : "FAILED (Canary Detected)") << "\n"
        << "- Status: READY FOR ATTACHMENT\n";

    return out.str();
}

} // namespace nstu::support

