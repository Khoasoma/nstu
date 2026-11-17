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
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <new>
#include <string>
#include <vector>

namespace capture_allocation_test {
thread_local bool active = false;
thread_local std::size_t count = 0;
thread_local std::size_t fail_at = 0;
}

void* operator new(std::size_t size) {
    if (capture_allocation_test::active &&
        ++capture_allocation_test::count == capture_allocation_test::fail_at) throw std::bad_alloc();
    if (void* value = std::malloc(size == 0 ? 1 : size)) return value;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
// Keep replacement deallocation opaque to optimizer allocation-pair diagnostics.
#if defined(_MSC_VER)
__declspec(noinline)
#else
[[gnu::noinline]]
#endif
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { ::operator delete(value); }
void operator delete(void* value, std::size_t) noexcept { ::operator delete(value); }
void operator delete[](void* value, std::size_t) noexcept { ::operator delete(value); }

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

void exercise_ordinary_host_policy() {
    using namespace nstu::client;
    const auto managed = exam_host_window_policy(ExamHostPolicy::managed);
    const auto ordinary = exam_host_window_policy(ExamHostPolicy::ordinary_user);
    assert(ExamHostOptions{}.policy == ExamHostPolicy::managed);
    assert(managed && managed->style == WS_POPUP);
    assert(managed->extended_style == (WS_EX_TOPMOST | WS_EX_TOOLWINDOW));
    assert(managed->managed_lockdown);
    assert(ordinary && ordinary->style == WS_OVERLAPPEDWINDOW);
    assert(ordinary->extended_style == 0 && !ordinary->managed_lockdown);
    assert(!exam_host_window_policy(static_cast<ExamHostPolicy>(255)));

    const auto root = std::filesystem::current_path() /
        ("nstu-ordinary-host-policy-" + std::to_string(GetCurrentProcessId()));
    std::error_code fs_error;
    const auto package_area = root / "student" / "packages";
    const auto package = package_area / "verified-package";
    const auto web = package / "exam" / "web";
    const auto profiles = root / "student" / "profiles";
    assert(std::filesystem::create_directories(web, fs_error));
    assert(!fs_error);
    assert(std::filesystem::create_directories(profiles, fs_error));
    assert(!fs_error);
    const std::string manifest = R"({"id":"cs-package","title":"Programming","durationSeconds":3600,"contentMode":"computer_science","programmingVersion":1,"sessionMode":"contest","languages":["cpp20","python3"],"questions":[{"id":"sum","type":"programming","revision":2,"prompt":"Add integers","samples":[{"id":"sample-1","input":"1 2\n","output":"3\n"}],"timeLimitMs":1000,"memoryLimitMiB":64}]})";
    auto write = [](const auto& path, std::string_view text) {
        std::ofstream stream(path, std::ios::binary);
        stream << text;
        stream.close();
        assert(stream.good());
    };
    write(package / "manifest.json", manifest);
    write(web / "index.html", "<!doctype html><title>Public contest</title>");
    assert(std::filesystem::create_directories(package / "exam" / "shared", fs_error));
    assert(!fs_error);
    write(package / "exam" / "shared" / "programming.js", "// public fixture asset");
    ExamPackageReport report;
    std::string error;
    assert(inspect_exam_package(package, web, {}, false, report, &error));
    ExamHostOptions options;
    options.policy = ExamHostPolicy::ordinary_user;
    options.allowed_package_root = package_area;
    options.package_root = package;
    options.allowed_user_data_root = profiles;
    options.user_data_root = profiles / "bound-session";
    options.expected_digest_hex = report.digest_hex;
    options.context_json = "{\"packageId\":\"cs-package\",\"packageDigestHex\":\"" +
        report.digest_hex + "\",\"clientIdHex\":\"" + std::string(32, '1') +
        "\",\"sessionIdHex\":\"" + std::string(32, '2') +
        "\",\"candidateId\":\"candidate-01\",\"synchronizedStart\":true}";
    assert(validate_exam_host_options(options, report, &error));
    assert(report.package_root == package);
    ExamHost callbacks_rejected;
    error.clear();
    assert(!callbacks_rejected.start(nullptr, options, {}, &error));
    assert(error.find("typed room callbacks") != std::string::npos);
    ExamHostCallbacks mixed_callbacks;
    mixed_callbacks.enqueue_programming_request = [](const auto&) { return true; };
    mixed_callbacks.report_ready = [](const auto&) { return true; };
    mixed_callbacks.before_close = [](const auto&) { return true; };
    mixed_callbacks.send_to_service = [](AgentMessage) {};
    error.clear();
    assert(!callbacks_rejected.start(nullptr, options, mixed_callbacks, &error));
    assert(error.find("typed room callbacks") != std::string::npos);
    auto rejects = [&](ExamHostOptions invalid) {
        error.clear();
        assert(!validate_exam_host_options(invalid, report, &error));
        assert(!error.empty());
    };
    auto invalid = options; invalid.allowed_package_root.clear(); rejects(invalid);
    invalid = options; invalid.allowed_user_data_root.clear(); rejects(invalid);
    invalid = options; invalid.allowed_package_root = "relative"; rejects(invalid);
    invalid = options; invalid.package_root = package_area; rejects(invalid);
    invalid = options; invalid.package_root = package_area / ".." / "outside"; rejects(invalid);
    invalid = options; invalid.allowed_package_root = root / "student" / "packages-prefix"; rejects(invalid);
    invalid = options; invalid.web_root = profiles; rejects(invalid);
    invalid = options; invalid.user_data_root = profiles; rejects(invalid);
    invalid = options; invalid.user_data_root = profiles / ".." / "outside"; rejects(invalid);
    invalid = options; invalid.user_data_root.clear(); rejects(invalid);
    invalid = options; invalid.allowed_user_data_root = package_area;
    invalid.user_data_root = package / "browser-profile"; rejects(invalid);
    invalid = options; invalid.allowed_data_root = root; rejects(invalid);
    invalid = options; invalid.allowed_user_data_root = package_area;
    invalid.user_data_root = package_area / "other-profile"; rejects(invalid);
    invalid = options; invalid.require_digest = false; rejects(invalid);
    invalid = options; invalid.expected_digest_hex.clear(); rejects(invalid);
    invalid = options; invalid.expected_digest_hex = std::string(64, '0'); rejects(invalid);
    invalid = options; invalid.policy = static_cast<ExamHostPolicy>(255); rejects(invalid);
    invalid = options; invalid.context_json.insert(invalid.context_json.size() - 1,
        ",\"operatorToken\":\"private\""); rejects(invalid);
    invalid = options; invalid.context_json.insert(invalid.context_json.size() - 1,
        ",\"candidateId\":\"other\""); rejects(invalid);
    invalid = options; invalid.context_json.replace(invalid.context_json.find("candidate-01"),
        12, ""); rejects(invalid);
    invalid = options; invalid.context_json.replace(invalid.context_json.find(std::string(32, '1')),
        32, std::string(32, '0')); rejects(invalid);
    invalid = options; invalid.context_json.replace(invalid.context_json.find(report.digest_hex),
        64, std::string(64, '3')); rejects(invalid);
    invalid = options; invalid.context_json.replace(invalid.context_json.find("synchronizedStart\":true"),
        23, "synchronizedStart\":false"); rejects(invalid);
    invalid = options;
    auto uppercase_digest = options.expected_digest_hex;
    for (auto& digit : uppercase_digest) if (digit >= 'a' && digit <= 'f') digit = static_cast<char>(digit - 'a' + 'A');
    assert(uppercase_digest != options.expected_digest_hex);
    invalid.context_json.replace(invalid.context_json.find(options.expected_digest_hex), 64, uppercase_digest);
    rejects(invalid);

    // Managed inspection retains its existing unrooted diagnostics contract.
    auto diagnostics = options; diagnostics.policy = ExamHostPolicy::managed;
    diagnostics.allowed_package_root.clear(); diagnostics.allowed_user_data_root.clear();
    assert(validate_exam_host_options(diagnostics, report, &error));
    auto exam_manifest = manifest;
    exam_manifest.replace(exam_manifest.find("contest"), 7, "exam");
    write(package / "manifest.json", exam_manifest);
    assert(inspect_exam_package(package, web, {}, false, report, &error));
    invalid = options;
    invalid.context_json.replace(invalid.context_json.find(invalid.expected_digest_hex), 64, report.digest_hex);
    invalid.expected_digest_hex = report.digest_hex;
    rejects(invalid);
    write(package / "manifest.json", manifest);

    nstu::exam::StateRequest trusted;
    trusted.package_id = "cs-package"; trusted.candidate_id = "candidate-01";
    fill_bytes(trusted.package_digest, 0x10);
    fill_bytes(trusted.client_id, 0x30);
    fill_bytes(trusted.session_id, 0x50);
    const auto request_json = "{\"type\":\"exam_programming_request\",\"context\":" +
        nstu::exam::programming::context_json(trusted) +
        R"(,"operation":"draft","requestId":"draft-01","problemId":"sum","problemRevision":2,"language":"cpp20","draftRevision":1,"source":"int main() {}"})";
    const auto parsed = parse_browser_programming_request(request_json, manifest, trusted);
    assert(parsed && parsed->source == "int main() {}" && parsed->context.candidate_id == trusted.candidate_id);
    auto hostile = request_json;
    hostile.replace(hostile.find("candidate-01"), 12, "another-candidate");
    assert(!parse_browser_programming_request(hostile, manifest, trusted));
    for (std::string_view extra : {",\"operatorToken\":\"private\"", ",\"score\":100",
                                   ",\"workstationPolicyReady\":true"}) {
        hostile = request_json; hostile.insert(hostile.size() - 1, extra);
        assert(!parse_browser_programming_request(hostile, manifest, trusted));
    }
    hostile = request_json; hostile.replace(hostile.find("exam_programming_request"), 24, "remote_input");
    assert(!parse_browser_programming_request(hostile, manifest, trusted));
    write(package / "private-cases.json", "[]");
    rejects(options);
    assert(std::filesystem::remove(package / "private-cases.json", fs_error));
    assert(!fs_error);
    write(web / "index.html", "changed public entry point");
    rejects(options);
    ExamHost managed_host;
    assert(!managed_host.request_close());
    ExamHost ordinary_host;
    std::string latest_source = "// current editor source";
    std::string preserved_source;
    bool capture_complete = false;
    ExamHostCallbacks callbacks;
    callbacks.enqueue_programming_request = [](const auto&) { return true; };
    callbacks.report_ready = [](const auto&) { return true; };
    callbacks.before_close = [&](const auto& pending) {
        assert(pending.empty());
        if (!capture_complete) return false;
        preserved_source = latest_source;
        return true;
    };
    // A failed preparation has no physical window. Its close gate still
    // preserves the same owner contract, without simulating browser capture.
    assert(!ordinary_host.start(nullptr, options, callbacks, &error));
    assert(ordinary_host.state() == ExamHostState::failed);
    assert(!ordinary_host.request_close());
    assert(preserved_source.empty());
    assert(ordinary_host.state() == ExamHostState::failed);
    auto replacement_callbacks = callbacks;
    replacement_callbacks.before_close = [](const auto&) { return true; };
    assert(!ordinary_host.start(nullptr, options, replacement_callbacks, &error));
    // A replacement cannot waive the old owner's unfinished preservation.
    assert(!ordinary_host.request_close());
    assert(preserved_source.empty());
    assert(!ordinary_host.apply_telemetry_enable(nstu::exam::TelemetryEnable{}));
    assert(!ordinary_host.telemetry_enabled() && ordinary_host.telemetry_collector_id() == 0);
    latest_source += "\n// newer edit remains recoverable";
    capture_complete = true;
    assert(ordinary_host.request_close());
    assert(preserved_source == latest_source);
    assert(ordinary_host.state() == ExamHostState::idle);
    callbacks.before_close = [](const auto&) -> bool { throw 1; };
    assert(!ordinary_host.start(nullptr, options, callbacks, &error));
    assert(!ordinary_host.request_close());
    assert(ordinary_host.state() == ExamHostState::failed);
    ordinary_host.stop();
    callbacks.before_close = [&](const auto&) {
        assert(!ordinary_host.request_close());
        return true;
    };
    assert(!ordinary_host.start(nullptr, options, callbacks, &error));
    assert(ordinary_host.request_close());
    callbacks.before_close = [&](const auto&) {
        ordinary_host.stop();
        return true;
    };
    assert(!ordinary_host.start(nullptr, options, callbacks, &error));
    assert(!ordinary_host.request_close());
    assert(ordinary_host.state() == ExamHostState::idle);
    std::filesystem::remove_all(root, fs_error);
    assert(!fs_error);
}

void exercise_contest_lifecycle_clock() {
    using namespace nstu::client;
    ExamHostContestLifecycle value;
    value.context.package_id = "cs-package"; value.context.candidate_id = "candidate-01";
    fill_bytes(value.context.package_digest, 0x10); fill_bytes(value.context.client_id, 0x30);
    fill_bytes(value.context.session_id, 0x50);
    value.phase = ExamHostContestPhase::running; value.revision = 3;
    value.server_start = 100000; value.deadline = 700000; value.server_time = 130000;
    ExamHostContestLifecycleState clock;
    assert(clock.accept(value));
    assert(clock.json().find("\"serverTime\":130000") != std::string::npos);
    auto stale = value; stale.revision = 2; assert(!clock.accept(stale));
    stale = value; stale.server_time--; assert(!clock.accept(stale));
    stale = value; stale.deadline++; stale.revision++; assert(!clock.accept(stale));
    stale = value; stale.context.candidate_id = "foreign"; assert(!clock.accept(stale));
    stale = value; stale.phase = ExamHostContestPhase::joining; assert(!clock.accept(stale));
    value.phase = ExamHostContestPhase::closed; value.revision++;
    assert(clock.accept(value));
    assert(clock.json().find("\"phase\":\"Closed\"") != std::string::npos);
    stale = value; stale.phase = ExamHostContestPhase::running; stale.revision++; assert(!clock.accept(stale));
    clock.reset();
    value.server_time = 9007199254740992ULL; assert(!clock.accept(value));
    value.server_time = 130000; value.server_start = 0; assert(!clock.accept(value));
    value.deadline = 0; assert(clock.accept(value));
    stale = value; stale.revision++; stale.server_start = 100000; stale.deadline = 700000;
    assert(!clock.accept(stale));
    assert(clock.json().find("private") == std::string::npos);
    ExamHostContestLifecycleState publication;
    value.phase = ExamHostContestPhase::running; value.server_start = 100000; value.deadline = 700000;
    assert(publication.accept(value));
    assert(!publication.publish([&](std::string_view message) {
        assert(message.find("Running") != std::string::npos);
        value.phase = ExamHostContestPhase::closed; ++value.revision;
        assert(publication.accept(value));
        return true;
    }));
    assert(publication.publish([](std::string_view message) { return message.find("Closed") != std::string::npos; }));
}

void exercise_close_capture_consumer() {
    using namespace nstu::client;
    nstu::exam::StateRequest context;
    context.package_id = "cs-package"; context.candidate_id = "candidate-01";
    fill_bytes(context.package_digest, 0x10); fill_bytes(context.client_id, 0x30);
    fill_bytes(context.session_id, 0x50);
    const std::string manifest = R"({"id":"cs-package","title":"CS","durationSeconds":60,"contentMode":"computer_science","programmingVersion":1,"sessionMode":"contest","languages":["cpp20","python3"],"questions":[{"id":"sum","type":"programming","revision":2,"prompt":"Sum","timeLimitMs":1000,"memoryLimitMiB":64,"samples":[{"id":"sample","input":"1","output":"1"}]}]})";
    const auto origin = ExamHostCloseCaptureState::Clock::now();
    ExamHostCloseCaptureState capture;
    assert(capture.start(context, manifest, 7));
    std::string posted;
    const auto post = [&](std::string_view json) { posted = json; return true; };
    assert(capture.prepare(post, origin));
    assert(posted.starts_with("{\"type\":\"exam_close_capture_request\",\"context\":"));
    const auto nonce = posted.substr(posted.find("\"nonce\":\"") + 9, 64);
    assert(nonce.size() == 64);
    assert(posted == "{\"type\":\"exam_close_capture_request\",\"context\":" +
        nstu::exam::programming::context_json(context) + ",\"nonce\":\"" + nonce + "\"}");
    assert(!capture.prepare(post, origin));
    assert(!capture.start(context, manifest, 8));
    const std::string document = R"({"problemId":"sum","problemRevision":2,"language":"cpp20","draftRevision":3,"sourceDigest":")" +
        std::string(64, 'a') + R"(","requestId":"draft-01","status":"forwarded"})";
    const auto reply = [&](std::string_view documents, bool complete = true) {
        return "{\"type\":\"exam_close_capture_" + std::string(complete ? "complete" : "failed") +
            "\",\"context\":" + nstu::exam::programming::context_json(context) +
            ",\"nonce\":\"" + nonce + "\",\"localSaved\":true,\"documents\":[" + std::string(documents) + "]" +
            (complete ? "}" : ",\"reason\":\"native_enqueue_unavailable\"}");
    };
    const auto valid = reply(document);
    auto rejects = [&](std::string hostile) {
        assert(!capture.accept(hostile, origin)); assert(!capture.report());
    };
    for (std::string_view extra : {",\"source\":\"private source\"", ",\"score\":1", ",\"operatorToken\":\"secret\"",
                                  ",\"runnerReady\":true", ",\"noSourceProblemIds\":[\"sum\"]", ",\"localSaved\":true"}) {
        auto hostile = valid; hostile.insert(hostile.size() - 1, extra); rejects(hostile);
    }
    const auto replace = [&](std::string from, std::string to) {
        auto hostile = valid; const auto at = hostile.find(from); assert(at != std::string::npos);
        hostile.replace(at, from.size(), to); rejects(hostile);
    };
    replace("candidate-01", "other-candidate"); replace(nonce, std::string(64, 'f'));
    replace("\"context\":{", "\"context\":{\"synchronizedStart\":true,");
    replace("\"problemRevision\":2", "\"problemRevision\":1");
    replace("\"draftRevision\":3", "\"draftRevision\":0");
    replace("\"draftRevision\":3", "\"draftRevision\":9007199254740992");
    replace("\"draftRevision\":3", "\"draftRevision\":3.0");
    replace("\"draftRevision\":3", "\"draftRevision\":\"3\"");
    replace("\"problemRevision\":2", "\"problemRevision\":4294967296");
    replace("\"language\":\"cpp20\"", "\"language\":\"shell\"");
    replace("\"problemId\":\"sum\"", "\"problemId\":\"other\"");
    replace("\"requestId\":\"draft-01\"", "\"requestId\":\"constructor\"");
    replace("\"requestId\":\"draft-01\"", "\"requestId\":\"" + std::string(97, 'x') + "\"");
    replace("\"status\":\"forwarded\"", "\"status\":\"queued\"");
    replace("\"status\":\"forwarded\"", "\"status\":\"saved\"");
    replace("\"localSaved\":true", "\"localSaved\":false");
    replace("\"localSaved\":true", "\"localSaved\":\"true\"");
    replace("\"documents\":[", "\"documents\":{\"invented\":[");
    replace("\"sourceDigest\":", "\"source\":\"private bytes\",\"sourceDigest\":");
    replace("\"sourceDigest\":", "\"privateCases\":[],\"sourceDigest\":");
    replace("exam_close_capture_complete", "remote_input");
    replace(std::string(64, 'a'), std::string(64, 'A'));
    replace(std::string(64, 'a'), std::string(63, 'a'));
    rejects(reply(document + "," + document));
    rejects(valid + std::string(65536, ' '));
    assert(capture.accept(valid, origin));
    const auto completed = *capture.report();
    assert(completed.complete && completed.local_saved && completed.generation == 7);
    assert(completed.documents.size() == 1 && completed.documents[0].draft_revision == 3);
    assert(completed.documents[0].source_digest == std::string(64, 'a'));
    assert(capture.is_current_complete(completed));
    assert(!capture.preserve([](const auto&) { return false; }));
    assert(!capture.preserve([](const auto&) -> bool { throw 1; }));
    assert(capture.preserve([&](const auto& report) {
        assert(capture.is_current_complete(report));
        return true;
    }));
    assert(capture.is_current_complete(completed));
    assert(!capture.accept(valid, origin));
    auto tampered = completed; tampered.nonce = std::string(64, 'f');
    assert(!capture.is_current_complete(tampered));
    tampered = completed; tampered.documents[0].draft_revision++;
    assert(!capture.is_current_complete(tampered));
    tampered = completed; tampered.documents[0].source_digest = std::string(64, 'b');
    assert(!capture.is_current_complete(tampered));
    tampered = completed; tampered.documents[0].request_id = "different-request";
    assert(!capture.is_current_complete(tampered));
    tampered = completed; tampered.context.candidate_id = "another-candidate";
    assert(!capture.is_current_complete(tampered));
    assert(!capture.accept("{\"type\":\"exam_ready\"}", origin));
    assert(capture.is_current_complete(completed));
    // Failed/revocation is installed before the owner can retry its predicate.
    auto revoked = reply({}, false);
    revoked.replace(revoked.find("\"localSaved\":true"), 17, "\"localSaved\":false");
    assert(!capture.preserve([&](const auto& report) {
        assert(capture.is_current_complete(report));
        assert(capture.accept(revoked, origin));
        return true;
    }));
    assert(!capture.is_current_complete(completed));
    assert(capture.report() && !capture.report()->complete && capture.report()->documents.empty());
    assert(!capture.accept(valid, origin));
    assert(!capture.accept(revoked, origin));
    bool cancelled_before_post = false;
    assert(!capture.cancel([&](std::string_view json) {
        cancelled_before_post = !capture.report() && !capture.is_current_complete(completed);
        assert(!capture.cancel(post));
        assert(!capture.accept(valid, origin));
        assert(json == "{\"type\":\"exam_close_capture_cancel\",\"context\":" +
            nstu::exam::programming::context_json(context) + ",\"nonce\":\"" + nonce + "\"}");
        return false;
    }));
    assert(cancelled_before_post && !capture.prepare(post, origin));
    assert(capture.cancel(post)); assert(!capture.cancel(post));
    assert(capture.prepare(post, origin));
    assert(!capture.accept(valid, origin));
    assert(!capture.expired(origin + std::chrono::seconds(4)));
    assert(capture.expired(origin + std::chrono::seconds(5)));
    assert(capture.cancel(post));
    assert(capture.start(context, manifest, 8));
    assert(!capture.is_current_complete(completed));
    assert(capture.prepare(post, origin));
    const auto second_nonce = posted.substr(posted.find("\"nonce\":\"") + 9, 64);
    auto second_reply = valid; second_reply.replace(second_reply.find(nonce), 64, second_nonce);
    assert(!capture.accept(second_reply, origin + std::chrono::seconds(5)));
    assert(!capture.report()); assert(capture.cancel(post));
    assert(capture.prepare(post, origin));
    const auto third_nonce = posted.substr(posted.find("\"nonce\":\"") + 9, 64);
    auto third_reply = valid; third_reply.replace(third_reply.find(nonce), 64, third_nonce);
    assert(capture.accept(third_reply, origin));
    assert(!capture.preserve([&](const auto& report) {
        assert(!capture.preserve([](const auto&) { return true; }));
        assert(capture.is_current_complete(report));
        assert(capture.cancel(post));
        assert(capture.prepare(post, origin));
        return true;
    }));
    assert(capture.cancel(post));
    capture.stop(); assert(!capture.prepare(post, origin));
    assert(capture.start(context, manifest, 9));
    assert(!capture.prepare([](std::string_view) { return false; }, origin));
    assert(!capture.prepare(post, origin));
    assert(capture.cancel(post));
    assert(!capture.prepare([](std::string_view) -> bool { throw 1; }, origin));
    assert(capture.cancel(post));
    // The page's 256 consumed-nonce bound cannot be reset by another request.
    for (unsigned attempt = 2; attempt < 256; ++attempt) {
        assert(capture.prepare(post, origin)); assert(capture.cancel(post));
    }
    assert(!capture.prepare(post, origin));
    assert(!capture.start(context, manifest, 9));
    capture.stop();
    auto maximum_manifest = manifest;
    const auto questions_begin = maximum_manifest.find("\"questions\":[") + 13;
    const auto question = maximum_manifest.substr(questions_begin, maximum_manifest.size() - questions_begin - 2);
    std::string questions, documents;
    for (unsigned problem = 0; problem < 64; ++problem) {
        auto item = question; item.replace(item.find("sum"), 3, "p" + std::to_string(problem));
        if (problem) questions += ',';
        questions += item;
        for (const auto* language : {"cpp20", "python3"}) {
            auto inventory = document;
            inventory.replace(inventory.find("sum"), 3, "p" + std::to_string(problem));
            inventory.replace(inventory.find("cpp20"), 5, language);
            inventory.replace(inventory.find("draft-01"), 8, "draft-" + std::to_string(problem) + "-" + language);
            if (!documents.empty()) documents += ',';
            documents += inventory;
        }
    }
    maximum_manifest.replace(questions_begin, maximum_manifest.size() - questions_begin - 2, questions);
    assert(capture.start(context, maximum_manifest, 10));
    assert(capture.prepare(post, origin));
    const auto maximum_nonce = posted.substr(posted.find("\"nonce\":\"") + 9, 64);
    auto maximum_reply = reply(documents); maximum_reply.replace(maximum_reply.find(nonce), 64, maximum_nonce);
    auto over_limit = maximum_reply;
    over_limit.insert(over_limit.size() - 2, "," + document);
    assert(!capture.accept(over_limit, origin));
    assert(capture.accept(maximum_reply, origin));
    assert(capture.report()->documents.size() == 128);
    assert(capture.cancel(post));
    assert(capture.prepare(post, origin));
    const auto empty_nonce = posted.substr(posted.find("\"nonce\":\"") + 9, 64);
    auto empty = reply({}); empty.replace(empty.find(nonce), 64, empty_nonce);
    assert(capture.accept(empty, origin));
    assert(capture.report()->complete && capture.report()->documents.empty());
    assert(capture.cancel(post));
    capture.stop(); assert(capture.start(context, manifest, 11));
    assert(capture.prepare(post, origin));
    const auto failed_nonce = posted.substr(posted.find("\"nonce\":\"") + 9, 64);
    auto queued_document = document;
    queued_document.replace(queued_document.find("forwarded"), 9, "queued");
    auto failed = reply(queued_document, false);
    failed.replace(failed.find(nonce), 64, failed_nonce);
    auto bad_reason = failed;
    bad_reason.replace(bad_reason.find("native_enqueue_unavailable"), 26, "pretend_received");
    assert(!capture.accept(bad_reason, origin));
    assert(capture.accept(failed, origin));
    assert(capture.report() && !capture.report()->complete && capture.report()->local_saved);
    assert(capture.report()->documents[0].status == "queued");
    assert(!capture.preserve([](const auto&) { return true; }));
    assert(capture.cancel(post));
    assert(capture.prepare(post, origin));
    const auto received_nonce = posted.substr(posted.find("\"nonce\":\"") + 9, 64);
    auto received_document = document;
    received_document.replace(received_document.find("forwarded"), 9, "received");
    received_document.replace(received_document.find("\"draftRevision\":3"), 17, "\"draftRevision\":9007199254740991");
    received_document.replace(received_document.find("draft-01"), 8, std::string(96, 'r'));
    auto received = reply(received_document); received.replace(received.find(nonce), 64, received_nonce);
    assert(capture.accept(received, origin));
    assert(capture.report()->documents[0].status == "received");
    assert(capture.report()->documents[0].draft_revision == nstu::exam::programming::kMaximumDraftRevision);
    // Browser Received is still refused when the native owner's durable check fails.
    assert(!capture.preserve([](const auto&) { return false; }));
    assert(capture.cancel(post));
    capture.stop();
    // Every revocation allocation must fail closed, including failures before its identity is parsed.
    std::size_t revocation_allocations = 0;
    for (std::size_t failure = 0; failure <= revocation_allocations; ++failure) {
        assert(capture.start(context, manifest, 12));
        assert(capture.prepare(post, origin));
        const auto current_nonce = posted.substr(posted.find("\"nonce\":\"") + 9, 64);
        auto completion = valid;
        completion.replace(completion.find(nonce), 64, current_nonce);
        auto revocation = reply(document, false);
        revocation.replace(revocation.find(nonce), 64, current_nonce);
        assert(capture.accept(completion, origin));
        const auto previous = *capture.report();
        capture_allocation_test::count = 0;
        capture_allocation_test::fail_at = failure;
        capture_allocation_test::active = true;
        bool accepted = false;
        try { accepted = capture.accept(revocation, origin); } catch (const std::bad_alloc&) {}
        capture_allocation_test::active = false;
        if (failure == 0) {
            revocation_allocations = capture_allocation_test::count;
            assert(accepted && revocation_allocations > 0);
        } else {
            assert(!accepted);
            assert(!capture.is_current_complete(previous));
            assert(!capture.preserve([](const auto&) { return true; }));
            assert(!capture.accept(completion, origin));
            assert(!capture.prepare(post, origin));
        }
        assert(capture.cancel(post));
        capture.stop();
    }
    std::puts("native close capture strict metadata, revocation and ordered cancellation: PASS");
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
    exercise_ordinary_host_policy();
    exercise_close_capture_consumer();
    exercise_contest_lifecycle_clock();
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
