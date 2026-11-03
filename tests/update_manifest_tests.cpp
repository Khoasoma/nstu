#include "nstu/update_manifest.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using namespace nstu::update;

void test_semantic_version_parsing_and_ordering() {
    // Valid parsing
    const auto v1 = SemanticVersion::parse("1.0.0");
    assert(v1.has_value());
    assert(v1->major == 1 && v1->minor == 0 && v1->patch == 0);
    assert(v1->to_string() == "1.0.0");

    const auto v2 = SemanticVersion::parse("2.15.8");
    assert(v2.has_value());
    assert(v2->major == 2 && v2->minor == 15 && v2->patch == 8);

    // Invalid parsing
    assert(!SemanticVersion::parse(""));
    assert(!SemanticVersion::parse("1"));
    assert(!SemanticVersion::parse("1.2"));
    assert(!SemanticVersion::parse("1.2.3.4"));
    assert(!SemanticVersion::parse("a.b.c"));
    assert(!SemanticVersion::parse("1.-2.3"));
    assert(!SemanticVersion::parse(".."));

    // Monotonic comparison
    const auto va = *SemanticVersion::parse("1.0.0");
    const auto vb = *SemanticVersion::parse("1.0.1");
    const auto vc = *SemanticVersion::parse("1.1.0");
    const auto vd = *SemanticVersion::parse("2.0.0");
    const auto ve = *SemanticVersion::parse("1.0.0");

    assert(va < vb);
    assert(vb < vc);
    assert(vc < vd);
    assert(va == ve);
    assert(vd > vc);
    assert(vd > vb);
    assert(vd > va);
}

void test_asymmetric_ecdsa_manifest_signing_and_signature_verification() {
    const auto keypair = generate_update_signing_keypair();
    assert(keypair.has_value());
    assert(!keypair->public_key.empty());
    assert(!keypair->private_key.empty());

    const auto wrong_keypair = generate_update_signing_keypair();
    assert(wrong_keypair.has_value());

    UpdateManifest manifest;
    manifest.package_role = PackageRole::client;
    manifest.version = {1, 2, 0};
    manifest.min_supported_version = {1, 0, 0};
    manifest.package_url = "https://updates.school.edu/nstu-client-1.2.0.msi";
    manifest.byte_length = 15000000;
    manifest.sha256_hex = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    manifest.channel = ReleaseChannel::ltsc_stable;
    manifest.release_timestamp_utc = 1730000000;
    manifest.rollout_percentage = 50;
    manifest.min_windows_build = 17763;
    manifest.requires_reboot = true;
    manifest.key_id = 1;

    // Sign manifest using private key
    assert(sign_update_manifest(manifest, keypair->private_key));
    assert(!manifest.signature_hex.empty());
    assert(manifest.signature_hex.size() == 128); // 64 bytes = 128 hex chars for ECDSA P-256

    // Verify using public key only (trust root)
    assert(verify_manifest_signature(manifest, keypair->public_key));

    // Verify with wrong public key
    assert(!verify_manifest_signature(manifest, wrong_keypair->public_key));

    // Tampering test: tamper rollout_percentage (must invalidate signature)
    auto tampered_rollout = manifest;
    tampered_rollout.rollout_percentage = 100;
    assert(!verify_manifest_signature(tampered_rollout, keypair->public_key));

    // Tampering test: tamper version
    auto tampered_version = manifest;
    tampered_version.version = {1, 2, 1};
    assert(!verify_manifest_signature(tampered_version, keypair->public_key));

    // Tampering test: tamper package_url
    auto tampered_url = manifest;
    tampered_url.package_url = "https://updates.school.edu/malicious.msi";
    assert(!verify_manifest_signature(tampered_url, keypair->public_key));

    // Tampering test: tamper signature
    auto bad_sig = manifest;
    bad_sig.signature_hex[0] = (bad_sig.signature_hex[0] == 'a') ? 'b' : 'a';
    assert(!verify_manifest_signature(bad_sig, keypair->public_key));
}

