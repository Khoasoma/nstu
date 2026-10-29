#include "nstu/agent_protocol.hpp"
#include "nstu/exam_bridge.hpp"
#include "nstu/exam_host.hpp"
#include "nstu/exam_profile.hpp"
#include "nstu/exam_sync.hpp"

#include <windows.h>

#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

template <typename Array>
void fill_bytes(Array& bytes, std::uint8_t first) {
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::byte>(first + index);
    }
}

nstu::exam::AnswerEvent make_event() {
    nstu::exam::AnswerEvent event;
    event.package_id = "sample-package";
    fill_bytes(event.package_digest, 0x10);
    fill_bytes(event.client_id, 0x30);
    fill_bytes(event.session_id, 0x50);
    event.candidate_id = "candidate-01";
    event.question_id = "reading-01";
    event.question_revision = 2;
    event.sequence = 1;
    event.client_time_unix_milliseconds = 1'700'000'000'000ULL;
    event.kind = nstu::exam::AnswerKind::upsert;
    event.answer = {std::byte{'B'}};
    return event;
}

void assert_agent_round_trip(nstu::client::AgentMessageType type,
                             const std::vector<std::byte>& payload) {
    const auto wire = nstu::client::encode_agent_message({type, payload});
    assert(!wire.empty());
    const auto decoded = nstu::client::decode_agent_message(wire);
    assert(decoded.has_value());
    assert(decoded->type == type);
    assert(decoded->payload == payload);
}

void assert_path_policy_rejects(
    const std::filesystem::path& data_root,
    const std::filesystem::path& package_root,
    const std::filesystem::path& web_root,
    const std::filesystem::path& user_data_root) {
    std::string error;
    assert(!nstu::client::validate_exam_path_policy(
        data_root, package_root, web_root, user_data_root, &error));
    assert(!error.empty());
}

void exercise_exam_path_policy() {
    std::error_code error;
    const auto temporary = std::filesystem::temp_directory_path(error);
    assert(!error);
    const auto suffix = std::to_string(GetCurrentProcessId()) + "-" +
                        std::to_string(std::chrono::steady_clock::now()
                                           .time_since_epoch()
                                           .count());
    const auto root = temporary / ("nstu-exam-policy-" + suffix);
    std::filesystem::remove_all(root, error);
    assert(!error);

    const auto data_root = root / "data";
    const auto package_area = data_root / "exams";
    const auto package_root = package_area / "sample";
    const auto web_root = package_root / "exam" / "web";
    const auto user_area = data_root / "exam-user-data";
    const auto user_leaf = user_area / "session-01";
    const auto profile_root = root / "local-app-data" / "NSTU" /
                              "exam-webview";
    const auto profile_leaf = profile_root / "session-01";
    assert(std::filesystem::create_directories(web_root, error));
    assert(!error);
    assert(std::filesystem::create_directories(user_area, error));
    assert(!error);
    assert(std::filesystem::create_directories(profile_root, error));
    assert(!error);

    std::string policy_error;
    assert(nstu::client::validate_exam_path_policy(
        data_root, package_root, web_root, user_leaf, &policy_error));
    policy_error.clear();
    // An omitted web root resolves to package_root/exam/web.
    assert(nstu::client::validate_exam_path_policy(
        data_root, package_root, {}, user_leaf, &policy_error));
    policy_error.clear();
    // A not-yet-created session leaf is valid when its parent is controlled.
    assert(nstu::client::validate_exam_path_policy(
        data_root, package_root, web_root, user_area / "new-session",
        &policy_error));
    policy_error.clear();
    assert(nstu::client::validate_exam_user_data_path(
        profile_root, profile_leaf, &policy_error));
    policy_error.clear();
    assert(nstu::client::validate_exam_user_data_path(
        profile_root, profile_root / "new-session", &policy_error));
    std::filesystem::remove_all(profile_root, error);
    assert(!error);
    policy_error.clear();
    // The host creates the profile after validation; a missing policy root is
    // accepted when its canonical parent is still resolvable.
    assert(nstu::client::validate_exam_user_data_path(
        profile_root, profile_root / "first-session", &policy_error));

    const auto sibling_area = data_root / "exams-evil" / "sample";
    assert(std::filesystem::create_directories(sibling_area, error));
    assert(!error);
    assert_path_policy_rejects(data_root, sibling_area, {}, user_leaf);

    const auto outside_package = root / "outside-package";
    assert(std::filesystem::create_directories(outside_package, error));
    assert(!error);
    assert_path_policy_rejects(data_root, outside_package, {}, user_leaf);
    assert_path_policy_rejects(data_root, package_area, {}, user_leaf);
    assert_path_policy_rejects(data_root,
                               package_area / ".." / "outside-package", {},
                               user_leaf);

    const auto outside_web = package_root / ".." / "web-outside";
    assert(std::filesystem::create_directories(outside_web, error));
    assert(!error);
    assert_path_policy_rejects(data_root, package_root, outside_web, user_leaf);

    const auto sibling_user = data_root / "exam-user-data-evil" / "session";
    assert(std::filesystem::create_directories(sibling_user, error));
    assert(!error);
    assert_path_policy_rejects(data_root, package_root, web_root, sibling_user);
    assert_path_policy_rejects(data_root, package_root, web_root, user_area);

    assert_path_policy_rejects(data_root, package_root, web_root,
                               profile_leaf);
    policy_error.clear();
    assert(!nstu::client::validate_exam_user_data_path(
        profile_root, profile_root, &policy_error));
    assert(!policy_error.empty());
    policy_error.clear();
    assert(!nstu::client::validate_exam_user_data_path(
        profile_root, profile_root / ".." / "outside-profile", &policy_error));
    assert(!policy_error.empty());

    assert_path_policy_rejects(data_root, "relative-package", {}, user_leaf);

    std::filesystem::remove_all(root, error);
    assert(!error);
}

void exercise_exam_profile_isolation() {
    const auto root = std::filesystem::path(L"C:\\Users\\student\\AppData\\Local\\NSTU\\exam-webview");
    nstu::security::Sha256Digest package_digest{};
    nstu::security::ClientId client_id{};
    nstu::exam::SessionId session_id{};
    fill_bytes(package_digest, 0x10);
    fill_bytes(client_id, 0x40);
    fill_bytes(session_id, 0x70);

    const auto first = nstu::client::derive_exam_profile_path(
        root, package_digest, client_id, session_id);
    assert(first.has_value());
    assert(first->parent_path() == root);
    const auto leaf = first->filename().string();
    assert(leaf.starts_with("v2-"));
    assert(leaf.size() == 3 + 64 + 1 + 32 + 1 + 32);
    assert(leaf.find_first_not_of("v0123456789abcdef-") == std::string::npos);

    // The same authenticated context intentionally reuses its profile so a
    // reconnect can recover browser-local durable state.
    const auto same = nstu::client::derive_exam_profile_path(
        root, package_digest, client_id, session_id);
    assert(same == first);

    auto other_package = package_digest;
    other_package[0] ^= std::byte{0x01};
    assert(nstu::client::derive_exam_profile_path(
               root, other_package, client_id, session_id) != first);
    auto other_client = client_id;
    other_client[0] ^= std::byte{0x01};
    assert(nstu::client::derive_exam_profile_path(
               root, package_digest, other_client, session_id) != first);
    auto other_session = session_id;
    other_session[0] ^= std::byte{0x01};
    assert(nstu::client::derive_exam_profile_path(
               root, package_digest, client_id, other_session) != first);

    assert(!nstu::client::derive_exam_profile_path(
        std::filesystem::path(L"relative"), package_digest, client_id,
        session_id));
    auto zero_digest = package_digest;
    zero_digest.fill(std::byte{0});
    assert(!nstu::client::derive_exam_profile_path(
        root, zero_digest, client_id, session_id));
}

