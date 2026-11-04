#pragma once

#include "nstu/update_manifest.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nstu::update {

enum class UpdateState : int {
    idle = 0,
    checking = 1,
    deferred = 2,
    downloading = 3,
    verifying = 4,
    staged = 5,
    apply_pending_reboot = 6,
    validating = 7,
    committed = 8,
    rolling_back = 9,
    failed = 10
};

[[nodiscard]] constexpr std::string_view to_string(UpdateState state) noexcept {
    switch (state) {
    case UpdateState::idle: return "idle";
    case UpdateState::checking: return "checking";
    case UpdateState::deferred: return "deferred";
    case UpdateState::downloading: return "downloading";
    case UpdateState::verifying: return "verifying";
    case UpdateState::staged: return "staged";
    case UpdateState::apply_pending_reboot: return "apply_pending_reboot";
    case UpdateState::validating: return "validating";
    case UpdateState::committed: return "committed";
    case UpdateState::rolling_back: return "rolling_back";
    case UpdateState::failed: return "failed";
    }
    return "unknown";
}

[[nodiscard]] std::optional<UpdateState> parse_update_state(std::string_view str) noexcept;

struct StagingRecord {
    std::string schema_version{"1.0"};
    SemanticVersion active_version{0, 0, 0};
    SemanticVersion target_version{0, 0, 0};
    std::string target_sha256_hex;
    std::uint64_t expected_bytes{0};
    std::uint64_t staged_bytes{0};
    UpdateState current_state{UpdateState::idle};
    std::string last_error;
    std::uint64_t state_timestamp_utc{0};
    bool requires_reboot{true};
    std::string backup_path;
    std::string staged_package_path;
};

// Canonical numeric formatting and the existing 64 KiB record limit.
// Throws std::length_error before output allocation if serialization exceeds it.
[[nodiscard]] std::string export_staging_record_json(const StagingRecord& record);
[[nodiscard]] std::optional<StagingRecord> import_staging_record_json(std::string_view json_str);

struct UpdateStagingConfig {
    std::string staging_directory; // e.g. %ProgramData%\NSTU\Updates\Staging
    std::string active_directory;  // e.g. %ProgramFiles%\NSTU\Client
    std::string backup_directory;  // e.g. %ProgramData%\NSTU\Updates\Backup
};

enum class UpdateStagingError : int {
    ok = 0,
    invalid_state = 1,
    io_error = 2,
    byte_overflow = 3,
    digest_mismatch = 4,
    truncated_payload = 5,
    backup_failed = 6,
    rollback_failed = 7,
    exam_active_conflict = 8
};

[[nodiscard]] constexpr std::string_view to_string(UpdateStagingError err) noexcept {
    switch (err) {
    case UpdateStagingError::ok: return "ok";
    case UpdateStagingError::invalid_state: return "invalid_state";
    case UpdateStagingError::io_error: return "io_error";
    case UpdateStagingError::byte_overflow: return "byte_overflow";
    case UpdateStagingError::digest_mismatch: return "digest_mismatch";
    case UpdateStagingError::truncated_payload: return "truncated_payload";
    case UpdateStagingError::backup_failed: return "backup_failed";
    case UpdateStagingError::rollback_failed: return "rollback_failed";
    case UpdateStagingError::exam_active_conflict: return "exam_active_conflict";
    }
    return "unknown";
}

struct UpdateStagingResult {
    UpdateStagingError error{UpdateStagingError::ok};
    std::string message;

    [[nodiscard]] bool ok() const noexcept {
        return error == UpdateStagingError::ok;
    }
};

class UpdateStagingController {
public:
    explicit UpdateStagingController(UpdateStagingConfig config);
    ~UpdateStagingController();

    UpdateStagingController(const UpdateStagingController&) = delete;
    UpdateStagingController& operator=(const UpdateStagingController&) = delete;
    UpdateStagingController(UpdateStagingController&&) noexcept;
    UpdateStagingController& operator=(UpdateStagingController&&) noexcept;

    // Initializes directories, loads existing staging_record.json if present.
    [[nodiscard]] UpdateStagingResult init();

    [[nodiscard]] const StagingRecord& record() const noexcept { return record_; }
    [[nodiscard]] UpdateState state() const noexcept { return record_.current_state; }
    [[nodiscard]] const UpdateStagingConfig& config() const noexcept { return config_; }

    // Marks update as deferred (e.g. outside maintenance window or rollout bucket deferred).
    void set_deferred(std::string_view reason);

    // Starts staging a new package download. Fails closed if active exam is running.
    [[nodiscard]] UpdateStagingResult begin_staging(const UpdateManifest& manifest,
                                                   const SemanticVersion& current_version,
                                                   bool is_exam_active = false);

    // Appends an incoming payload chunk. Computes incremental SHA-256. Fails closed on byte overflow.
    [[nodiscard]] UpdateStagingResult append_chunk(std::span<const std::byte> chunk);

    // Finalizes download: validates exact length and complete SHA-256 digest against manifest.
    // On success: renames .part to .pkg/.msi, updates staging record to staged/apply_pending_reboot.
    // On failure: immediately deletes .part file and marks state failed.
    [[nodiscard]] UpdateStagingResult finalize_staging();

    // Aborts active staging: deletes partial file, sets state to failed/idle.
    UpdateStagingResult abort_staging(std::string_view reason);

    // Moves active directory to backup directory, and installs staged package into active directory.
    // Sets state to validating.
    [[nodiscard]] UpdateStagingResult apply_staged_update(std::string_view primary_binary_name = "nstu-service.exe");

    // Confirms successful post-update validation: purges backup directory and marks committed.
    [[nodiscard]] UpdateStagingResult commit_update();

    // Health check failed after update: restores active directory from backup, cleans up failed state.
    [[nodiscard]] UpdateStagingResult rollback_update();

private:
    UpdateStagingConfig config_;
    StagingRecord record_;

    void* hash_handle_{nullptr};
    void* alg_handle_{nullptr};
    std::vector<std::uint8_t> hash_object_;
    bool is_streaming_{false};

    void cleanup_hash_context() noexcept;
    bool save_record_to_disk();
    [[nodiscard]] std::string get_part_file_path() const;
    [[nodiscard]] std::string get_final_package_path() const;
    [[nodiscard]] std::string get_record_file_path() const;
};

} // namespace nstu::update
