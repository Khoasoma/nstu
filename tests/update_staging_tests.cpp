#include "nstu/update_staging.hpp"

#include <windows.h>
#include <array>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <locale>
#include <stdexcept>
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

namespace {

std::string replace_record_text(std::string text, std::string_view old,
                                std::string_view replacement) {
    const auto position = text.find(old);
    assert(position != std::string::npos);
    text.replace(position, old.size(), replacement);
    return text;
}

void write_test_file(const std::filesystem::path& path, std::string_view content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    assert(out.good());
}

std::string read_test_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    assert(in.is_open());
    return std::string(std::istreambuf_iterator<char>(in), {});
}

void test_record_json_preflight() {
    assert(!import_staging_record_json("{}"));
    const auto valid = export_staging_record_json(StagingRecord{});
    assert(import_staging_record_json(valid));
    constexpr std::array<std::string_view, 12> fields{
        "schema_version", "active_version", "target_version", "target_sha256_hex",
        "expected_bytes", "staged_bytes", "current_state", "last_error",
        "state_timestamp_utc", "requires_reboot", "backup_path", "staged_package_path"};
    for (const auto field : fields) {
        auto missing = valid;
        const auto key = missing.find('"' + std::string(field) + '"');
        assert(key != std::string::npos);
        const auto start = missing.rfind('\n', key) + 1;
        const auto end = missing.find('\n', key);
        assert(end != std::string::npos);
        missing.erase(start, end - start + 1);
        if (field == "staged_package_path") {
            const auto comma = missing.rfind(",\n");
            assert(comma != std::string::npos);
            missing.erase(comma, 1);
        }
        assert(!import_staging_record_json(missing));
    }
    for (const auto& malformed : {
        replace_record_text(valid, "{\n", "{\n\"unknown\": 0,\n"),
        replace_record_text(valid, "{\n", "{\n\"schema_version\": \"1.0\",\n"),
        replace_record_text(valid, "\"schema_version\": \"1.0\"", "\"schema_version\": \"2.0\""),
        replace_record_text(valid, "\"schema_version\": \"1.0\",", "\"schema_version\": \"1.0\""),
        replace_record_text(valid, "\"current_state\": \"idle\"", "\"current_state\": \"unknown\""),
        replace_record_text(valid, "\"expected_bytes\": 0", "\"expected_bytes\": true"),
        replace_record_text(valid, "\"expected_bytes\": 0", "\"expected_bytes\": 00"),
        replace_record_text(valid, "\"expected_bytes\": 0", "\"expected_bytes\": 18446744073709551616"),
        replace_record_text(valid, "\"last_error\": \"\"", "\"last_error\": \"\\q\""),
        replace_record_text(valid, "\"last_error\": \"\"", "\"last_error\": \"raw\ncontrol\""),
        replace_record_text(valid, "\"last_error\": \"\"", "\"last_error\": \"\\uGGGG\""),
        replace_record_text(valid, "\"last_error\": \"\"", "\"last_error\": \"\\uD800\""),
        replace_record_text(valid, "\"last_error\": \"\"", "\"last_error\": \"\\uDC00\""),
        replace_record_text(valid, "\"staged_package_path\": \"\"", "\"staged_package_path\": \"\","),
        valid + "garbage"}) {
        assert(!import_staging_record_json(malformed));
    }
    StagingRecord escaped;
    escaped.last_error = "quote\" slash\\ tab\t newline\n return\r backspace\b formfeed\f";
    escaped.last_error.push_back('\1');
    escaped.last_error += "\xc3\xa9";
    const auto roundtrip = import_staging_record_json(export_staging_record_json(escaped));
    assert(roundtrip && roundtrip->last_error == escaped.last_error);
    const auto unicode = import_staging_record_json(replace_record_text(
        valid, "\"last_error\": \"\"", "\"last_error\": \"\\u00e9\\uD83D\\uDE00\""));
    assert(unicode && unicode->last_error == "\xc3\xa9\xf0\x9f\x98\x80");
    StagingRecord inconsistent;
    inconsistent.expected_bytes = 1;
    inconsistent.staged_bytes = 2;
    assert(!import_staging_record_json(export_staging_record_json(inconsistent)));
}