void exercise_exam_package_inspection() {
    std::error_code error;
    const auto temporary = std::filesystem::temp_directory_path(error);
    assert(!error);
    const auto suffix = std::to_string(GetCurrentProcessId()) + "-" +
                        std::to_string(std::chrono::steady_clock::now()
                                           .time_since_epoch()
                                           .count());
    const auto root = temporary / ("nstu-exam-package-" + suffix);
    const auto package = root / "sample";
    const auto web = package / "exam" / "web";
    assert(std::filesystem::create_directories(web, error));
    assert(!error);

    const std::string manifest = R"json({
  "id": "inspection-sample",
  "title": "Inspection sample",
  "durationSeconds": 600,
  "questions": [{
    "id": "q1",
    "type": "short_answer",
    "prompt": "Answer"
  }]
})json";
    {
        std::ofstream manifest_file(package / "manifest.json",
                                    std::ios::binary);
        assert(manifest_file);
        manifest_file << manifest;
    }
    {
        std::ofstream page_file(web / "index.html", std::ios::binary);
        assert(page_file);
        page_file << "<!doctype html><title>sample</title>";
    }
    {
        std::ofstream asset_file(web / "app.js", std::ios::binary);
        assert(asset_file);
        asset_file << "document.body.dataset.ready='1';";
    }

    nstu::client::ExamPackageReport report;
    std::string inspection_error;
    assert(nstu::client::inspect_exam_package(
        package, web, {}, false, report, &inspection_error));
    assert(report.package_root == std::filesystem::weakly_canonical(package));
    assert(report.web_root == std::filesystem::weakly_canonical(web));
    assert(report.manifest_json == manifest);
    assert(report.digest_hex.size() == 64);
    const auto pinned_digest = report.digest_hex;

    nstu::client::ExamPackageReport pinned;
    inspection_error.clear();
    assert(nstu::client::inspect_exam_package(
        package, web, pinned_digest, true, pinned, &inspection_error));
    assert(pinned.digest_hex == pinned_digest);

    {
        std::ofstream page_file(web / "index.html", std::ios::binary |
                                             std::ios::trunc);
        assert(page_file);
        page_file << "<!doctype html><title>tampered</title>";
    }
    nstu::client::ExamPackageReport tampered;
    inspection_error.clear();
    assert(!nstu::client::inspect_exam_package(
        package, web, pinned_digest, true, tampered, &inspection_error));
    assert(!inspection_error.empty());

    // The package root itself must never become the WebView web root: that
    // would expose manifest.json and non-web package data through the mapped
    // origin. The host requires a child directory such as exam/web.
    inspection_error.clear();
    assert(!nstu::client::inspect_exam_package(
        package, package, {}, false, tampered, &inspection_error));
    assert(!inspection_error.empty());

    std::filesystem::remove_all(root, error);
    assert(!error);
}

void exercise_exam_manifest_asset_validation() {
    std::error_code error;
    const auto temporary = std::filesystem::temp_directory_path(error);
    assert(!error);
    const auto suffix = std::to_string(GetCurrentProcessId()) + "-" +
                        std::to_string(std::chrono::steady_clock::now()
                                           .time_since_epoch()
                                           .count());
    const auto root = temporary / ("nstu-exam-assets-" + suffix);
    const auto package = root / "sample";
    const auto web = package / "exam" / "web";
    const auto media = package / "media";
    const auto documents = package / "documents";
    assert(std::filesystem::create_directories(web, error));
    assert(!error);
    assert(std::filesystem::create_directories(media, error));
    assert(!error);
    assert(std::filesystem::create_directories(documents, error));
    assert(!error);

    {
        std::ofstream page_file(web / "index.html", std::ios::binary);
        assert(page_file);
        page_file << "<!doctype html><title>assets</title>";
    }
    {
        std::ofstream audio_file(media / "listening.mp3", std::ios::binary);
        assert(audio_file);
        audio_file << "audio";
    }
    {
        std::ofstream document_file(documents / "reference.pdf",
                                    std::ios::binary);
        assert(document_file);
        document_file << "%PDF-1.4\n";
    }

    const auto write_manifest = [&](std::string_view audio,
                                    std::string_view document) {
        std::ofstream manifest_file(package / "manifest.json",
                                    std::ios::binary | std::ios::trunc);
        assert(manifest_file);
        manifest_file << R"json({
  "id": "inspection-assets",
  "title": "Inspection assets",
  "durationSeconds": 600,
  "questions": [{
    "id": "q1",
    "type": "listening",
    "prompt": "Listen",
    "audio": ")json"
                      << audio << R"json(",
    "options": ["A", "B"]
  }],
  "documents": [{"url": ")json"
                      << document << R"json("}]
})json";
    };

    write_manifest("media/listening.mp3", "documents/reference.pdf");
    nstu::client::ExamPackageReport report;
    std::string inspection_error;
    assert(nstu::client::inspect_exam_package(
        package, web, {}, false, report, &inspection_error));

    write_manifest("media/missing.mp3", "documents/reference.pdf");
    inspection_error.clear();
    assert(!nstu::client::inspect_exam_package(
        package, web, {}, false, report, &inspection_error));
    assert(inspection_error.find("missing asset") != std::string::npos);

    // Deployment metadata must never become browser-readable merely because a
    // package manifest names it as an ordinary media/document path.
    write_manifest("MANIFEST.JSON", "documents/reference.pdf");
    inspection_error.clear();
    assert(!nstu::client::inspect_exam_package(
        package, web, {}, false, report, &inspection_error));
    assert(!inspection_error.empty());

    write_manifest("media/listening.mp3", "MANIFEST.P7S");
    inspection_error.clear();
    assert(!nstu::client::inspect_exam_package(
        package, web, {}, false, report, &inspection_error));
    assert(!inspection_error.empty());

    std::filesystem::remove_all(root, error);
    assert(!error);
}

