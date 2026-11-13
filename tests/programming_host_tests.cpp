#include "nstu/exam_host.hpp"
#include "nstu/programming.hpp"

#include <cassert>
#include <string>
#include <utility>

namespace {
const std::string manifest = R"({"id":"cs-package","title":"Programming","durationSeconds":3600,"contentMode":"computer_science","programmingVersion":1,"sessionMode":"exam","languages":["cpp20","python3"],"questions":[{"id":"sum","type":"programming","revision":2,"prompt":"Add integers","samples":[{"id":"sample-1","input":"1 2\n","output":"3\n"}],"timeLimitMs":1000,"memoryLimitMiB":64}]})";

nstu::exam::StateRequest context() {
    nstu::exam::StateRequest result;
    result.package_id = "cs-package";
    result.candidate_id = "candidate-01";
    result.package_digest[0] = std::byte{1};
    result.client_id[0] = std::byte{2};
    result.session_id[0] = std::byte{3};
    return result;
}

std::string browser_request() {
    return "{\"type\":\"exam_programming_request\",\"context\":" +
        nstu::exam::programming::context_json(context()) +
        R"(,"operation":"run","requestId":"run-01","problemId":"sum","problemRevision":2,"language":"cpp20","draftRevision":1,"source":"int main() {}","sampleId":"sample-1"})";
}

void replace_once(std::string& text, const std::string& from, const std::string& to) {
    const auto at = text.find(from);
    assert(at != std::string::npos);
    text.replace(at, from.size(), to);
}
}

