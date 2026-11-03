#include "nstu/update_staging.hpp"

#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace nstu::update;

namespace {

struct ScopedTestDirs {
    std::filesystem::path root;
    std::string staging;
    std::string active;
    std::string backup;

    explicit ScopedTestDirs(std::string_view name) {
        root = std::filesystem::temp_directory_path() / ("nstu_test_" + std::string(name));
        std::filesystem::remove_all(root);
        staging = (root / "staging").string();
        active = (root / "active").string();
        backup = (root / "backup").string();
        std::filesystem::create_directories(staging);
        std::filesystem::create_directories(active);
    }

    ~ScopedTestDirs() {
        try {
            std::filesystem::remove_all(root);
        } catch (...) {}
    }

    UpdateStagingConfig config() const {
        return UpdateStagingConfig{staging, active, backup};
    }
};

std::string compute_sha256_hex_for_data(std::span<const std::byte> data) {
    const auto digest = nstu::security::sha256(data);
    assert(digest.has_value());
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (const auto b : *digest) {
        ss << std::setw(2) << static_cast<unsigned int>(std::to_integer<unsigned char>(b));
    }
    return ss.str();
}

} // namespace

void test_staging_state_transitions_happy_path() {
    ScopedTestDirs dirs("happy_path");
    UpdateStagingController controller(dirs.config());
    assert(controller.init().ok());
    assert(controller.state() == UpdateState::idle);

    // Create existing active payload
    const std::filesystem::path active_bin = std::filesystem::path(dirs.active) / "nstu-service.exe";
    {
        std::ofstream ofs(active_bin, std::ios::binary);
        ofs << "active-binary-v1.0.0";
    }
    assert(std::filesystem::exists(active_bin));

    // Prepare new update package data (1000 bytes)
    std::vector<std::byte> payload(1000);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::byte>(i & 0xFF);
    }
    const std::string payload_hash = compute_sha256_hex_for_data(payload);

    UpdateManifest manifest;
    manifest.version = {1, 1, 0};
    manifest.byte_length = payload.size();
    manifest.sha256_hex = payload_hash;
    manifest.requires_reboot = true;

    // Begin staging
    const auto res_begin = controller.begin_staging(manifest, {1, 0, 0});
    assert(res_begin.ok());
    assert(controller.state() == UpdateState::downloading);
    assert(controller.record().staged_bytes == 0);
    assert(controller.record().expected_bytes == 1000);

    // Stream chunks in two pieces (400 bytes, then 600 bytes)
    std::span<const std::byte> chunk1(payload.data(), 400);
    std::span<const std::byte> chunk2(payload.data() + 400, 600);

    assert(controller.append_chunk(chunk1).ok());
    assert(controller.record().staged_bytes == 400);

    assert(controller.append_chunk(chunk2).ok());
    assert(controller.record().staged_bytes == 1000);

    // Finalize staging
    const auto res_final = controller.finalize_staging();
    assert(res_final.ok());
    assert(controller.state() == UpdateState::apply_pending_reboot);
    assert(std::filesystem::exists(controller.record().staged_package_path));

    // Apply staged update
    const auto res_apply = controller.apply_staged_update("nstu-service.exe");
    assert(res_apply.ok());
    assert(controller.state() == UpdateState::validating);
    assert(std::filesystem::exists(dirs.backup)); // Backup created
    assert(std::filesystem::exists(active_bin));   // New active binary installed

    // Verify active binary now contains new payload
    {
        std::ifstream ifs(active_bin, std::ios::binary);
        std::string installed_str((std::istreambuf_iterator<char>(ifs)),
                                  std::istreambuf_iterator<char>());
        std::vector<std::byte> installed(installed_str.size());
        std::memcpy(installed.data(), installed_str.data(), installed_str.size());
        assert(installed == payload);
    }

    // Commit update
    const auto res_commit = controller.commit_update();
    assert(res_commit.ok());
    assert(controller.state() == UpdateState::committed);
    assert(!std::filesystem::exists(dirs.backup)); // Backup purged
    assert(controller.record().active_version == SemanticVersion(1, 1, 0));
}