void exercise_exam_host_telemetry() {
    using namespace nstu;
    using namespace nstu::exam;
    using namespace nstu::client;

    const std::set<std::string> allowed_questions = {"q1", "reading-01", "math-02"};

    // 1. Valid browser sample parsing:
    // Browser claims packageId, candidateId, source="native", collectorId=999.
    // Host overrides source to browser, binds native collector ID (42), and ignores browser identity.
    const std::string valid_json = R"json({
        "type": "exam_telemetry_samples",
        "packageId": "spoofed-package",
        "candidateId": "spoofed-candidate",
        "samples": [{
            "kind": "question_dwell",
            "source": "native",
            "collectorId": 42,
            "questionId": "reading-01",
            "questionRevision": 2,
            "monotonicElapsedMilliseconds": 12500,
            "durationMilliseconds": 3500,
            "clientTimeUnixMilliseconds": 1700000001000,
            "numericValue": 0,
            "count": 1,
            "reason": "navigation_button"
        }]
    })json";

    std::vector<TelemetrySample> samples;
    std::string error;
    assert(parse_browser_telemetry_samples(valid_json, 42, allowed_questions, samples, &error));
    assert(samples.size() == 1);
    const auto s = samples.front();
    assert(s.kind == TelemetrySampleKind::question_dwell);
    assert(s.source == TelemetrySource::browser); // overridden
    assert(s.collector_id == 42);                  // native bound
    assert(s.question_id == "reading-01");
    assert(s.question_revision == 2);
    assert(s.monotonic_elapsed_milliseconds == 12500);
    assert(s.duration_milliseconds == 3500);
    assert(s.client_time_unix_milliseconds == 1700000001000ULL);
    assert(s.count == 1);
    assert(s.reason == TelemetryReason::navigation_button);

    for (const auto& [field, value] : std::vector<std::pair<std::string, std::string>>{
            {"collectorId", "\"41\""}, {"durationMilliseconds", "-1"},
            {"durationMilliseconds", "true"}, {"durationMilliseconds", "\"3500\""},
            {"questionRevision", "4294967298"}, {"durationMilliseconds", "4294970796"},
            {"numericValue", "4294967296"}, {"count", "4294967297"},
            {"monotonicElapsedMilliseconds", "null"}, {"questionId", "42"}, {"reason", "true"}}) {
        auto malformed = valid_json;
        const auto begin = malformed.find("\"" + field + "\":") + field.size() + 3;
        assert(begin < malformed.size());
        const auto end = malformed.find_first_of(",\n", begin);
        malformed.replace(begin, end - begin, value);
        error.clear();
        assert(!parse_browser_telemetry_samples(malformed, 42, allowed_questions, samples, &error));
        assert(!error.empty());
    }

    // 2. Stale collector ID rejection:
    // Browser explicitly provides a non-zero collectorId that does not match native collector ID.
    const std::string stale_collector_json = R"json({
        "type": "exam_telemetry_samples",
        "samples": [{
            "kind": "focus_changed",
            "collectorId": 41,
            "monotonicElapsedMilliseconds": 13000,
            "durationMilliseconds": 0,
            "clientTimeUnixMilliseconds": 1700000002000,
            "numericValue": 0,
            "count": 0,
            "reason": "focus_blur"
        }]
    })json";
    samples.clear();
    error.clear();
    assert(!parse_browser_telemetry_samples(stale_collector_json, 42, allowed_questions, samples, &error));
    assert(!error.empty());

    // 3. Hostile JSON and schema violations:
    // (a) Oversized payload (> 32 KiB)
    std::string oversized_json = "{\"type\":\"exam_telemetry_samples\",\"samples\":[";
    for (int i = 0; i < 300; ++i) {
        oversized_json += R"json({"kind":"question_dwell","questionId":"reading-01","questionRevision":1,"monotonicElapsedMilliseconds":100,"durationMilliseconds":100,"clientTimeUnixMilliseconds":1700000000000,"numericValue":0,"count":1,"reason":"navigation_button"},)json";
    }
    oversized_json.pop_back(); // trailing comma
    oversized_json += "]}";
    assert(oversized_json.size() > 32 * 1024);
    samples.clear();
    assert(!parse_browser_telemetry_samples(oversized_json, 42, allowed_questions, samples, &error));

    // (b) Depth limit exceeded
    const std::string deep_json = "{\"type\":\"exam_telemetry_samples\",\"samples\":[[[{\"kind\":\"focus_changed\"}]]]}";
    samples.clear();
    assert(!parse_browser_telemetry_samples(deep_json, 42, allowed_questions, samples, &error));

    // (c) Samples array count limit: 0 samples
    const std::string empty_samples_json = R"json({"type":"exam_telemetry_samples","samples":[]})json";
    samples.clear();
    assert(!parse_browser_telemetry_samples(empty_samples_json, 42, allowed_questions, samples, &error));

    // (d) Samples array count limit: > 32 samples
    std::string too_many_samples_json = "{\"type\":\"exam_telemetry_samples\",\"samples\":[";
    for (int i = 0; i < 33; ++i) {
        too_many_samples_json += R"json({"kind":"focus_changed","monotonicElapsedMilliseconds":100,"durationMilliseconds":0,"clientTimeUnixMilliseconds":1700000000000,"numericValue":0,"count":0,"reason":"focus_gain"},)json";
    }
    too_many_samples_json.pop_back();
    too_many_samples_json += "]}";
    samples.clear();
    assert(!parse_browser_telemetry_samples(too_many_samples_json, 42, allowed_questions, samples, &error));

    // (e) Disallowed forbidden fields in sample (privacy violation / data leakage)
    const std::string forbidden_field_json = R"json({
        "type": "exam_telemetry_samples",
        "samples": [{
            "kind": "question_dwell",
            "questionId": "reading-01",
            "questionRevision": 1,
            "monotonicElapsedMilliseconds": 1000,
            "durationMilliseconds": 500,
            "clientTimeUnixMilliseconds": 1700000000000,
            "numericValue": 0,
            "count": 1,
            "reason": "navigation_button",
            "answerText": "Option B selected by candidate"
        }]
    })json";
    samples.clear();
    assert(!parse_browser_telemetry_samples(forbidden_field_json, 42, allowed_questions, samples, &error));

    // (f) Unknown sample kind
    const std::string unknown_kind_json = R"json({
        "type": "exam_telemetry_samples",
        "samples": [{
            "kind": "arbitrary_custom_kind",
            "monotonicElapsedMilliseconds": 1000,
            "durationMilliseconds": 0,
            "clientTimeUnixMilliseconds": 1700000000000,
            "numericValue": 0,
            "count": 0,
            "reason": "none"
        }]
    })json";
    samples.clear();
    assert(!parse_browser_telemetry_samples(unknown_kind_json, 42, allowed_questions, samples, &error));

    // (g) Unknown / invalid reason
    const std::string unknown_reason_json = R"json({
        "type": "exam_telemetry_samples",
        "samples": [{
            "kind": "focus_changed",
            "monotonicElapsedMilliseconds": 1000,
            "durationMilliseconds": 0,
            "clientTimeUnixMilliseconds": 1700000000000,
            "numericValue": 0,
            "count": 0,
            "reason": "unauthorized_reason"
        }]
    })json";
    samples.clear();
    assert(!parse_browser_telemetry_samples(unknown_reason_json, 42, allowed_questions, samples, &error));

    // (h) Nonempty question_id with revision == 0
    const std::string zero_rev_json = R"json({
        "type": "exam_telemetry_samples",
        "samples": [{
            "kind": "question_first_view",
            "questionId": "reading-01",
            "questionRevision": 0,
            "monotonicElapsedMilliseconds": 1000,
            "durationMilliseconds": 0,
            "clientTimeUnixMilliseconds": 1700000000000,
            "numericValue": 0,
            "count": 0,
            "reason": "navigation_initial"
        }]
    })json";
    samples.clear();
    assert(!parse_browser_telemetry_samples(zero_rev_json, 42, allowed_questions, samples, &error));

    // (i) Empty question_id with revision > 0
    const std::string empty_qid_nonzero_rev_json = R"json({
        "type": "exam_telemetry_samples",
        "samples": [{
            "kind": "focus_changed",
            "questionId": "",
            "questionRevision": 1,
            "monotonicElapsedMilliseconds": 1000,
            "durationMilliseconds": 0,
            "clientTimeUnixMilliseconds": 1700000000000,
            "numericValue": 0,
            "count": 0,
            "reason": "focus_blur"
        }]
    })json";
    samples.clear();
    assert(!parse_browser_telemetry_samples(empty_qid_nonzero_rev_json, 42, allowed_questions, samples, &error));

    // (j) Question ID not in allowed questions
    const std::string disallowed_qid_json = R"json({
        "type": "exam_telemetry_samples",
        "samples": [{
            "kind": "question_first_view",
            "questionId": "unauthorized-question-99",
            "questionRevision": 1,
            "monotonicElapsedMilliseconds": 1000,
            "durationMilliseconds": 0,
            "clientTimeUnixMilliseconds": 1700000000000,
            "numericValue": 0,
            "count": 0,
            "reason": "navigation_initial"
        }]
    })json";
    samples.clear();
    assert(!parse_browser_telemetry_samples(disallowed_qid_json, 42, allowed_questions, samples, &error));

    // (k) Omitted collectorId: succeeds and binds native collector ID
    const std::string omitted_collector_json = R"json({
        "type": "exam_telemetry_samples",
        "samples": [{
            "kind": "focus_changed",
            "monotonicElapsedMilliseconds": 1000,
            "durationMilliseconds": 0,
            "clientTimeUnixMilliseconds": 1700000000000,
            "numericValue": 0,
            "count": 0,
            "reason": "focus_gain"
        }]
    })json";
    samples.clear();
    assert(parse_browser_telemetry_samples(omitted_collector_json, 42, allowed_questions, samples, &error));
    assert(samples.size() == 1);
    assert(samples.front().collector_id == 42);

    // 4. Blocked shortcut atomic recording & coalescing drain:
    // Simulated hook: no allocations or locks
    record_blocked_shortcut_for_testing(3);
    record_blocked_shortcut_for_testing(2);
    // Drain coalesces into 1 sample with count 5
    const auto drained = drain_blocked_shortcuts(42, 5000, 1700000005000ULL);
    assert(drained.has_value());
    assert(drained->kind == TelemetrySampleKind::blocked_shortcut_summary);
    assert(drained->source == TelemetrySource::native);
    assert(drained->collector_id == 42);
    assert(drained->count == 5);
    assert(drained->reason == TelemetryReason::shortcut_system);
    assert(drained->monotonic_elapsed_milliseconds == 5000);
    assert(drained->client_time_unix_milliseconds == 1700000005000ULL);
    // Subsequent drain is empty
    assert(!drain_blocked_shortcuts(42, 5000, 1700000005000ULL).has_value());

    // 5. IPC payload serialization for AgentMessageType::exam_telemetry_samples:
    std::vector<TelemetrySample> test_samples;
    test_samples.push_back(*drained);
    test_samples.push_back(s);
    const auto ipc_payload = encode_exam_telemetry_samples_payload(test_samples);
    assert(!ipc_payload.empty());
    const auto decoded_samples = decode_exam_telemetry_samples_payload(ipc_payload);
    assert(decoded_samples.has_value());
    assert(decoded_samples->size() == 2);
    assert((*decoded_samples)[0].kind == TelemetrySampleKind::blocked_shortcut_summary);
    assert((*decoded_samples)[1].kind == TelemetrySampleKind::question_dwell);
    assert_agent_round_trip(AgentMessageType::exam_telemetry_samples, ipc_payload);

    // Corrupted payload rejection:
    auto corrupt_ipc = ipc_payload;
    corrupt_ipc.pop_back();
    assert(!decode_exam_telemetry_samples_payload(corrupt_ipc).has_value());

    // 6. Matching enable & idempotency in ExamHost:
    ExamHost host;
    assert(!host.telemetry_enabled());
    assert(host.telemetry_collector_id() == 0);

    // When stopped or not started with matching context, enable rejects:
    TelemetryEnable enable;
    enable.package_id = "sample-package";
    enable.candidate_id = "candidate-01";
    fill_bytes(enable.package_digest, 0x10);
    fill_bytes(enable.client_id, 0x30);
    fill_bytes(enable.session_id, 0x50);
    enable.server_time_unix_milliseconds = 1700000000000ULL;

    assert(!host.apply_telemetry_enable(enable));
    assert(!host.telemetry_enabled());
}

