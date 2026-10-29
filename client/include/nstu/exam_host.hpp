#pragma once

#include "nstu/agent_protocol.hpp"
#include "nstu/exam_bridge.hpp"
#include "nstu/exam_telemetry.hpp"

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nstu::client {

// The host consumes an unpacked, already-enrolled package.  It never accepts
// a path supplied by the web page; the service/provisioning layer supplies the
// package root and identity-bound context before the window is created.
struct ExamHostOptions {
    std::filesystem::path package_root;
    std::filesystem::path web_root;
    std::filesystem::path user_data_root;
    // When set, package and WebView2 profile paths must stay below the
    // installed NSTU data root. The agent supplies this value for commands
    // received from the server; leaving it empty preserves the standalone
    // package-inspection API used by diagnostics and tests.
    std::filesystem::path allowed_data_root;
    // WebView2 profile data is user-scoped and normally lives below
    // %LOCALAPPDATA%, because the interactive student account may not be able
    // to write the SYSTEM/admin-protected data root.
    std::filesystem::path allowed_user_data_root;
    std::string context_json;
    std::string expected_digest_hex;
    bool require_digest = true;
};

struct ExamHostCallbacks {
    // Called from the agent UI thread.  The callback must enqueue the message
    // for the service pipe; it must not perform blocking I/O on this thread.
    std::function<void(AgentMessage)> send_to_service;
    std::function<void(std::string)> status;
};

enum class ExamHostState : std::uint8_t {
    idle = 0,
    preparing = 1,
    initializing = 2,
    running = 3,
    degraded = 4,
    failed = 5,
};

struct ExamPackageReport {
    std::filesystem::path package_root;
    std::filesystem::path web_root;
    std::filesystem::path page_path;
    std::string manifest_json;
    std::string digest_hex;
};

struct ExamLockdownPolicy {
    std::vector<std::string> allowed_origins;   // validated, de-duplicated
    std::uint32_t duration_seconds = 0;         // 60..86400, from manifest
};

[[nodiscard]] bool extract_exam_lockdown_policy(
    std::string_view manifest_json, ExamLockdownPolicy& out,
    std::string* error = nullptr);


// Validates the bounded, unpacked package boundary without starting a browser.
// Manifest-declared media and document paths must resolve to regular files
// inside the package; deployment metadata (manifest.json/manifest.p7s) is not
// a browser asset. The digest covers sorted relative file names and file bytes,
// so a pinned digest detects both manifest and media changes. ZIP extraction
// and release signature verification remain deployment-layer responsibilities.
[[nodiscard]] bool inspect_exam_package(
    const std::filesystem::path& package_root,
    const std::filesystem::path& web_root,
    std::string_view expected_digest_hex,
    bool require_digest,
    ExamPackageReport& report,
    std::string* error = nullptr);

// Enforces the installed client package boundary before a browser is created.
// Packages must live below <data-root>\\exams. The WebView2 profile has its
// own user-writable policy root and is validated with
// validate_exam_user_data_path(). Paths are canonicalized before comparison so
// traversal and sibling-prefix tricks fail closed.
[[nodiscard]] bool validate_exam_path_policy(
    const std::filesystem::path& allowed_data_root,
    const std::filesystem::path& package_root,
    const std::filesystem::path& web_root,
    const std::filesystem::path& user_data_root,
    std::string* error = nullptr);

// Validates a user-scoped WebView2 profile path. The leaf may not be the
// policy root itself, and it may not escape that root through traversal or a
// reparse point. The leaf is allowed not to exist yet; the host creates it.
[[nodiscard]] bool validate_exam_user_data_path(
    const std::filesystem::path& allowed_user_data_root,
    const std::filesystem::path& user_data_root,
    std::string* error = nullptr);

// Parses and validates raw browser telemetry samples JSON against strict limits.
// Overrides browser-claimed identity/source and binds the native collector ID.
// Rejects hostile JSON, oversized messages, disallowed fields, invalid reasons,
// stale collector IDs, and bursts exceeding quota.
[[nodiscard]] bool parse_browser_telemetry_samples(
    std::string_view json,
    std::uint64_t native_collector_id,
    const std::set<std::string>& allowed_question_ids,
    std::vector<nstu::exam::TelemetrySample>& out_samples,
    std::string* error = nullptr);

// IPC payload serialization for AgentMessageType::exam_telemetry_samples.
[[nodiscard]] std::vector<std::byte> encode_exam_telemetry_samples_payload(
    std::span<const nstu::exam::TelemetrySample> samples);

[[nodiscard]] std::optional<std::vector<nstu::exam::TelemetrySample>>
decode_exam_telemetry_samples_payload(
    std::span<const std::byte> payload,
    std::string* error = nullptr);

// Helper for testing blocked shortcut recording outside of the Win32 hook.
void record_blocked_shortcut_for_testing(std::uint32_t count = 1) noexcept;

// Drains recorded blocked shortcuts into a coalesced TelemetrySample.
// Returns std::nullopt if count is 0.
[[nodiscard]] std::optional<nstu::exam::TelemetrySample>
drain_blocked_shortcuts(std::uint64_t collector_id,
                        std::uint64_t monotonic_elapsed_ms,
                        std::uint64_t client_time_unix_ms) noexcept;

// UI-thread telemetry state used by the native host independently of browser
// creation. Only a validated bound tuple can enable its atomic shortcut gate.
inline constexpr std::uint64_t kMaximumExamCollectorId = (1ULL << 53) - 1;
// Random process origin, then monotonically allocated IDs for every host and
// document generation. Zero means allocation unavailable; never wrap or reuse.
[[nodiscard]] std::uint64_t allocate_exam_telemetry_collector_id() noexcept;

// Local WebView JSON only; the existing wire and IPC codecs are unchanged.
[[nodiscard]] std::string make_exam_telemetry_enable_json(
    const exam::TelemetryEnable& enable, std::uint64_t collector_id,
    std::uint64_t collector_elapsed_milliseconds, bool startup_retry = false);

class ExamHostTelemetryState {
public:
    using Clock = std::chrono::steady_clock;
    // Explicit IDs allow deterministic state tests. Production uses the native
    // allocator. A valid context still starts if its ID is unavailable, with
    // collection disabled, so telemetry cannot block an otherwise valid exam.
    [[nodiscard]] bool start(exam::StateRequest context,
        std::uint64_t collector_id = allocate_exam_telemetry_collector_id(),
        Clock::time_point origin = Clock::now()) noexcept;
    void stop() noexcept;
    [[nodiscard]] bool apply_enable(const exam::TelemetryEnable& enable) noexcept;
    void advance_collector(
        std::uint64_t collector_id = allocate_exam_telemetry_collector_id(),
        Clock::time_point origin = Clock::now()) noexcept;
    // The posting callback is the real WebView publication boundary. Only a
    // successful post establishes a generation-bound pending anchor.
    [[nodiscard]] bool post_browser_enable(
        const std::function<bool(std::string_view)>& post,
        Clock::time_point now = Clock::now());
    // UI maintenance retries the original pending startup at most once/second,
    // with three attempts. Successful mapping ends retry scheduling.
    [[nodiscard]] bool maintain_browser_startup(
        const std::function<bool(std::string_view)>& post,
        Clock::time_point now = Clock::now());
    [[nodiscard]] std::optional<std::uint64_t> native_elapsed(
        Clock::time_point now = Clock::now()) noexcept;
    [[nodiscard]] bool forward_browser_samples(
        std::string_view json, const std::set<std::string>& allowed_question_ids,
        Clock::time_point receipt, std::vector<exam::TelemetrySample>& samples,
        std::string* error = nullptr);
    void record_blocked_shortcuts(std::uint32_t count = 1) noexcept;
    [[nodiscard]] std::optional<exam::TelemetrySample> drain_blocked_shortcuts(
        std::uint64_t monotonic_elapsed_ms, std::uint64_t client_time_unix_ms) noexcept;

    [[nodiscard]] bool enabled() const noexcept { return active_enable_.has_value(); }
    [[nodiscard]] std::uint64_t collector_id() const noexcept { return collector_id_; }
    [[nodiscard]] const std::optional<exam::TelemetryEnable>& active_enable() const noexcept {
        return active_enable_;
    }

private:
    exam::StateRequest context_{};
    std::uint64_t collector_id_ = 0;
    std::optional<exam::TelemetryEnable> active_enable_;
    ExamShortcutCounter shortcuts_;
    Clock::time_point generation_origin_{};
    std::optional<Clock::time_point> last_native_observation_;
    std::optional<std::uint64_t> pending_browser_anchor_;
    std::optional<std::uint64_t> last_browser_elapsed_;
    bool browser_clock_bound_ = false;
    std::optional<Clock::time_point> last_startup_attempt_;
    unsigned startup_retry_attempts_ = 0;
    // Quota belongs to the host lifetime, so document reload never resets it.
    std::optional<Clock::time_point> browser_rate_window_start_;
    std::uint32_t browser_rate_samples_ = 0;
    void clear_browser_clock() noexcept;
};

class ExamHost {
public:
    ExamHost();
    ~ExamHost();
    ExamHost(const ExamHost&) = delete;
    ExamHost& operator=(const ExamHost&) = delete;

    // Must be called on the agent's UI/STA thread.  WebView2 initialization is
    // asynchronous; success means the kiosk window and validation completed,
    // while the state may remain `initializing` until the browser is ready.
    // Agent command publications preserve enables for subsequently queued
    // operations. The default publication is for standalone callers.
    [[nodiscard]] bool start(HWND owner, const ExamHostOptions& options,
                             ExamHostCallbacks callbacks,
                             std::string* error = nullptr,
                             std::uint64_t command_publication = 0);
    // A queued stop clears only through its publication; a standalone stop
    // (publication zero) clears the entire pending enable slot.
    void stop(std::uint64_t command_publication = 0) noexcept;
    void drain_bridge();
    void enforce_foreground() noexcept;

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] ExamHostState state() const noexcept;
    [[nodiscard]] HWND window() const noexcept;
    [[nodiscard]] std::uint64_t command_publication() const noexcept;

    // Applies matching telemetry enable from service. Idempotent on duplicate.
    // Stale or wrong-context enable is rejected/ignored.
    [[nodiscard]] bool apply_telemetry_enable(
        const nstu::exam::TelemetryEnable& enable) noexcept;
    [[nodiscard]] bool telemetry_enabled() const noexcept;
    [[nodiscard]] std::uint64_t telemetry_collector_id() const noexcept;

    // Routes messages from the agent window procedure.  Returns true when the
    // message belongs to the exam host and sets `result`.
    [[nodiscard]] bool handle_window_message(UINT message, WPARAM wparam,
                                             LPARAM lparam,
                                             LRESULT& result) noexcept;

public:
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace nstu::client