int main() {
    using namespace nstu::client;
    using namespace nstu::exam::programming;
    static_assert(static_cast<unsigned>(nstu::protocol::CommandType::exam_programming_request) == 70);
    static_assert(static_cast<unsigned>(AgentMessageType::exam_programming_request) == 45);
    const auto local_wire = encode_agent_message({AgentMessageType::exam_programming_request,
                                                  {std::byte{1}}});
    assert(decode_agent_message(local_wire));
    ExamBridge bridge;
    assert(bridge.publish({AgentMessageType::exam_programming_response, {std::byte{1}}}));
    assert(bridge.try_pop()->type == AgentMessageType::exam_programming_response);
    assert(validate_programming_manifest(manifest));
    ExamLockdownPolicy policy;
    assert(extract_exam_lockdown_policy(manifest, policy));
    assert(policy.programming_mode == "exam" && policy.duration_seconds == 3600);
    auto malformed_policy = manifest;
    replace_once(malformed_policy, "\"programmingVersion\":1", "\"programmingVersion\":2");
    assert(!extract_exam_lockdown_policy(malformed_policy, policy));
    assert(policy.programming_mode.empty());
    assert(extract_exam_lockdown_policy(R"({"durationSeconds":60})", policy));
    assert(policy.programming_mode.empty() && policy.duration_seconds == 60);
    constexpr std::string_view verified = R"({"version":1,"status":"verified","code":"policy_verified"})";
    assert(validate_programming_policy_probe_json(verified, 0));
    assert(validate_programming_policy_probe_json(std::string(verified) + "\r\n", 0));
    assert(!validate_programming_policy_probe_json(verified, 1));
    for (const std::string_view invalid : {
        R"({"version":1,"status":"unavailable","code":"physical_not_qualified"})",
        R"({"version":2,"status":"verified","code":"policy_verified"})",
        R"({"version":"1","status":"verified","code":"policy_verified"})",
        R"({"version":1,"status":"verified","code":"policy_verified","extra":true})",
        R"({"version":1,"status":"verified","code":"policy_verified","code":"policy_verified"})",
        R"({"version":1,"status":"verified","code":"policy_verified"}{})",
        R"({"version":1,"status":"verified","code":"probe_unavailable"})"}) {
        assert(!validate_programming_policy_probe_json(invalid, 0));
    }
    assert(!validate_programming_policy_probe_json(std::string(4097, ' '), 0));
    for (const auto& change : {std::pair{std::string{"\"programmingVersion\":1"}, std::string{"\"programmingVersion\":2"}},
                               std::pair{std::string{"\"type\":\"programming\""}, std::string{"\"type\":\"mcq\""}},
                               std::pair{std::string{"\"timeLimitMs\":1000"}, std::string{"\"hiddenCases\":[],\"timeLimitMs\":1000"}},
                               std::pair{std::string{"\"memoryLimitMiB\":64"}, std::string{"\"memoryLimitMiB\":\"64\""}}}) {
        auto bad = manifest;
        replace_once(bad, change.first, change.second);
        assert(!validate_programming_manifest(bad));
    }
    const auto request = parse_browser_programming_request(browser_request(), manifest, context());
    assert(request && request->problem_revision == 2 && request->source == "int main() {}");
    std::map<std::string, Request> pending;
    auto autosave = *request;
    autosave.operation = Operation::draft;
    autosave.sample_id.clear();
    for (std::uint64_t revision = 1; revision <= 256; ++revision) {
        autosave.request_id = "draft-" + std::to_string(revision);
        autosave.draft_revision = revision;
        assert(track_browser_programming_request(pending, autosave));
        assert(pending.size() == 1);
    }
    auto equal = autosave; equal.request_id = "draft-same";
    assert(track_browser_programming_request(pending, equal));
    assert(pending.size() == 1 && pending.contains(equal.request_id));
    auto conflict = equal; conflict.source += "different";
    assert(!track_browser_programming_request(pending, conflict));
    conflict.request_id = "draft-conflict";
    assert(!track_browser_programming_request(pending, conflict));
    auto older = equal; older.request_id = "draft-old"; --older.draft_revision;
    assert(!track_browser_programming_request(pending, older));
    assert(pending.size() == 1 && pending.at(equal.request_id).source == equal.source);
    auto other_language = equal; other_language.request_id = "draft-python"; other_language.language = "python3";
    assert(track_browser_programming_request(pending, other_language));
    assert(pending.size() == 2);
    for (unsigned index = 0; index < 126; ++index) {
        auto immutable = *request; immutable.operation = Operation::submit; immutable.sample_id.clear();
        immutable.request_id = "submit-" + std::to_string(index);
        assert(track_browser_programming_request(pending, immutable));
    }
    auto full = *request; full.request_id = "run-full";
    assert(!track_browser_programming_request(pending, full));
    equal.request_id = "draft-full"; ++equal.draft_revision;
    assert(track_browser_programming_request(pending, equal));
    assert(pending.size() == 128 && pending.contains("submit-0") && pending.contains("draft-python"));
    for (const auto& change : {std::pair{std::string{"\"candidateId\":\"candidate-01\""}, std::string{"\"candidateId\":\"other\""}},
                               std::pair{std::string{"\"problemRevision\":2"}, std::string{"\"problemRevision\":3"}},
                               std::pair{std::string{"\"sampleId\":\"sample-1\""}, std::string{"\"sampleId\":\"private-case\""}},
                               std::pair{std::string{"\"source\":\"int main() {}\""}, std::string{"\"source\":\"int main() {}\",\"compilerFlags\":\"-include secret\""}}}) {
        auto bad = browser_request();
        replace_once(bad, change.first, change.second);
        assert(!parse_browser_programming_request(bad, manifest, context()));
    }
    auto forged_ready = "{\"type\":\"exam_programming_request\",\"context\":" + context_json(context()) +
        R"(,"operation":"ready","requestId":"ready-01","workstationPolicyReady":true})";
    assert(!parse_browser_programming_request(forged_ready, manifest, context()));
    auto forged_recovery = browser_request();
    forged_recovery.insert(forged_recovery.size() - 1, ",\"recoveryOnly\":true");
    assert(!parse_browser_programming_request(forged_recovery, manifest, context()));
    const auto response = unavailable_response_json(*request, "runner_unavailable");
    assert(parse_response_json(response, &*request));
    for (const std::string& extra : {std::string{",\"score\":100"}, std::string{",\"hiddenCases\":[]"},
                                     std::string{",\"verdict\":\"accepted\""}, std::string{",\"unknown\":true"}}) {
        auto bad = response;
        bad.insert(bad.size() - 1, extra);
        assert(!parse_response_json(bad, &*request));
    }
    auto mismatched = response;
    replace_once(mismatched, "candidate-01", "other");
    assert(!parse_response_json(mismatched, &*request));
    assert(!parse_response_json(std::string(nstu::exam::kMaximumExamPayloadBytes + 1, 'x'), &*request));
    return 0;
}