void exercise_started_host_telemetry_state() {
    using namespace nstu;
    using namespace nstu::client;
    const auto event = make_event();
    exam::StateRequest context;
    context.package_id = event.package_id;
    context.package_digest = event.package_digest;
    context.client_id = event.client_id;
    context.session_id = event.session_id;
    context.candidate_id = event.candidate_id;
    exam::TelemetryEnable enable;
    enable.package_id = context.package_id;
    enable.package_digest = context.package_digest;
    enable.client_id = context.client_id;
    enable.session_id = context.session_id;
    enable.candidate_id = context.candidate_id;
    enable.server_time_unix_milliseconds = 1700000000000ULL;

    // This is the production state owned by ExamHost. No browser/window or
    // platform response is substituted; successful WebView2 start is separate.
    ExamHostTelemetryState telemetry;
    assert(!telemetry.apply_enable(enable));
    telemetry.record_blocked_shortcuts(7);
    assert(!telemetry.drain_blocked_shortcuts(100, 1000).has_value());
    assert(telemetry.start(context, 1));
    assert(!telemetry.enabled());
    assert(telemetry.collector_id() == 1);
    telemetry.record_blocked_shortcuts(11);

    for (unsigned field = 0; field < 5; ++field) {
        auto wrong = enable;
        switch (field) {
        case 0: wrong.package_id = "wrong-package"; break;
        case 1: wrong.package_digest[0] ^= std::byte{1}; break;
        case 2: wrong.client_id[0] ^= std::byte{1}; break;
        case 3: wrong.session_id[0] ^= std::byte{1}; break;
        case 4: wrong.candidate_id = "wrong-candidate"; break;
        }
        assert(!telemetry.apply_enable(wrong));
        assert(!telemetry.enabled());
    }
    assert(telemetry.apply_enable(enable));
    assert(telemetry.enabled());
    assert(!telemetry.drain_blocked_shortcuts(100, 1000).has_value());
    telemetry.record_blocked_shortcuts(3);
    auto duplicate = enable;
    duplicate.server_time_unix_milliseconds += 1000;
    assert(telemetry.apply_enable(duplicate));
    assert(telemetry.enabled());
    assert(telemetry.collector_id() == 1);
    assert(telemetry.active_enable()->server_time_unix_milliseconds ==
           enable.server_time_unix_milliseconds);
    telemetry.record_blocked_shortcuts(2);
    const auto sample = telemetry.drain_blocked_shortcuts(100, 1000);
    assert(sample.has_value());
    assert(sample->count == 5);
    assert(sample->collector_id == 1);
    assert(sample->monotonic_elapsed_milliseconds == 100);
    assert(sample->client_time_unix_milliseconds == 1000);
    assert(!telemetry.drain_blocked_shortcuts(100, 1000).has_value());

    telemetry.record_blocked_shortcuts(9);
    telemetry.advance_collector(2);
    assert(telemetry.collector_id() == 2);
    assert(telemetry.enabled());
    assert(!telemetry.drain_blocked_shortcuts(100, 1000).has_value());
    telemetry.record_blocked_shortcuts(13);
    telemetry.stop();
    assert(!telemetry.enabled());
    assert(telemetry.collector_id() == 0);
    assert(!telemetry.active_enable().has_value());
    assert(!telemetry.apply_enable(enable));
    assert(!telemetry.drain_blocked_shortcuts(100, 1000).has_value());
    telemetry.record_blocked_shortcuts(17);

    auto second_context = context;
    second_context.session_id[0] ^= std::byte{2};
    auto second_enable = enable;
    second_enable.session_id = second_context.session_id;
    assert(telemetry.start(second_context, 1));
    assert(!telemetry.enabled());
    assert(!telemetry.apply_enable(enable));
    assert(telemetry.apply_enable(second_enable));
    assert(!telemetry.drain_blocked_shortcuts(100, 1000).has_value());
    telemetry.record_blocked_shortcuts(19);
    assert(telemetry.start(second_context, 1));
    assert(!telemetry.enabled());
    assert(telemetry.collector_id() == 1);
    assert(telemetry.apply_enable(second_enable));
    assert(!telemetry.drain_blocked_shortcuts(100, 1000).has_value());
    assert(!telemetry.start(exam::StateRequest{}));
    assert(!telemetry.enabled());
    assert(telemetry.collector_id() == 0);
    assert(!telemetry.apply_enable(second_enable));
}

} // namespace

void test_collector_identity_is_distinct_across_restart_and_hosts() {
    const auto event = make_event();
    nstu::exam::StateRequest context;
    context.package_id = event.package_id;
    context.package_digest = event.package_digest;
    context.client_id = event.client_id;
    context.session_id = event.session_id;
    context.candidate_id = event.candidate_id;
    nstu::client::ExamHostTelemetryState first;
    assert(first.start(context));
    const auto started_id = first.collector_id();
    assert(started_id > 0 && started_id <= ((1ULL << 53) - 1));
    first.advance_collector();
    const auto document_id = first.collector_id();
    assert(document_id != started_id);
    first.stop();
    assert(first.start(context));
    const auto restarted_id = first.collector_id();
    assert(restarted_id != started_id && restarted_id != document_id);
    nstu::client::ExamHostTelemetryState second;
    assert(second.start(context));
    assert(second.collector_id() != restarted_id);
    assert(second.collector_id() > 0 && second.collector_id() <= ((1ULL << 53) - 1));

    nstu::exam::TelemetryEnable enable;
    enable.package_id = context.package_id;
    enable.package_digest = context.package_digest;
    enable.client_id = context.client_id;
    enable.session_id = context.session_id;
    enable.candidate_id = context.candidate_id;
    for (const auto unavailable : {std::uint64_t{0}, (std::uint64_t{1} << 53)}) {
        assert(second.start(context, unavailable));
        assert(second.collector_id() == 0);
        assert(!second.apply_enable(enable));
        assert(!second.enabled());
        second.record_blocked_shortcuts(9);
        assert(!second.drain_blocked_shortcuts(100, 1000));
    }
    assert(second.start(context, nstu::client::kMaximumExamCollectorId));
    assert(second.apply_enable(enable));
    second.advance_collector(0);
    assert(!second.enabled() && second.collector_id() == 0);

    const auto stale_json = std::string("{\"type\":\"exam_telemetry_samples\",\"samples\":[{") +
        "\"kind\":\"question_dwell\",\"collectorId\":" + std::to_string(document_id) +
        ",\"questionId\":\"reading-01\",\"questionRevision\":1,\"durationMilliseconds\":100," +
        "\"clientTimeUnixMilliseconds\":1700000000000}]}";
    std::vector<nstu::exam::TelemetrySample> samples;
    std::string error;
    assert(!nstu::client::parse_browser_telemetry_samples(stale_json, restarted_id, {"reading-01"}, samples, &error));
    assert(!error.empty());
}