void test_staging_sha256_mismatch_fails_closed() {
    ScopedTestDirs dirs("sha_mismatch");
    UpdateStagingController controller(dirs.config());
    assert(controller.init().ok());

    std::vector<std::byte> payload(500, std::byte{0xAA});
    const std::string good_hash = compute_sha256_hex_for_data(payload);

    UpdateManifest manifest;
    manifest.version = {2, 0, 0};
    manifest.byte_length = payload.size();
    manifest.sha256_hex = good_hash;
    manifest.requires_reboot = false;

    assert(controller.begin_staging(manifest, {1, 0, 0}).ok());

    // Stream corrupted chunk
    std::vector<std::byte> corrupt_payload = payload;
    corrupt_payload[10] = std::byte{0xBB};

    assert(controller.append_chunk(corrupt_payload).ok());

    // Finalize must fail with digest_mismatch
    const auto res = controller.finalize_staging();
    assert(!res.ok());
    assert(res.error == UpdateStagingError::digest_mismatch);
    assert(controller.state() == UpdateState::failed);

    // Ensure .part file was deleted
    const std::filesystem::path part_file = std::filesystem::path(dirs.staging) / "update_package.part";
    assert(!std::filesystem::exists(part_file));
}

void test_staging_byte_overflow_fails_closed() {
    ScopedTestDirs dirs("byte_overflow");
    UpdateStagingController controller(dirs.config());
    assert(controller.init().ok());

    UpdateManifest manifest;
    manifest.version = {2, 0, 0};
    manifest.byte_length = 100;
    manifest.sha256_hex = "0000000000000000000000000000000000000000000000000000000000000000";

    assert(controller.begin_staging(manifest, {1, 0, 0}).ok());

    // Append 150 bytes when expected is 100
    std::vector<std::byte> excess(150, std::byte{0x11});
    const auto res = controller.append_chunk(excess);
    assert(!res.ok());
    assert(res.error == UpdateStagingError::byte_overflow);
    assert(controller.state() == UpdateState::failed);

    const std::filesystem::path part_file = std::filesystem::path(dirs.staging) / "update_package.part";
    assert(!std::filesystem::exists(part_file));
}

void test_staging_premature_finalize_fails_closed() {
    ScopedTestDirs dirs("premature_finalize");
    UpdateStagingController controller(dirs.config());
    assert(controller.init().ok());

    UpdateManifest manifest;
    manifest.version = {2, 0, 0};
    manifest.byte_length = 500;
    manifest.sha256_hex = "0000000000000000000000000000000000000000000000000000000000000000";

    assert(controller.begin_staging(manifest, {1, 0, 0}).ok());

    // Append only 200 bytes
    std::vector<std::byte> chunk(200, std::byte{0x22});
    assert(controller.append_chunk(chunk).ok());

    // Finalize must fail with truncated_payload
    const auto res = controller.finalize_staging();
    assert(!res.ok());
    assert(res.error == UpdateStagingError::truncated_payload);
    assert(controller.state() == UpdateState::failed);

    const std::filesystem::path part_file = std::filesystem::path(dirs.staging) / "update_package.part";
    assert(!std::filesystem::exists(part_file));
}