void test_anti_downgrade_and_equal_version_enforcement() {
    const auto keypair = generate_update_signing_keypair();
    assert(keypair.has_value());

    UpdateManifest manifest;
    manifest.package_role = PackageRole::client;
    manifest.version = {1, 4, 9};
    manifest.min_supported_version = {1, 0, 0};
    manifest.package_url = "https://updates.school.edu/pkg.msi";
    manifest.byte_length = 1000;
    manifest.sha256_hex = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    manifest.channel = ReleaseChannel::ltsc_stable;
    manifest.release_timestamp_utc = 1730000000;
    manifest.min_windows_build = 17763;
    assert(sign_update_manifest(manifest, keypair->private_key));

    ClientEnvironment env;
    env.installed_role = PackageRole::client;
    env.current_version = {1, 5, 0};
    env.current_windows_build = 19045;
    env.machine_guid = "{11111111-2222-3333-4444-555555555555}";
    env.allow_same_version = false;

    // 1. Candidate 1.4.9 < installed 1.5.0 -> downgrade rejected
    const auto res1 = verify_manifest_against_environment(manifest, env, keypair->public_key);
    assert(!res1.ok());
    assert(res1.error == ManifestVerificationError::downgrade_detected);

    // 2. Candidate 1.5.0 == installed 1.5.0 with allow_same_version = false -> already_up_to_date
    manifest.version = {1, 5, 0};
    assert(sign_update_manifest(manifest, keypair->private_key));
    const auto res2 = verify_manifest_against_environment(manifest, env, keypair->public_key);
    assert(!res2.ok());
    assert(res2.error == ManifestVerificationError::already_up_to_date);

    // 3. Candidate 1.5.0 == installed 1.5.0 with allow_same_version = true -> ok
    env.allow_same_version = true;
    const auto res3 = verify_manifest_against_environment(manifest, env, keypair->public_key);
    assert(res3.ok());

    // 4. Candidate 1.6.0 > installed 1.5.0 -> ok
    env.allow_same_version = false;
    manifest.version = {1, 6, 0};
    assert(sign_update_manifest(manifest, keypair->private_key));
    const auto res4 = verify_manifest_against_environment(manifest, env, keypair->public_key);
    assert(res4.ok());

    // 5. Installed below candidate min_supported_version -> below_minimum_supported_version
    manifest.min_supported_version = {1, 5, 5};
    assert(sign_update_manifest(manifest, keypair->private_key));
    const auto res5 = verify_manifest_against_environment(manifest, env, keypair->public_key);
    assert(!res5.ok());
    assert(res5.error == ManifestVerificationError::below_minimum_supported_version);
}

void test_environment_os_and_role_compatibility() {
    const auto keypair = generate_update_signing_keypair();
    assert(keypair.has_value());

    UpdateManifest manifest;
    manifest.package_role = PackageRole::client;
    manifest.version = {2, 0, 0};
    manifest.min_supported_version = {1, 0, 0};
    manifest.package_url = "https://updates.school.edu/pkg.msi";
    manifest.byte_length = 5000;
    manifest.sha256_hex = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    manifest.channel = ReleaseChannel::ltsc_stable;
    manifest.release_timestamp_utc = 1730000000;
    manifest.min_windows_build = 19041; // Requires Windows 10 2004+
    assert(sign_update_manifest(manifest, keypair->private_key));

    ClientEnvironment env;
    env.installed_role = PackageRole::client;
    env.current_version = {1, 0, 0};
    env.current_windows_build = 17763; // LTSC 2019 (build 17763)
    env.machine_guid = "{AAAA-BBBB-CCCC}";

    // OS build too old
    const auto res_os = verify_manifest_against_environment(manifest, env, keypair->public_key);
    assert(!res_os.ok());
    assert(res_os.error == ManifestVerificationError::incompatible_os_build);

    // Satisfy OS build requirement
    env.current_windows_build = 19045;
    const auto res_os_ok = verify_manifest_against_environment(manifest, env, keypair->public_key);
    assert(res_os_ok.ok());

    // Role mismatch
    env.installed_role = PackageRole::server;
    const auto res_role = verify_manifest_against_environment(manifest, env, keypair->public_key);
    assert(!res_role.ok());
    assert(res_role.error == ManifestVerificationError::role_mismatch);

    // Transport HTTPS mandate
    env.installed_role = PackageRole::client;
    manifest.package_url = "http://insecure.updates.school.edu/pkg.msi";
    assert(sign_update_manifest(manifest, keypair->private_key));
    const auto res_http = verify_manifest_against_environment(manifest, env, keypair->public_key);
    assert(!res_http.ok());
    assert(res_http.error == ManifestVerificationError::invalid_url);

    // Explicit allow_insecure_http override
    env.allow_insecure_http = true;
    const auto res_http_allowed = verify_manifest_against_environment(manifest, env, keypair->public_key);
    assert(res_http_allowed.ok());
}