void exercise_browser_clock_safe_boundary() {
    using namespace nstu::client;
    std::vector<nstu::exam::TelemetrySample> samples;
    std::string error;
    const std::string unsafe = R"json({"type":"exam_telemetry_samples","samples":[{
        "kind":"collector_started","collectorId":42,
        "monotonicElapsedMilliseconds":9007199254740992,
        "clientTimeUnixMilliseconds":1700000000000,"reason":"collector_initial"
    }]})json";
    assert(!parse_browser_telemetry_samples(unsafe, 42, {}, samples, &error));
    auto maximum = unsafe;
    maximum.replace(maximum.find("9007199254740992"), 16, "9007199254740991");
    assert(parse_browser_telemetry_samples(maximum, 42, {}, samples, &error));
    assert(samples[0].monotonic_elapsed_milliseconds == kMaximumExamCollectorId);
    maximum.replace(maximum.find("\"collectorId\":42"), 16, "\"collectorId\":9007199254740992");
    assert(!parse_browser_telemetry_samples(maximum, kMaximumExamCollectorId + 1, {}, samples, &error));
}


void exercise_native_capture_clock() {
    using namespace nstu;
    using namespace nstu::client;
    using Clock = ExamHostTelemetryState::Clock;
    const Clock::time_point origin{};
    const auto at = [origin](std::uint64_t ms) { return origin + std::chrono::milliseconds(ms); };
    const auto event = make_event();
    exam::StateRequest context;
    context.package_id = event.package_id; context.package_digest = event.package_digest;
    context.client_id = event.client_id; context.session_id = event.session_id;
    context.candidate_id = event.candidate_id;
    exam::TelemetryEnable enable;
    enable.package_id = context.package_id; enable.package_digest = context.package_digest;
    enable.client_id = context.client_id; enable.session_id = context.session_id;
    enable.candidate_id = context.candidate_id;
    enable.server_time_unix_milliseconds = 1700000000000ULL;
    ExamHostTelemetryState clock;
    assert(clock.start(context, 42, origin));
    assert(clock.apply_enable(enable));
    unsigned posts = 0;
    std::string posted;
    const auto post = [&](std::string_view json) { ++posts; posted = json; return true; };
    const auto packet = [](std::string id, std::string kind, std::uint64_t elapsed, unsigned numeric = 999) {
        return "{\"type\":\"exam_telemetry_samples\",\"samples\":[{" + id +
            "\"kind\":\"" + kind + "\",\"monotonicElapsedMilliseconds\":" + std::to_string(elapsed) +
            ",\"clientTimeUnixMilliseconds\":1700000000000,\"numericValue\":" + std::to_string(numeric) +
            ",\"reason\":\"" + (kind == "collector_started" ? "collector_initial" : "none") + "\"}]}";
    };
    const std::string id = "\"collectorId\":42,";
    std::vector<exam::TelemetrySample> samples;
    std::string error;
    // Failed local publication never authorizes a browser clock.
    assert(!clock.post_browser_enable([](std::string_view) { return false; }, at(30)));
    assert(!clock.forward_browser_samples(packet(id, "collector_started", 30), {}, at(35), samples, &error));
    assert(!clock.forward_browser_samples(packet(id, "focus_changed", 35), {}, at(35), samples, &error));
    assert(clock.post_browser_enable(post, at(40)));
    assert(posted.find("\"collectorId\":42") != std::string::npos);
    assert(posted.find("\"collectorElapsedMilliseconds\":40") != std::string::npos);
    assert(!clock.forward_browser_samples(packet(id, "focus_changed", 40), {}, at(45), samples, &error));
    assert(!clock.forward_browser_samples(packet("\"collectorId\":41,", "collector_started", 40), {}, at(46), samples, &error));
    assert(!clock.forward_browser_samples(packet(id, "collector_started", 39), {}, at(47), samples, &error));
    assert(!clock.forward_browser_samples(packet("", "collector_started", 40), {}, at(48), samples, &error));
    assert(!clock.forward_browser_samples(packet("\"collectorId\":0,", "collector_started", 40), {}, at(49), samples, &error));
    assert(clock.forward_browser_samples(packet(id, "collector_started", 40), {}, at(65), samples, &error));
    assert(samples.size() == 1 && samples[0].monotonic_elapsed_milliseconds == 40);
    assert(samples[0].numeric_value == 27); // native receipt interval + signed quantization slack
    assert(clock.apply_enable(enable));
    assert(clock.post_browser_enable(post, at(70)) && posts == 1);
    assert(!clock.forward_browser_samples(packet(id, "collector_started", 40), {}, at(75), samples, &error));
    assert(!clock.forward_browser_samples(packet("", "focus_changed", 50), {}, at(75), samples, &error));
    assert(!clock.forward_browser_samples(packet("\"collectorId\":0,", "focus_changed", 50), {}, at(75), samples, &error));
    assert(clock.forward_browser_samples(packet(id, "focus_changed", 50), {}, at(75), samples, &error));
    assert(!clock.forward_browser_samples(packet(id, "focus_changed", 49), {}, at(76), samples, &error));
    assert(!clock.forward_browser_samples(packet(id, "focus_changed", 60), {}, at(77), samples, &error));

    // Reload at a repeated zero anchor still requires the new explicitly supplied ID.
    clock.advance_collector(43, origin);
    assert(clock.post_browser_enable(post, at(0)));
    for (const std::string raw_id : {"", "\"collectorId\":0,", "\"collectorId\":42,", "\"collectorId\":9007199254740992,"}) {
        assert(!clock.forward_browser_samples(packet(raw_id, "collector_started", 0), {}, at(0), samples, &error));
    }
    const std::string new_id = "\"collectorId\":43,";
    assert(clock.forward_browser_samples(packet(new_id, "collector_started", 0), {}, at(0), samples, &error));
    assert(samples[0].numeric_value == 2);
    const auto radius = samples[0].numeric_value;
    // At initial calibration, estimate 1 with radius 2 includes native floored elapsed 0.
    assert(clock.forward_browser_samples(packet(new_id, "focus_changed", 1), {}, origin + std::chrono::microseconds(600), samples, &error));
    assert(samples[0].monotonic_elapsed_milliseconds == 1);
    const auto estimate = samples[0].monotonic_elapsed_milliseconds;
    const auto lower = estimate > radius ? estimate - radius : 0;
    const auto upper = estimate + radius;
    assert(lower == 0 && upper == 3);
    const auto floored_native = clock.native_elapsed(origin + std::chrono::microseconds(600));
    assert(floored_native && lower <= *floored_native && *floored_native <= upper);

    clock.advance_collector(44, origin);
    assert(clock.post_browser_enable(post, at(40)));
    assert(!clock.forward_browser_samples(packet("\"collectorId\":44,", "collector_started", 40), {}, at(39), samples, &error));
    assert(!clock.forward_browser_samples(packet("\"collectorId\":44,", "collector_started", 40), {}, at(65), samples, &error));
    clock.advance_collector(45, origin);
    assert(clock.post_browser_enable(post, at(0)));
    const auto largest_radius = static_cast<std::uint64_t>(UINT32_MAX) - 2;
    assert(clock.forward_browser_samples(packet("\"collectorId\":45,", "collector_started", 0), {}, at(largest_radius), samples, &error));
    assert(samples[0].numeric_value == UINT32_MAX);
    clock.advance_collector(46, origin);
    assert(clock.post_browser_enable(post, at(0)));
    assert(!clock.forward_browser_samples(packet("\"collectorId\":46,", "collector_started", 0), {}, at(largest_radius + 1), samples, &error));
    // Signed steady-clock subtraction extremes refuse overflow rather than clamp.
    clock.advance_collector(47, Clock::time_point::min());
    assert(!clock.native_elapsed(Clock::time_point::max()));
    clock.advance_collector(48, origin);
    assert(!clock.post_browser_enable([](std::string_view) -> bool { throw 1; }, at(0)));
    assert(!clock.forward_browser_samples(packet("\"collectorId\":48,", "collector_started", 0), {}, at(0), samples, &error));
    assert(clock.post_browser_enable(post, at(0)));
    assert(clock.forward_browser_samples(packet("\"collectorId\":48,", "collector_started", 0), {}, at(65), samples, &error));
    assert(!clock.forward_browser_samples(packet("\"collectorId\":48,", "focus_changed", 1), {}, at(64), samples, &error));
    assert(!clock.forward_browser_samples(packet("\"collectorId\":48,", "focus_changed", 1), {}, at(66), samples, &error));
    clock.stop();
    assert(!clock.post_browser_enable(post, at(0)));
    assert(!clock.forward_browser_samples(packet(new_id, "collector_started", 0), {}, at(0), samples, &error));
    assert(clock.start(context, 0, origin)); // unavailable telemetry identity cannot block the exam context
    assert(!clock.apply_enable(enable));
    assert(make_exam_telemetry_enable_json(enable, kMaximumExamCollectorId, kMaximumExamCollectorId).find(
        "\"collectorElapsedMilliseconds\":9007199254740991") != std::string::npos);
    assert(make_exam_telemetry_enable_json(enable, 0, 0).empty());
    assert(make_exam_telemetry_enable_json(enable, 42, kMaximumExamCollectorId + 1).empty());
    assert(make_exam_telemetry_enable_json(enable, kMaximumExamCollectorId + 1, 0).empty());
}