void test_staging_rollback_recovery() {
    ScopedTestDirs dirs("rollback");
    UpdateStagingController controller(dirs.config());
    assert(controller.init().ok());

    // Establish old active installation
    const std::filesystem::path active_bin = std::filesystem::path(dirs.active) / "nstu-service.exe";
    {
        std::ofstream ofs(active_bin, std::ios::binary);
        ofs << "proven-old-version-0.9.0";
    }

    std::vector<std::byte> payload(300, std::byte{0x77});
    const std::string hash = compute_sha256_hex_for_data(payload);

    UpdateManifest manifest;
    manifest.version = {1, 0, 0};
    manifest.byte_length = payload.size();
    manifest.sha256_hex = hash;
    manifest.requires_reboot = false;

    assert(controller.begin_staging(manifest, {0, 9, 0}).ok());
    assert(controller.append_chunk(payload).ok());
    assert(controller.finalize_staging().ok());
    assert(controller.apply_staged_update("nstu-service.exe").ok());
    assert(controller.state() == UpdateState::validating);

    // Simulate post-reboot health check failure: invoke rollback
    const auto res_rollback = controller.rollback_update();
    assert(res_rollback.ok());
    assert(controller.state() == UpdateState::rolling_back);

    // Verify active binary restored to original contents
    {
        std::ifstream ifs(active_bin, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
        assert(content == "proven-old-version-0.9.0");
    }
}

void test_staging_state_record_persistence() {
    ScopedTestDirs dirs("persistence");
    {
        UpdateStagingController c1(dirs.config());
        assert(c1.init().ok());

        UpdateManifest manifest;
        manifest.version = {1, 5, 0};
        manifest.byte_length = 250;
        manifest.sha256_hex = "abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890";
        manifest.requires_reboot = true;

        assert(c1.begin_staging(manifest, {1, 0, 0}).ok());
        assert(c1.state() == UpdateState::downloading);
        // Destructor called here; stream closed
    }

    // Reload controller from same staging directory (simulating restart during download)
    {
        UpdateStagingController c2(dirs.config());
        assert(c2.init().ok());
        // Interrupted download must fail-closed to failed
        assert(c2.state() == UpdateState::failed);
        assert(c2.record().expected_bytes == 250);
        assert(c2.record().target_version == SemanticVersion(1, 5, 0));
    }
}

void test_staging_exam_active_conflict() {
    ScopedTestDirs dirs("exam_conflict");
    UpdateStagingController controller(dirs.config());
    assert(controller.init().ok());

    UpdateManifest manifest;
    manifest.version = {2, 0, 0};
    manifest.byte_length = 100;
    manifest.sha256_hex = "1111222233334444555566667777888899990000aaaabbbbccccddddeeeeffff";

    // Attempting to begin staging while exam is active must be rejected
    const auto res = controller.begin_staging(manifest, {1, 0, 0}, /*is_exam_active=*/true);
    assert(!res.ok());
    assert(res.error == UpdateStagingError::exam_active_conflict);
    assert(controller.state() == UpdateState::idle);
}

void test_staging_deferral() {
    ScopedTestDirs dirs("deferral");
    UpdateStagingController controller(dirs.config());
    assert(controller.init().ok());

    controller.set_deferred("Maintenance window closed; client is thawed-reboot bound");
    assert(controller.state() == UpdateState::deferred);
    assert(controller.record().last_error == "Maintenance window closed; client is thawed-reboot bound");

    // Verify persisted across restart
    UpdateStagingController reloaded(dirs.config());
    assert(reloaded.init().ok());
    assert(reloaded.state() == UpdateState::deferred);
    assert(reloaded.record().last_error == "Maintenance window closed; client is thawed-reboot bound");
}

int main() {
    std::cout << "[update_staging_tests] running test suites...\n";

    test_staging_state_transitions_happy_path();
    std::cout << "  PASS test_staging_state_transitions_happy_path\n";

    test_staging_sha256_mismatch_fails_closed();
    std::cout << "  PASS test_staging_sha256_mismatch_fails_closed\n";

    test_staging_byte_overflow_fails_closed();
    std::cout << "  PASS test_staging_byte_overflow_fails_closed\n";

    test_staging_premature_finalize_fails_closed();
    std::cout << "  PASS test_staging_premature_finalize_fails_closed\n";

    test_staging_rollback_recovery();
    std::cout << "  PASS test_staging_rollback_recovery\n";

    test_staging_state_record_persistence();
    std::cout << "  PASS test_staging_state_record_persistence\n";

    test_staging_exam_active_conflict();
    std::cout << "  PASS test_staging_exam_active_conflict\n";

    test_staging_deferral();
    std::cout << "  PASS test_staging_deferral\n";

    std::cout << "[update_staging_tests] ALL 8 TEST SUITES PASSED.\n";
    return 0;
}