void test_record_startup_preflight() {
    ScopedTestDirs dirs("record_preflight_" + std::to_string(GetCurrentProcessId()));
    UpdateStagingController controller(dirs.config());
    assert(controller.init().ok());
    controller.set_deferred("trusted prior state");
    const auto before = export_staging_record_json(controller.record());
    const auto record = std::filesystem::path(dirs.staging) / "staging_record.json";
    const auto part = std::filesystem::path(dirs.staging) / "update_package.part";
    const auto package = std::filesystem::path(dirs.staging) / "update_package.pkg";
    const auto active = std::filesystem::path(dirs.active) / "nstu-service.exe";
    std::filesystem::create_directories(dirs.backup);
    const auto backup = std::filesystem::path(dirs.backup) / "nstu-service.exe";
    for (const auto& path : {part, package, active, backup}) write_test_file(path, "preserved canary");
    for (const auto& bad : {std::string{}, std::string("{}"), std::string(64 * 1024 + 1, ' ')}) {
        write_test_file(record, bad);
        const auto result = controller.init();
        assert(!result.ok());
        assert(result.error == UpdateStagingError::invalid_state);
        assert(result.message.size() <= 192);
        assert(export_staging_record_json(controller.record()) == before);
        assert(read_test_file(record) == bad);
        for (const auto& path : {part, package, active, backup}) assert(read_test_file(path) == "preserved canary");
    }
    write_test_file(record, before);
    const HANDLE lock = CreateFileW(record.c_str(), GENERIC_READ, 0, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(lock != INVALID_HANDLE_VALUE);
    const auto locked = controller.init();
    assert(CloseHandle(lock));
    assert(!locked.ok() && locked.error == UpdateStagingError::io_error);
    assert(locked.message.size() <= 192);
    assert(export_staging_record_json(controller.record()) == before);
    assert(read_test_file(record) == before);
    std::filesystem::remove(record);
    std::filesystem::create_directory(record);
    const auto directory = controller.init();
    assert(!directory.ok() && directory.error == UpdateStagingError::io_error);
    assert(directory.message.size() <= 192);
    assert(export_staging_record_json(controller.record()) == before);
    std::filesystem::remove(record);
    const auto missing_target = dirs.root / "missing-record-target";
    assert(!std::filesystem::exists(missing_target));
    assert(CreateSymbolicLinkW(record.c_str(), missing_target.c_str(),
                               SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE));
    const auto link_attributes = GetFileAttributesW(record.c_str());
    assert(link_attributes != INVALID_FILE_ATTRIBUTES);
    assert((link_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0);
    const auto dangling = controller.init();
    assert(!dangling.ok() && dangling.error == UpdateStagingError::io_error);
    assert(dangling.message.size() <= 192);
    assert(export_staging_record_json(controller.record()) == before);
    const auto preserved_attributes = GetFileAttributesW(record.c_str());
    assert(preserved_attributes != INVALID_FILE_ATTRIBUTES);
    assert((preserved_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0);
    assert(!std::filesystem::exists(missing_target));
    for (const auto& path : {part, package, active, backup}) assert(read_test_file(path) == "preserved canary");
    assert(DeleteFileW(record.c_str()));
    auto boundary = export_staging_record_json(StagingRecord{});
    boundary.resize(64 * 1024, ' ');
    write_test_file(record, boundary);
    assert(controller.init().ok());
    assert(controller.state() == UpdateState::idle);
    boundary.push_back(' ');
    write_test_file(record, boundary);
    const auto oversized = controller.init();
    assert(!oversized.ok() && oversized.error == UpdateStagingError::invalid_state);
    assert(controller.state() == UpdateState::idle);
    assert(read_test_file(record) == boundary);
}

void test_record_path_preflight() {
    ScopedTestDirs dirs("record_paths_" + std::to_string(GetCurrentProcessId()));
    UpdateStagingController controller(dirs.config());
    assert(controller.init().ok());
    controller.set_deferred("trusted prior state");
    const auto before = export_staging_record_json(controller.record());
    const auto record = std::filesystem::path(dirs.staging) / "staging_record.json";
    const auto part = std::filesystem::path(dirs.staging) / "update_package.part";
    const auto package = std::filesystem::path(dirs.staging) / "update_package.pkg";
    const auto active = std::filesystem::path(dirs.active) / "nstu-service.exe";
    std::filesystem::create_directories(dirs.backup);
    const auto backup = std::filesystem::path(dirs.backup) / "nstu-service.exe";
    for (const auto& path : {part, package, active, backup}) write_test_file(path, "preserved path canary");
    std::vector<StagingRecord> invalid;
    StagingRecord candidate;
    candidate.current_state = UpdateState::downloading;
    candidate.expected_bytes = 10;
    candidate.target_sha256_hex = std::string(64, '0');
    for (const auto& path : {(dirs.root / "foreign.pkg").string(),
                            dirs.staging + "\\..\\staging\\update_package.pkg",
                            dirs.staging + "\\.\\update_package.pkg",
                            std::string("C:drive-relative.pkg"),
                            std::string("\\\\no-probe.invalid\\share\\package.pkg")}) {
        candidate.staged_package_path = path;
        invalid.push_back(candidate);
    }
    candidate.staged_package_path = package.string() + std::string(1, '\0') + "suffix";
    invalid.push_back(candidate);
    candidate.staged_package_path.clear();
    candidate.backup_path = (dirs.root / "foreign_backup").string();
    invalid.push_back(candidate);
    for (const auto& value : invalid) {
        const auto bytes = export_staging_record_json(value);
        write_test_file(record, bytes);
        const auto result = controller.init();
        assert(!result.ok() && result.error == UpdateStagingError::invalid_state);
        assert(result.message.size() <= 192);
        assert(export_staging_record_json(controller.record()) == before);
        assert(read_test_file(record) == bytes);
        for (const auto& path : {part, package, active, backup}) assert(read_test_file(path) == "preserved path canary");
    }
    candidate.current_state = UpdateState::staged;
    candidate.staged_bytes = candidate.expected_bytes;
    candidate.backup_path = dirs.backup;
    candidate.staged_package_path = package.generic_string();
    write_test_file(record, export_staging_record_json(candidate));
    assert(controller.init().ok());
    assert(controller.state() == UpdateState::staged);
    assert(controller.record().backup_path == dirs.backup);
    assert(controller.record().staged_package_path == package.string());
    for (const auto& path : {part, package, active, backup}) assert(read_test_file(path) == "preserved path canary");
}

void test_record_serialization_limits() {
    StagingRecord record;
    const auto empty = export_staging_record_json(record);
    constexpr std::string_view expected_default =
        "{\n"
        "  \"schema_version\": \"1.0\",\n"
        "  \"active_version\": \"0.0.0\",\n"
        "  \"target_version\": \"0.0.0\",\n"
        "  \"target_sha256_hex\": \"\",\n"
        "  \"expected_bytes\": 0,\n"
        "  \"staged_bytes\": 0,\n"
        "  \"current_state\": \"idle\",\n"
        "  \"last_error\": \"\",\n"
        "  \"state_timestamp_utc\": 0,\n"
        "  \"requires_reboot\": true,\n"
        "  \"backup_path\": \"\",\n"
        "  \"staged_package_path\": \"\"\n"
        "}\n";
    assert(empty == expected_default);
    record.last_error.assign(64 * 1024 - empty.size(), 'a');
    const auto boundary = export_staging_record_json(record);
    assert(boundary.size() == 64 * 1024);
    assert(import_staging_record_json(boundary));
    record.last_error.push_back('a');
    bool rejected = false;
    try { (void)export_staging_record_json(record); }
    catch (const std::length_error& error) { rejected = true; assert(std::strlen(error.what()) <= 192); }
    assert(rejected);
    record.last_error.assign(12000, '\1');
    rejected = false;
    try { (void)export_staging_record_json(record); }
    catch (const std::length_error&) { rejected = true; }
    assert(rejected);
    record.last_error.clear();
    record.backup_path.assign(64 * 1024 + 1, 'b');
    rejected = false;
    try { (void)export_staging_record_json(record); }
    catch (const std::length_error&) { rejected = true; }
    assert(rejected);
}

struct GroupedRecordNumbers : std::numpunct<char> {
    GroupedRecordNumbers() : std::numpunct<char>(1) {}
    char do_thousands_sep() const override { return ','; }
    std::string do_grouping() const override { return "\3"; }
};

void test_record_serialization_locale() {
    StagingRecord record;
    record.active_version = {1234, 5678, 9012};
    record.target_version = {4321, 8765, 2109};
    record.expected_bytes = std::numeric_limits<std::uint64_t>::max();
    record.staged_bytes = 1234567890;
    record.state_timestamp_utc = 9876543210;
    GroupedRecordNumbers grouped;
    const auto prior = std::locale::global(std::locale(std::locale::classic(), &grouped));
    std::string bytes;
    try { bytes = export_staging_record_json(record); }
    catch (...) { std::locale::global(prior); throw; }
    std::locale::global(prior);
    assert(bytes.find("\"active_version\": \"1234.5678.9012\"") != std::string::npos);
    assert(bytes.find("\"target_version\": \"4321.8765.2109\"") != std::string::npos);
    assert(bytes.find("\"expected_bytes\": 18446744073709551615") != std::string::npos);
    assert(bytes.find("\"staged_bytes\": 1234567890") != std::string::npos);
    assert(bytes.find("\"state_timestamp_utc\": 9876543210") != std::string::npos);
    const auto parsed = import_staging_record_json(bytes);
    assert(parsed && parsed->active_version == record.active_version);
    assert(parsed->target_version == record.target_version);
    assert(parsed->expected_bytes == record.expected_bytes);
}

void test_rejected_serialization_does_not_touch_disk() {
    const std::string unsupported(12000, '\1');
    ScopedTestDirs dirs("serialization_save_" + std::to_string(GetCurrentProcessId()));
    UpdateStagingController controller(dirs.config());
    assert(controller.init().ok());
    controller.set_deferred("existing valid state");
    const auto record = std::filesystem::path(dirs.staging) / "staging_record.json";
    const auto before = read_test_file(record);
    controller.set_deferred(unsupported);
    assert(read_test_file(record) == before);
    assert(import_staging_record_json(read_test_file(record)));
    std::filesystem::remove_all(dirs.staging);
    UpdateStagingController missing(dirs.config());
    missing.set_deferred(unsupported);
    assert(!std::filesystem::exists(dirs.staging));
}

void test_record_hardlink_preflight() {
    ScopedTestDirs dirs("record_hardlink_" + std::to_string(GetCurrentProcessId()));
    UpdateStagingController controller(dirs.config());
    assert(controller.init().ok());
    controller.set_deferred("trusted prior state");
    const auto before = export_staging_record_json(controller.record());
    const auto record = std::filesystem::path(dirs.staging) / "staging_record.json";
    const auto source = dirs.root / "linked-source.json";
    const auto part = std::filesystem::path(dirs.staging) / "update_package.part";
    const auto package = std::filesystem::path(dirs.staging) / "update_package.pkg";
    const auto active = std::filesystem::path(dirs.active) / "nstu-service.exe";
    std::filesystem::create_directories(dirs.backup);
    const auto backup = std::filesystem::path(dirs.backup) / "nstu-service.exe";
    const std::array paths{part, package, active, backup};
    for (const auto& path : paths) write_test_file(path, "preserved leaf canary");
    StagingRecord candidate;
    candidate.current_state = UpdateState::downloading;
    candidate.expected_bytes = 10;
    candidate.staged_bytes = 4;
    candidate.target_sha256_hex = std::string(64, '0');
    candidate.staged_package_path = package.string();
    candidate.backup_path = dirs.backup;
    const auto bytes = export_staging_record_json(candidate);
    assert(import_staging_record_json(bytes));
    write_test_file(source, bytes);
    assert(DeleteFileW(record.c_str()));
    assert(CreateHardLinkW(record.c_str(), source.c_str(), nullptr));
    const HANDLE probe = CreateFileW(record.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(probe != INVALID_HANDLE_VALUE);
    BY_HANDLE_FILE_INFORMATION information{};
    const bool inspected = GetFileInformationByHandle(probe, &information) != 0;
    const bool probe_closed = CloseHandle(probe) != 0;
    assert(inspected && probe_closed && information.nNumberOfLinks >= 2);
    const auto result = controller.init();
    const auto memory_after = export_staging_record_json(controller.record());
    const auto record_after = read_test_file(record);
    const auto source_after = read_test_file(source);
    std::array<std::string, 4> observed;
    for (std::size_t i = 0; i < paths.size(); ++i) {
        if (std::filesystem::exists(paths[i])) observed[i] = read_test_file(paths[i]);
    }
    const bool link_removed = DeleteFileW(record.c_str()) != 0;
    std::cerr << "[leaf hardlink witness] accepted=" << result.ok()
              << " memory_changed=" << (memory_after != before)
              << " linked_bytes_changed=" << (source_after != bytes) << '\n';
    assert(link_removed);
    assert(!result.ok() && result.error == UpdateStagingError::io_error);
    assert(result.message.size() <= 192);
    assert(memory_after == before && record_after == bytes && source_after == bytes);
    for (const auto& content : observed) assert(content == "preserved leaf canary");
    write_test_file(record, before);
    assert(controller.init().ok());
    assert(export_staging_record_json(controller.record()) == before);
}

void test_record_native_sharing_and_recovery() {
    ScopedTestDirs dirs("record_sharing_" + std::to_string(GetCurrentProcessId()));
    UpdateStagingController controller(dirs.config());
    assert(controller.init().ok());
    controller.set_deferred("trusted sharing state");
    const auto before = export_staging_record_json(controller.record());
    const auto record = std::filesystem::path(dirs.staging) / "staging_record.json";
    for (const DWORD access : std::array<DWORD, 2>{GENERIC_WRITE, DELETE}) {
        const HANDLE other = CreateFileW(record.c_str(), access,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        assert(other != INVALID_HANDLE_VALUE);
        const auto result = controller.init();
        const auto memory_after = export_staging_record_json(controller.record());
        const bool other_closed = CloseHandle(other) != 0;
        std::cerr << "[leaf sharing witness] access=" << access
                  << " accepted=" << result.ok() << '\n';
        assert(other_closed);
        assert(!result.ok() && result.error == UpdateStagingError::io_error);
        assert(result.message.size() <= 192);
        assert(memory_after == before && read_test_file(record) == before);
        assert(controller.init().ok());
    }
    const HANDLE reader = CreateFileW(record.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(reader != INVALID_HANDLE_VALUE);
    const auto readable = controller.init();
    const bool reader_closed = CloseHandle(reader) != 0;
    assert(reader_closed && readable.ok());
    assert(export_staging_record_json(controller.record()) == before);
    const auto part = std::filesystem::path(dirs.staging) / "update_package.part";
    const auto package = std::filesystem::path(dirs.staging) / "update_package.pkg";
    const auto active = std::filesystem::path(dirs.active) / "nstu-service.exe";
    std::filesystem::create_directories(dirs.backup);
    const auto backup = std::filesystem::path(dirs.backup) / "nstu-service.exe";
    for (const auto& path : {part, package, active, backup}) write_test_file(path, "recovery canary");
    StagingRecord downloading;
    downloading.current_state = UpdateState::downloading;
    downloading.expected_bytes = 10;
    downloading.staged_bytes = 4;
    downloading.target_sha256_hex = std::string(64, '0');
    downloading.staged_package_path = package.string();
    downloading.backup_path = dirs.backup;
    write_test_file(record, export_staging_record_json(downloading));
    const auto recovered = controller.init();
    const auto persisted = import_staging_record_json(read_test_file(record));
    assert(recovered.ok() && controller.state() == UpdateState::failed);
    assert(persisted && persisted->current_state == UpdateState::failed);
    assert(persisted->last_error == "Download interrupted by system restart");
    assert(persisted->expected_bytes == 10 && persisted->staged_bytes == 4);
    assert(!std::filesystem::exists(part));
    for (const auto& path : {package, active, backup}) assert(read_test_file(path) == "recovery canary");
}

} // namespace

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
    test_record_json_preflight();
    test_record_startup_preflight();
    test_record_path_preflight();
    std::cout << "[update_staging_tests] ALL PREFLIGHT REGRESSIONS PASSED.\n";
    test_record_serialization_limits();
    test_record_serialization_locale();
    test_rejected_serialization_does_not_touch_disk();
    std::cout << "[update_staging_tests] ALL SERIALIZATION REGRESSIONS PASSED.\n";
    test_record_hardlink_preflight();
    test_record_native_sharing_and_recovery();
    std::cout << "[update_staging_tests] ALL LEAF READ REGRESSIONS PASSED.\n";
    return 0;
}