void exercise_native_startup_full_window_recovery() {
    using namespace nstu;
    using namespace nstu::client;
    using Clock = ExamHostTelemetryState::Clock;
    const Clock::time_point origin{};
    const auto at = [origin](int ms) { return origin + std::chrono::milliseconds(ms); };
    const auto event = make_event();
    exam::StateRequest context;
    context.package_id = event.package_id; context.package_digest = event.package_digest;
    context.client_id = event.client_id; context.session_id = event.session_id;
    context.candidate_id = event.candidate_id;
    exam::TelemetryEnable enable;
    enable.package_id = context.package_id; enable.package_digest = context.package_digest;
    enable.client_id = context.client_id; enable.session_id = context.session_id;
    enable.candidate_id = context.candidate_id; enable.server_time_unix_milliseconds = 1700000000000ULL;
    ExamHostTelemetryState telemetry;
    assert(telemetry.start(context, 42, origin));
    assert(telemetry.apply_enable(enable));
    std::vector<std::string> posted;
    const auto post = [&](std::string_view json) { posted.emplace_back(json); return true; };
    const auto packet = [](unsigned id, std::string_view kind, unsigned elapsed, unsigned count = 1) {
        const std::string item = "{\"kind\":\"" + std::string(kind) + "\",\"collectorId\":" +
            std::to_string(id) + ",\"monotonicElapsedMilliseconds\":" + std::to_string(elapsed) +
            ",\"clientTimeUnixMilliseconds\":1700000000000,\"reason\":\"none\"}";
        std::string json = "{\"type\":\"exam_telemetry_samples\",\"samples\":[";
        for (unsigned i = 0; i < count; ++i) { if (i != 0) json += ','; json += item; }
        return json + "]}";
    };
    std::vector<exam::TelemetrySample> samples;
    std::string error;
    assert(telemetry.post_browser_enable(post, at(40)));
    assert(telemetry.forward_browser_samples(packet(42, "collector_started", 40), {}, at(65), samples, &error));
    for (unsigned batch = 0; batch < 7; ++batch) {
        assert(telemetry.forward_browser_samples(packet(42, "focus_changed", 70, 32), {}, at(100), samples, &error));
    }
    assert(telemetry.forward_browser_samples(packet(42, "focus_changed", 70, 31), {}, at(100), samples, &error));
    // Exactly 256 admitted samples fill the existing window, including startup.
    assert(!telemetry.forward_browser_samples(packet(42, "focus_changed", 70), {}, at(100), samples, &error));
    telemetry.advance_collector(43, at(200));
    assert(telemetry.post_browser_enable(post, at(200)));
    assert(!telemetry.forward_browser_samples(packet(43, "collector_started", 0), {}, at(201), samples, &error));
    assert(!telemetry.forward_browser_samples(packet(43, "focus_changed", 1), {}, at(201), samples, &error));
    assert(!telemetry.maintain_browser_startup(post, at(1199)) && posted.size() == 2);
    assert(telemetry.maintain_browser_startup(post, at(1200)) && posted.size() == 3);
    assert(posted.back().find("\"collectorStartupRetry\":true") != std::string::npos);
    assert(posted.back().find("\"collectorId\":43") != std::string::npos);
    assert(posted.back().find("\"collectorElapsedMilliseconds\":0") != std::string::npos);
    assert(!telemetry.maintain_browser_startup(post, at(1200)) && posted.size() == 3);
    assert(!telemetry.forward_browser_samples(packet(43, "focus_changed", 1), {}, at(1200), samples, &error));
    assert(!telemetry.forward_browser_samples(packet(42, "collector_started", 0), {}, at(1200), samples, &error));
    assert(!telemetry.forward_browser_samples(packet(43, "collector_started", 1), {}, at(1200), samples, &error));
    assert(telemetry.forward_browser_samples(packet(43, "collector_started", 0), {}, at(1201), samples, &error));
    assert(samples[0].numeric_value == 1003); // Same original anchor, later receipt.
    for (unsigned batch = 0; batch < 7; ++batch) {
        assert(telemetry.forward_browser_samples(packet(43, "focus_changed", 2, 32), {}, at(1300), samples, &error));
    }
    assert(telemetry.forward_browser_samples(packet(43, "focus_changed", 2, 31), {}, at(1300), samples, &error));
    assert(!telemetry.forward_browser_samples(packet(43, "focus_changed", 2), {}, at(1300), samples, &error));
    assert(!telemetry.maintain_browser_startup(post, at(2200)) && posted.size() == 3);
    assert(telemetry.apply_enable(enable));
    assert(telemetry.post_browser_enable(post, at(2200)) && posted.size() == 3);
    // An already queued retry reply is rejected as another startup, while an
    // ordinary standalone packet remains eligible after the quota window resets.
    assert(!telemetry.forward_browser_samples(packet(43, "collector_started", 0), {}, at(2201), samples, &error));
    assert(telemetry.forward_browser_samples(packet(43, "focus_changed", 3), {}, at(2201), samples, &error));
    telemetry.advance_collector(44, at(2300));
    assert(telemetry.post_browser_enable(post, at(2300)));
    assert(!telemetry.maintain_browser_startup(post, at(3299)));
    assert(!telemetry.maintain_browser_startup([](std::string_view) { return false; }, at(3300)));
    assert(!telemetry.maintain_browser_startup(post, at(3400))); // Failed attempts are throttled too.
    assert(telemetry.maintain_browser_startup(post, at(4300)));
    assert(telemetry.maintain_browser_startup(post, at(5300)));
    const auto count_after_budget = posted.size();
    assert(!telemetry.maintain_browser_startup(post, at(6300)) && posted.size() == count_after_budget);
    assert(!telemetry.maintain_browser_startup(post, at(10000)) && posted.size() == count_after_budget);
    telemetry.advance_collector(45, at(10100));
    assert(telemetry.post_browser_enable(post, at(10100)));
    assert(telemetry.maintain_browser_startup(post, at(11100)));
    telemetry.stop();
    assert(!telemetry.maintain_browser_startup(post, at(12100)));
}