void test_deterministic_canary_rollout_bucketing() {
    // Empty GUID returns nullopt
    assert(!compute_canary_bucket("").has_value());

    // Valid GUIDs yield values in [0, 99]
    const std::string guid1 = "{12345678-1234-1234-1234-123456789ABC}";
    const std::string guid2 = "{87654321-4321-4321-4321-CBA987654321}";

    const auto b1 = compute_canary_bucket(guid1);
    const auto b2 = compute_canary_bucket(guid2);

    assert(b1.has_value());
    assert(b2.has_value());
    assert(*b1 < 100);
    assert(*b2 < 100);

    // Deterministic: multiple computations yield identical bucket
    assert(compute_canary_bucket(guid1) == b1);
    assert(compute_canary_bucket(guid2) == b2);

    // Alias compute_canary_rollout_bucket matches
    assert(compute_canary_rollout_bucket(guid1) == b1);

    // Rollout percentage gating
    const auto keypair = generate_update_signing_keypair();
    assert(keypair.has_value());

    UpdateManifest manifest;
    manifest.package_role = PackageRole::client;
    manifest.version = {2, 0, 0};
    manifest.min_supported_version = {1, 0, 0};
    manifest.package_url = "https://updates.school.edu/pkg.msi";
    manifest.byte_length = 5000;
    manifest.sha256_hex = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    manifest.channel = ReleaseChannel::pilot;
    manifest.release_timestamp_utc = 1730000000;
    manifest.min_windows_build = 17763;
    manifest.rollout_percentage = 0; // Paused rollout
    assert(sign_update_manifest(manifest, keypair->private_key));

    ClientEnvironment env;
    env.installed_role = PackageRole::client;
    env.current_version = {1, 0, 0};
    env.current_windows_build = 19045;
    env.machine_guid = guid1;

    // 0% rollout defers all clients
    const auto res_zero = verify_manifest_against_environment(manifest, env, keypair->public_key);
    assert(!res_zero.ok());
    assert(res_zero.error == ManifestVerificationError::rollout_deferred);

    // 100% rollout accepts all clients
    manifest.rollout_percentage = 100;
    assert(sign_update_manifest(manifest, keypair->private_key));
    const auto res_full = verify_manifest_against_environment(manifest, env, keypair->public_key);
    assert(res_full.ok());

    // Canary bucket gating
    manifest.rollout_percentage = *b1; // Exactly at threshold -> deferred
    assert(sign_update_manifest(manifest, keypair->private_key));
    const auto res_boundary = verify_manifest_against_environment(manifest, env, keypair->public_key);
    assert(!res_boundary.ok());
    assert(res_boundary.error == ManifestVerificationError::rollout_deferred);

    if (*b1 < 99) {
        manifest.rollout_percentage = *b1 + 1; // Strictly greater than client bucket -> accepted
        assert(sign_update_manifest(manifest, keypair->private_key));
        const auto res_accepted = verify_manifest_against_environment(manifest, env, keypair->public_key);
        assert(res_accepted.ok());
    }
}

void test_canonical_json_export_and_import() {
    const auto keypair = generate_update_signing_keypair();
    assert(keypair.has_value());

    UpdateManifest src;
    src.schema_version = "1.0";
    src.package_role = PackageRole::client;
    src.version = {2, 3, 4};
    src.min_supported_version = {2, 0, 0};
    src.package_url = "https://updates.school.edu/client-v2.3.4.msi";
    src.byte_length = 24567890;
    src.sha256_hex = "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
    src.channel = ReleaseChannel::ltsc_stable;
    src.release_timestamp_utc = 1731112233;
    src.rollout_percentage = 25;
    src.min_windows_build = 19044;
    src.requires_reboot = true;
    src.key_id = 42;
    assert(sign_update_manifest(src, keypair->private_key));

    const std::string json = export_update_manifest_json(src);
    assert(!json.empty());

    const auto [imported, err] = import_update_manifest_json(json);
    assert(err.ok());
    assert(imported.has_value());

    assert(imported->schema_version == src.schema_version);
    assert(imported->package_role == src.package_role);
    assert(imported->version == src.version);
    assert(imported->min_supported_version == src.min_supported_version);
    assert(imported->package_url == src.package_url);
    assert(imported->byte_length == src.byte_length);
    assert(imported->sha256_hex == src.sha256_hex);
    assert(imported->channel == src.channel);
    assert(imported->release_timestamp_utc == src.release_timestamp_utc);
    assert(imported->rollout_percentage == src.rollout_percentage);
    assert(imported->min_windows_build == src.min_windows_build);
    assert(imported->requires_reboot == src.requires_reboot);
    assert(imported->key_id == src.key_id);
    assert(imported->signature_hex == src.signature_hex);

    // Imported manifest verifies with public trust root
    assert(verify_manifest_signature(*imported, keypair->public_key));
}