void exercise_manifest_question_id_bounds() {
    std::error_code error;
    const auto temporary = std::filesystem::temp_directory_path(error);
    assert(!error);
    const auto root = temporary / ("nstu-exam-question-ids-" +
        std::to_string(GetCurrentProcessId()) + "-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto package = root / "sample";
    const auto web = package / "exam" / "web";
    assert(std::filesystem::create_directories(web, error));
    assert(!error);
    {
        std::ofstream page(web / "index.html", std::ios::binary);
        assert(page);
        page << "<!doctype html><title>question IDs</title>";
    }
    for (const std::size_t bytes : {65u, 96u, 97u}) {
        const std::string question_id(bytes, 'q');
        const std::string manifest =
            "{\"id\":\"question-id-bounds\",\"title\":\"Question IDs\",\"durationSeconds\":600,"
            "\"questions\":[{\"id\":\"" + question_id +
            "\",\"type\":\"short_answer\",\"prompt\":\"Answer\"}]}";
        {
            std::ofstream file(package / "manifest.json", std::ios::binary | std::ios::trunc);
            assert(file);
            file << manifest;
        }
        nstu::client::ExamPackageReport report;
        std::string inspection_error;
        const bool accepted = nstu::client::inspect_exam_package(
            package, web, {}, false, report, &inspection_error);
        assert(accepted == (bytes <= 96));
        if (accepted) assert(report.manifest_json == manifest);
        else assert(inspection_error == "exam manifest question is invalid");
    }
    std::filesystem::remove_all(root, error);
    assert(!error);
}

void exercise_parser_question_id_bounds() {
    using namespace nstu::client;
    using namespace nstu::exam;
    const std::string id65(65, 'a');
    const std::string id96(96, 'b');
    const std::string id128(128, 'c'); // Shared parser/IPC bound; not manifest-valid.
    const std::string id129(129, 'd');
    std::string multibyte;
    for (unsigned i = 0; i < 43; ++i) multibyte += "\xe2\x82\xac"; // 43 characters, 129 bytes.
    const std::string invalid_utf8 = id65 + "\xc3\x28";
    const std::string nul_id = id65 + std::string(1, '\0');
    const std::set<std::string> allowed = {id65, id96, id128, id129, multibyte, invalid_utf8, nul_id};
    const auto packet = [](const std::string& id, std::uint64_t revision = 7) {
        return "{\"type\":\"exam_telemetry_samples\",\"samples\":[{\"kind\":\"question_dwell\","
            "\"source\":\"native\",\"collectorId\":42,\"questionId\":\"" + id +
            "\",\"questionRevision\":" + std::to_string(revision) +
            ",\"monotonicElapsedMilliseconds\":12500,\"durationMilliseconds\":3500,"
            "\"clientTimeUnixMilliseconds\":1700000001000,\"numericValue\":9,\"count\":1,"
            "\"reason\":\"navigation_button\"}]}";
    };
    std::array<bool, 2> admitted{};
    std::vector<TelemetrySample> samples;
    std::string error;
    std::size_t index = 0;
    // Record both original-boundary outcomes before either positive assertion.
    for (const auto& id : {id65, id96}) {
        error.clear();
        admitted[index++] = parse_browser_telemetry_samples(packet(id), 42, allowed, samples, &error);
        std::fprintf(stderr, "question-ID parser: %zu-byte accepted=%d error=%s\n",
                     id.size(), admitted[index - 1] ? 1 : 0, error.c_str());
    }
    assert(admitted[0]);
    assert(admitted[1]);
    for (const auto& id : {id65, id96, id128}) {
        assert(parse_browser_telemetry_samples(packet(id), 42, allowed, samples, &error));
        assert(samples.size() == 1);
        const auto& sample = samples.front();
        assert(sample.question_id == id && sample.question_revision == 7);
        assert(sample.kind == TelemetrySampleKind::question_dwell);
        assert(sample.source == TelemetrySource::browser && sample.collector_id == 42);
        assert(sample.monotonic_elapsed_milliseconds == 12500 && sample.duration_milliseconds == 3500);
        assert(sample.client_time_unix_milliseconds == 1700000001000ULL);
        assert(sample.numeric_value == 9 && sample.count == 1);
        assert(sample.reason == TelemetryReason::navigation_button);
        const auto payload = encode_exam_telemetry_samples_payload(samples);
        assert(!payload.empty());
        const auto decoded = decode_exam_telemetry_samples_payload(payload);
        assert(decoded && decoded->size() == 1);
        assert(decoded->front().question_id == id && decoded->front().question_revision == 7);
        assert(encode_exam_telemetry_samples_payload(*decoded) == payload);
        assert_agent_round_trip(AgentMessageType::exam_telemetry_samples, payload);
    }
    for (const auto& id : {id129, multibyte}) {
        assert(!parse_browser_telemetry_samples(packet(id), 42, allowed, samples, &error));
        assert(error == "questionId exceeds maximum length");
    }
    assert(!parse_browser_telemetry_samples(packet(invalid_utf8), 42, allowed, samples, &error));
    assert(!parse_browser_telemetry_samples(packet(id65 + "\\u0000"), 42, allowed, samples, &error));
    assert(!parse_browser_telemetry_samples(packet(std::string(96, 'z')), 42, allowed, samples, &error));
    assert(error == "questionId not in allowed questions");
    assert(!parse_browser_telemetry_samples(packet(id65, 0), 42, allowed, samples, &error));
    assert(!parse_browser_telemetry_samples(packet(id96, 4294967296ULL), 42, allowed, samples, &error));
    assert(!parse_browser_telemetry_samples(packet("", 1), 42, allowed, samples, &error));
    // An empty allowlist keeps the existing pure-parser behavior.
    assert(parse_browser_telemetry_samples(packet(id96), 42, {}, samples, &error));
    assert(samples.front().question_id == id96);
}

bool exercise_live_question_id_binding() {
    using namespace nstu;
    using namespace nstu::client;
    using Clock = ExamHostTelemetryState::Clock;
    const Clock::time_point origin{};
    const auto at = [origin](int ms) { return origin + std::chrono::milliseconds(ms); };
    const auto event = make_event();
    exam::StateRequest context;
    context.package_id = event.package_id; context.package_digest = event.package_digest;
    context.client_id = event.client_id; context.session_id = event.session_id;
    context.candidate_id = event.candidate_id;
    exam::TelemetryEnable enable;
    enable.package_id = context.package_id; enable.package_digest = context.package_digest;
    enable.client_id = context.client_id; enable.session_id = context.session_id;
    enable.candidate_id = context.candidate_id;
    enable.server_time_unix_milliseconds = 1700000000000ULL;
    const std::string id65(65, 'a');
    const std::string id96(96, 'b');
    const std::set<std::string> allowed = {id65, id96};
    const auto packet = [](const std::string& raw_collector, const std::string& id, unsigned elapsed) {
        return "{\"type\":\"exam_telemetry_samples\",\"samples\":[{" + raw_collector +
            "\"kind\":\"question_dwell\",\"source\":\"native\",\"questionId\":\"" + id +
            "\",\"questionRevision\":2,\"monotonicElapsedMilliseconds\":" + std::to_string(elapsed) +
            ",\"durationMilliseconds\":5,\"clientTimeUnixMilliseconds\":1700000001000,"
            "\"count\":1,\"reason\":\"navigation_button\"}]}";
    };
    const auto startup = [](unsigned collector, unsigned elapsed) {
        return "{\"type\":\"exam_telemetry_samples\",\"samples\":[{\"kind\":\"collector_started\","
            "\"collectorId\":" + std::to_string(collector) +
            ",\"monotonicElapsedMilliseconds\":" + std::to_string(elapsed) +
            ",\"clientTimeUnixMilliseconds\":1700000000000,\"reason\":\"collector_initial\"}]}";
    };
    ExamHostTelemetryState telemetry;
    assert(telemetry.start(context, 42, origin));
    assert(telemetry.apply_enable(enable));
    std::string posted;
    const auto post = [&posted](std::string_view json) { posted = json; return true; };
    std::vector<exam::TelemetrySample> samples;
    std::string error;
    assert(telemetry.post_browser_enable(post, at(40)));
    assert(posted.find("\"collectorId\":42") != std::string::npos);
    assert(posted.find("\"collectorElapsedMilliseconds\":40") != std::string::npos);
    assert(telemetry.forward_browser_samples(startup(42, 40), allowed, at(65), samples, &error));
    assert(samples.front().kind == exam::TelemetrySampleKind::collector_started);
    assert(samples.front().numeric_value == 27);
    std::fprintf(stderr, "question-ID live: collector 42 clock started successfully\n");
    bool admitted = true;
    for (const auto& id : {id65, id96}) {
        error.clear();
        const bool accepted = telemetry.forward_browser_samples(
            packet("\"collectorId\":42,", id, 70), allowed, at(100), samples, &error);
        std::fprintf(stderr, "question-ID live: %zu-byte accepted=%d error=%s\n",
                     id.size(), accepted ? 1 : 0, error.c_str());
        admitted = admitted && accepted;
        if (accepted) {
            assert(samples.size() == 1);
            assert(samples.front().question_id == id && samples.front().question_revision == 2);
            assert(samples.front().source == exam::TelemetrySource::browser);
            assert(samples.front().collector_id == 42);
            assert(samples.front().monotonic_elapsed_milliseconds == 70);
            assert(samples.front().duration_milliseconds == 5 && samples.front().count == 1);
            assert(samples.front().client_time_unix_milliseconds == 1700000001000ULL);
            assert(samples.front().reason == exam::TelemetryReason::navigation_button);
        }
        for (const std::string raw_id : {"", "\"collectorId\":0,", "\"collectorId\":41,"}) {
            assert(!telemetry.forward_browser_samples(packet(raw_id, id, 70), allowed, at(100), samples, &error));
        }
    }
    telemetry.advance_collector(43, at(200));
    assert(telemetry.post_browser_enable(post, at(210)));
    assert(posted.find("\"collectorId\":43") != std::string::npos);
    assert(posted.find("\"collectorElapsedMilliseconds\":10") != std::string::npos);
    for (const auto& id : {id65, id96}) {
        assert(!telemetry.forward_browser_samples(packet("\"collectorId\":42,", id, 20), allowed, at(230), samples, &error));
        assert(!telemetry.forward_browser_samples(packet("\"collectorId\":43,", id, 20), allowed, at(230), samples, &error));
    }
    assert(!telemetry.forward_browser_samples(startup(42, 10), allowed, at(230), samples, &error));
    assert(!telemetry.forward_browser_samples(startup(43, 11), allowed, at(230), samples, &error));
    assert(telemetry.forward_browser_samples(startup(43, 10), allowed, at(235), samples, &error));
    for (const auto& id : {id65, id96}) {
        const bool accepted = telemetry.forward_browser_samples(
            packet("\"collectorId\":43,", id, 40), allowed, at(250), samples, &error);
        admitted = admitted && accepted;
        if (accepted) {
            assert(samples.front().question_id == id && samples.front().question_revision == 2);
            assert(samples.front().collector_id == 43 && samples.front().source == exam::TelemetrySource::browser);
        }
        assert(!telemetry.forward_browser_samples(packet("\"collectorId\":42,", id, 40), allowed, at(250), samples, &error));
    }
    return admitted;
}

int main() {
    exercise_manifest_question_id_bounds();
    // Bootstrap and record both live IDs before the parser's RED assertion.
    const bool live_question_ids_accepted = exercise_live_question_id_binding();
    exercise_parser_question_id_bounds();
    assert(live_question_ids_accepted);
    exercise_native_startup_full_window_recovery();
    exercise_native_capture_clock();
    exercise_browser_clock_safe_boundary();
    test_collector_identity_is_distinct_across_restart_and_hosts();
    using namespace nstu;

    // An idle/default context must not authorize collection, even if all
    // fields of the supplied enable equal the host's empty initial fields.
    {
        client::ExamHost idle_host;
        assert(!idle_host.apply_telemetry_enable(exam::TelemetryEnable{}));
        assert(!idle_host.telemetry_enabled());
        assert(idle_host.telemetry_collector_id() == 0);
    }

    exercise_exam_path_policy();
    exercise_exam_profile_isolation();
    exercise_exam_package_inspection();
    exercise_exam_manifest_asset_validation();

    const auto event = make_event();
    const auto event_hash = exam::hash_answer_event(event);
    assert(event_hash.has_value());
    const auto event_payload = exam::encode_answer_event(event);
    assert(!event_payload.empty());
    assert(exam::decode_answer_event(event_payload).has_value());
    assert_agent_round_trip(client::AgentMessageType::exam_answer_event,
                            event_payload);

    exam::StateRequest request;
    request.package_id = event.package_id;
    request.package_digest = event.package_digest;
    request.client_id = event.client_id;
    request.session_id = event.session_id;
    request.candidate_id = event.candidate_id;
    const auto request_payload = exam::encode_state_request(request);
    assert(!request_payload.empty());
    const auto decoded_request = exam::decode_state_request(request_payload);
    assert(decoded_request.has_value());
    assert(decoded_request->package_id == request.package_id);
    assert(decoded_request->candidate_id == request.candidate_id);
    assert_agent_round_trip(client::AgentMessageType::exam_state_request,
                            request_payload);

    exam::AnswerAck ack;
    ack.status = exam::AnswerAckStatus::accepted;
    ack.session_id = event.session_id;
    ack.sequence = 1;
    ack.highest_contiguous_sequence = 1;
    ack.server_time_unix_milliseconds = 1'700'000'000'100ULL;
    ack.event_hash = *event_hash;
    ack.state_hash[0] = std::byte{0xa1};
    const auto ack_payload = exam::encode_answer_ack(ack);
    assert(!ack_payload.empty());
    const auto decoded_ack = exam::decode_answer_ack(ack_payload);
    assert(decoded_ack.has_value());
    assert(decoded_ack->status == exam::AnswerAckStatus::accepted);
    assert(decoded_ack->event_hash == ack.event_hash);
    assert_agent_round_trip(client::AgentMessageType::exam_answer_ack,
                            ack_payload);

    exam::StateResponse response;
    response.package_id = event.package_id;
    response.package_digest = event.package_digest;
    response.client_id = event.client_id;
    response.session_id = event.session_id;
    response.candidate_id = event.candidate_id;
    response.highest_contiguous_sequence = 1;
    response.state_hash = ack.state_hash;
    response.last_event_hash = *event_hash;
    response.answers.push_back({
        .question_id = event.question_id,
        .question_revision = event.question_revision,
        .sequence = event.sequence,
        .kind = exam::AnswerKind::upsert,
        .answer = event.answer,
        .event_hash = *event_hash,
    });
    const auto state_payload = exam::encode_state_response(response);
    assert(!state_payload.empty());
    const auto decoded_state = exam::decode_state_response(state_payload);
    assert(decoded_state.has_value());
    assert(decoded_state->answers.size() == 1);
    assert(decoded_state->answers.front().answer == event.answer);
    assert_agent_round_trip(client::AgentMessageType::exam_state_response,
                            state_payload);

    // Only service responses are allowed into the agent-side host bridge.
    // Browser-originated events and state requests must never be presented as
    // host commands, even when their wire framing is valid.
    client::ExamBridge bridge;
    assert(!bridge.publish({client::AgentMessageType::exam_answer_event,
                            event_payload}));
    assert(!bridge.publish({client::AgentMessageType::exam_state_request,
                            request_payload}));
    assert(bridge.publish({client::AgentMessageType::exam_answer_ack,
                           ack_payload}));
    assert(bridge.publish({client::AgentMessageType::exam_state_response,
                           state_payload}));
    const auto bridge_ack = bridge.try_pop();
    assert(bridge_ack.has_value());
    assert(bridge_ack->type == client::AgentMessageType::exam_answer_ack);
    const auto bridge_state = bridge.try_pop();
    assert(bridge_state.has_value());
    assert(bridge_state->type == client::AgentMessageType::exam_state_response);
    assert(!bridge.try_pop().has_value());

    // Framing failures are rejected before a host can inspect the payload.
    assert(!client::decode_agent_message({}).has_value());
    auto truncated = client::encode_agent_message(
        {client::AgentMessageType::exam_answer_ack, ack_payload});
    assert(!truncated.empty());
    truncated.pop_back();
    assert(!client::decode_agent_message(truncated).has_value());

    auto wrong_magic = client::encode_agent_message(
        {client::AgentMessageType::exam_answer_ack, ack_payload});
    wrong_magic[0] ^= std::byte{1};
    assert(!client::decode_agent_message(wrong_magic).has_value());

    auto wrong_version = client::encode_agent_message(
        {client::AgentMessageType::exam_answer_ack, ack_payload});
    wrong_version[4] ^= std::byte{1};
    assert(!client::decode_agent_message(wrong_version).has_value());

    auto unknown_type = client::encode_agent_message(
        {client::AgentMessageType::exam_answer_ack, ack_payload});
    unknown_type[6] = std::byte{0xff};
    unknown_type[7] = std::byte{0xff};
    assert(!client::decode_agent_message(unknown_type).has_value());

    auto trailing = client::encode_agent_message(
        {client::AgentMessageType::exam_answer_ack, ack_payload});
    trailing.push_back(std::byte{0});
    assert(!client::decode_agent_message(trailing).has_value());

    client::AgentMessage oversized{
        client::AgentMessageType::exam_answer_ack,
        std::vector<std::byte>(client::kMaximumAgentPayloadBytes + 1)};
    assert(client::encode_agent_message(oversized).empty());

    // A valid IPC frame does not make an invalid exam payload trustworthy;
    // the host must still decode the typed payload before using it.
    auto malformed_event = event_payload;
    malformed_event.pop_back();
    const auto malformed_frame = client::encode_agent_message(
        {client::AgentMessageType::exam_answer_event, malformed_event});
    assert(!malformed_frame.empty());
    const auto framed_event = client::decode_agent_message(malformed_frame);
    assert(framed_event.has_value());
    assert(!exam::decode_answer_event(framed_event->payload).has_value());

    exercise_exam_host_telemetry();
    exercise_started_host_telemetry_state();

    return 0;
}