void test_fail_closed_json_parser_hardening() {
    // 1. Trailing garbage after closing brace
    const std::string trailing_garbage = R"({
        "schema_version": "1.0",
        "package_role": "client",
        "version": "1.0.0",
        "min_supported_version": "1.0.0",
        "package_url": "https://updates.school.edu/pkg.msi",
        "byte_length": 100,
        "sha256_hex": "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789",
        "channel": "ltsc-stable",
        "release_timestamp_utc": 1000,
        "min_windows_build": 17763
    } MALICIOUS_TRAILING_DATA)";
    const auto [res1, err1] = import_update_manifest_json(trailing_garbage);
    assert(!res1.has_value());
    assert(err1.error == ManifestVerificationError::invalid_json);

    // 2. Trailing comma before closing brace
    const std::string trailing_comma = R"({
        "schema_version": "1.0",
        "package_role": "client",
        "version": "1.0.0",
        "min_supported_version": "1.0.0",
        "package_url": "https://updates.school.edu/pkg.msi",
        "byte_length": 100,
        "sha256_hex": "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789",
        "channel": "ltsc-stable",
        "release_timestamp_utc": 1000,
        "min_windows_build": 17763,
    })";
    const auto [res2, err2] = import_update_manifest_json(trailing_comma);
    assert(!res2.has_value());
    assert(err2.error == ManifestVerificationError::invalid_json);

    // 3. Missing comma between fields
    const std::string missing_comma = R"({
        "schema_version": "1.0"
        "package_role": "client"
    })";
    const auto [res3, err3] = import_update_manifest_json(missing_comma);
    assert(!res3.has_value());
    assert(err3.error == ManifestVerificationError::invalid_json);

    // 4. Integer overflow in byte_length
    const std::string int_overflow = R"({
        "schema_version": "1.0",
        "package_role": "client",
        "version": "1.0.0",
        "package_url": "https://updates.school.edu/pkg.msi",
        "byte_length": 9999999999999999999999999999999999999999
    })";
    const auto [res4, err4] = import_update_manifest_json(int_overflow);
    assert(!res4.has_value());
    assert(err4.error == ManifestVerificationError::invalid_json);

    // 5. Missing required field (missing byte_length)
    const std::string missing_required = R"({
        "schema_version": "1.0",
        "package_role": "client",
        "version": "1.0.0",
        "package_url": "https://updates.school.edu/pkg.msi",
        "sha256_hex": "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789",
        "channel": "ltsc-stable",
        "release_timestamp_utc": 1000,
        "min_windows_build": 17763
    })";
    const auto [res5, err5] = import_update_manifest_json(missing_required);
    assert(!res5.has_value());
    assert(err5.error == ManifestVerificationError::missing_required_field);

    // 6. Oversized JSON (> 64 KiB)
    std::string oversized = "{\"padding\":\"";
    oversized.append(70 * 1024, 'x');
    oversized += "\"}";
    const auto [res6, err6] = import_update_manifest_json(oversized);
    assert(!res6.has_value());
    assert(err6.error == ManifestVerificationError::oversized_manifest);
}

int main() {
    std::cout << "[update_manifest_tests] running test suites...\n";

    test_semantic_version_parsing_and_ordering();
    std::cout << "  PASS test_semantic_version_parsing_and_ordering\n";

    test_asymmetric_ecdsa_manifest_signing_and_signature_verification();
    std::cout << "  PASS test_asymmetric_ecdsa_manifest_signing_and_signature_verification\n";

    test_anti_downgrade_and_equal_version_enforcement();
    std::cout << "  PASS test_anti_downgrade_and_equal_version_enforcement\n";

    test_environment_os_and_role_compatibility();
    std::cout << "  PASS test_environment_os_and_role_compatibility\n";

    test_deterministic_canary_rollout_bucketing();
    std::cout << "  PASS test_deterministic_canary_rollout_bucketing\n";

    test_canonical_json_export_and_import();
    std::cout << "  PASS test_canonical_json_export_and_import\n";

    test_fail_closed_json_parser_hardening();
    std::cout << "  PASS test_fail_closed_json_parser_hardening\n";

    std::cout << "[update_manifest_tests] ALL 7 TEST SUITES PASSED.\n";
    return 0;
}
