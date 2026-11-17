#include "nstu/programming_broker.hpp"
#include "nstu/multicast.hpp"
#include "nstu/network.hpp"

#include <winsock2.h>

#include <atomic>
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <string>
#include <thread>

namespace {
using namespace nstu::server;
ImmutableRoomState pins() {
    ImmutableRoomState p;
    p.room_id = std::string(32, '1'); p.round_id = std::string(32, '2');
    p.owner_id = "teacher-01"; p.package_id = "cs-package";
    p.package_digest[0] = std::byte{1}; p.grading_digest[0] = std::byte{4};
    p.runtime_identity = std::string(64, 'a'); p.duration_seconds = 3600;
    return p;
}
nstu::exam::StateRequest member_context() {
    nstu::exam::StateRequest c;
    c.package_id = "cs-package"; c.package_digest[0] = std::byte{1};
    c.client_id[0] = std::byte{2}; c.session_id[0] = std::byte{3}; c.candidate_id = "candidate-01";
    return c;
}
std::string state_json(std::string members = "[]") {
    return "{\"programmingVersion\":1,\"roomId\":\"" + std::string(32, '1') +
        "\",\"roundId\":\"" + std::string(32, '2') + "\",\"ownerId\":\"teacher-01\","
        "\"packageId\":\"cs-package\",\"packageDigestHex\":\"01" + std::string(62, '0') +
        "\",\"gradingDigestHex\":\"04" + std::string(62, '0') +
        "\",\"runtimeIdentity\":\"" + std::string(64, 'a') +
        "\",\"publicationPolicy\":\"teacher_only\",\"durationSeconds\":3600,\"state\":\"" +
        (members == "[]" ? std::string("Draft") : std::string("Joining")) + "\","
        "\"revision\":1,\"serverStartUnixMilliseconds\":0,\"deadlineUnixMilliseconds\":0,"
        "\"serverTime\":1700000000000,\"members\":" + members + '}';
}
std::string member_json() {
    return "{\"context\":" + nstu::exam::programming::context_json(member_context()) +
        ",\"displayName\":\"Nguyễn\",\"prepared\":false,\"approved\":true,\"memberVersion\":1}";
}
std::string replace(std::string s, std::string_view from, std::string_view to) {
    const auto pos = s.find(from); assert(pos != std::string::npos);
    s.replace(pos, from.size(), to); return s;
}
void room_proxy_tests() {
    const ProgrammingBrokerConfig config{"https://judge.example.test", std::string(32, 's')};
    const auto p = pins(); const auto c = member_context();
    std::string response = state_json(); std::string last_body, last_route;
    std::thread::id transport_thread;
    ProgrammingBroker b(config, [&](std::string_view route, std::string_view body) -> std::optional<std::string> {
        last_route = route; last_body = body; transport_thread = std::this_thread::get_id(); return response;
    });
    auto state = b.create_room(p); assert(state && state->immutable.owner_id == p.owner_id);
    assert(transport_thread != std::this_thread::get_id());
    assert(last_route == "/v1/contest-rooms/create" && last_body.find("teacher_only") != std::string::npos);
    assert(last_body.find("ownerId") == std::string::npos && last_body.find("durationSeconds") == std::string::npos);
    auto unknown_runtime = p; unknown_runtime.runtime_identity.clear();
    assert(b.create_room(unknown_runtime)->immutable.runtime_identity == p.runtime_identity);
    assert(!b.room_state(unknown_runtime));
    response = replace(state_json("[" + member_json() + "]"), "\"revision\":1", "\"revision\":2");
    state = b.approve_member(*state, c, "Nguyễn");
    assert(state && state->members.size() == 1 && state->members[0].display_name == "Nguyễn");
    assert(last_body.find("\"context\":" + nstu::exam::programming::context_json(c)) != std::string::npos);
    assert(!b.prepare_member(*state, c, 1)); // An ACK must return the requested member's new version/readiness.
    response = replace(response, "\"prepared\":false", "\"prepared\":true");
    response = replace(response, "\"memberVersion\":1", "\"memberVersion\":2");
    response = replace(response, "\"revision\":2", "\"revision\":3");
    const auto prepared = b.prepare_member(*state, c, 1);
    assert(prepared); assert(last_body.find("\"action\":\"prepare\"") != std::string::npos);
    assert(!b.remove_member(*state, c, 1));
    response = replace(state_json(), "\"revision\":1", "\"revision\":3");
    response = replace(response, "\"state\":\"Draft\"", "\"state\":\"Joining\"");
    assert(b.remove_member(*state, c, 1)); assert(last_body.find("\"action\":\"remove\"") != std::string::npos);
    response = state_json("[" + replace(member_json(), "\"prepared\":false", "\"prepared\":true") + "]");
    response.pop_back(); response += ",\"requestId\":\"begin-01\"}";
    assert(!b.begin_room(*state, "begin-01")); // A successful Begin must be Running.
    response = replace(response, "\"state\":\"Joining\"", "\"state\":\"Running\"");
    response = replace(response, "\"serverStartUnixMilliseconds\":0", "\"serverStartUnixMilliseconds\":1700000000000");
    response = replace(response, "\"deadlineUnixMilliseconds\":0", "\"deadlineUnixMilliseconds\":1700003600000");
    assert(!b.begin_room(*prepared, "begin-01")); // A pinned but stale revision1 ACK must fail.
    response = replace(response, "\"revision\":1", "\"revision\":4");
    response = replace(response, "\"memberVersion\":1", "\"memberVersion\":2");
    const auto running_json = response;
    const auto running = b.begin_room(*prepared, "begin-01");
    assert(running && running->revision == 4); assert(last_route == "/v1/contest-rooms/begin");
    response = replace(running_json, "\"revision\":4", "\"revision\":5");
    response = replace(response, "1700000000000", "1700000001000");
    response = replace(response, "1700003600000", "1700003601000");
    assert(!b.begin_room(*running, "begin-01"));
    response = replace(running_json, "Nguyễn", "Other student");
    assert(!b.begin_room(*prepared, "begin-01"));
    response = running_json;
    response = replace(response, "\"state\":\"Running\"", "\"state\":\"Closed\"");
    response = replace(response, "begin-01", "stop-01");
    assert(!b.stop_room(*running, "stop-01")); // Stop must advance revision4 exactly once.
    response = replace(response, "\"revision\":4", "\"revision\":5");
    const auto stopped_json = response;
    response = replace(response, "\"serverStartUnixMilliseconds\":1700000000000", "\"serverStartUnixMilliseconds\":1700000001000");
    response = replace(response, "1700003600000", "1700003601000");
    assert(!b.stop_room(*running, "stop-01"));
    response = stopped_json;
    const auto stopped = b.stop_room(*running, "stop-01"); assert(stopped && stopped->revision == 5);
    response = replace(running_json, "\"revision\":4", "\"revision\":6");
    assert(!b.begin_room(*stopped, "begin-01"));
    for (const auto& invalid : std::vector<std::string>{
            state_json("[" + member_json() + "," + member_json() + "]"),
            state_json("[" + replace(member_json(), "\"approved\":true", "\"approved\":false") + "]"),
            state_json("[" + replace(member_json(), "\"prepared\":false", "\"prepared\":0") + "]"),
            state_json("[" + replace(member_json(), "\"memberVersion\":1", "\"memberVersion\":1.0") + "]"),
            state_json("[" + replace(member_json(), "candidate-01", std::string(129, 'x')) + "]"),
            state_json("[" + replace(member_json(), "\"displayName\":\"Nguyễn\"", "\"displayName\":\"" + std::string(65, 'x') + "\"") + "]"),
            state_json("[" + replace(member_json(), "\"prepared\":false", "\"prepared\":false,\"source\":\"SOURCE_CANARY\"") + "]"),
            state_json("[" + replace(member_json(), "\"candidateId\":", "\"role\":\"operator\",\"candidateId\":") + "]"),
            state_json("[" + replace(member_json(), "cs-package", "other-package") + "]"),
            state_json("[" + replace(member_json(), "01" + std::string(62, '0'), "09" + std::string(62, '0')) + "]"),
            state_json("[" + replace(member_json(), "\"candidateId\":\"candidate-01\"", "\"candidateId\":\"candidate-01\",\"candidateId\":\"other\"") + "]"),
            state_json("[" + replace(member_json(), "Nguyễn", "\\ud800") + "]"),
            replace(state_json(), "\"revision\":1", "\"revision\":true"),
            replace(state_json(), "\"serverTime\":1700000000000", "\"serverTime\":9007199254740992"),
            replace(state_json(), "\"state\":\"Draft\"", "\"state\":\"Running\""),
            replace(state_json(), "\"state\":\"Draft\"", "\"state\":\"Prepared\""),
            replace(state_json(), "teacher-01", "other-teacher"),
            replace(state_json(), "teacher_only", "live"),
            replace(state_json(), std::string(64, 'a'), std::string(64, 'b')),
            replace(state_json(), "\"durationSeconds\":3600", "\"durationSeconds\":60"),
            replace(state_json(), "\"programmingVersion\":1", "\"programmingVersion\":1,\"programmingVersion\":1"),
            replace(state_json(), "\"revision\":1", "\"revision\":1,\"source\":\"SOURCE_CANARY\""),
            replace(state_json(), "\"ownerId\":\"teacher-01\"", "\"ownerId\":\"\xc0\xaf\""),
            state_json() + "garbage", std::string(131073, ' ')}) {
        response = invalid; assert(!b.room_state(p));
    }
    std::string many_members = "[";
    for (unsigned i = 0; i < 65; ++i) {
        auto next = c; next.client_id[0] = static_cast<std::byte>(i + 1); next.session_id[0] = static_cast<std::byte>(i + 1);
        next.candidate_id = "candidate-" + std::to_string(i);
        if (i != 0) many_members += ',';
        many_members += "{\"context\":" + nstu::exam::programming::context_json(next) +
            ",\"displayName\":\"Student\",\"prepared\":false,\"approved\":true,\"memberVersion\":1}";
        if (i == 63) {
            response = state_json(many_members + ']');
            const auto full = b.room_state(p); assert(full && full->members.size() == 64);
        }
    }
    response = state_json(many_members + ']'); assert(!b.room_state(p));
    response = state_json("[" + member_json() + "]");
    auto wrong = c; wrong.package_digest[0] = std::byte{9};
    const auto before = last_body;
    assert(!b.approve_member(*state, wrong, "Student")); assert(last_body == before);
    assert(!b.prepare_member(*state, c, 0)); assert(!b.stop_room(*state, "bad request"));
    const std::string snapshot = "{\"roomId\":\"" + p.room_id + "\",\"roundId\":\"" + p.round_id +
        "\",\"revision\":2,\"generatedTime\":1700000000000,\"freshness\":\"current\","
        "\"rows\":[{\"rank\":1,\"displayName\":\"Student\",\"solved\":2,\"penaltyMinutes\":10}]}";
    response = snapshot; auto scores = b.room_standings(p);
    assert(scores && scores->rows.size() == 1 && scores->rows[0].solved == 2);
    assert(last_route == "/v1/contest-rooms/standings");
    assert(last_body == "{\"programmingVersion\":1,\"roomId\":\"" + p.room_id + "\",\"roundId\":\"" + p.round_id + "\"}");
    for (const auto freshness : {"pending", "unavailable"}) {
        response = replace(snapshot, "current", freshness); assert(b.room_standings(p));
    }
    for (const auto& invalid : std::vector<std::string>{
            replace(snapshot, "\"rank\":1", "\"rank\":0"), replace(snapshot, "\"rank\":1", "\"rank\":2"),
            replace(snapshot, "\"solved\":2", "\"solved\":65"), replace(snapshot, "\"solved\":2", "\"solved\":2.0"),
            replace(snapshot, "\"penaltyMinutes\":10", "\"penaltyMinutes\":-1"),
            replace(snapshot, "\"penaltyMinutes\":10", "\"penaltyMinutes\":9007199254740992"),
            replace(snapshot, "\"freshness\":\"current\"", "\"freshness\":\"fresh\""),
            replace(snapshot, "\"generatedTime\":1700000000000", "\"generatedTime\":0"),
            replace(snapshot, "\"rows\":", "\"source\":\"SOURCE_CANARY\",\"rows\":"),
            replace(snapshot, "\"rank\":1", "\"rank\":1,\"privateResult\":100"),
            replace(snapshot, "\"rank\":1", "\"rank\":1,\"rank\":1"),
            replace(snapshot, p.room_id, std::string(32, '3'))}) {
        response = invalid; assert(!b.room_standings(p));
    }
    response = "{\"status\":\"registered\",\"packageId\":\"cs-package\",\"packageDigestHex\":\"01" + std::string(62, '0') + "\"}";
    const std::string manifest = "{\"id\":\"cs-package\",\"title\":\"CS\",\"durationSeconds\":3600,\"contentMode\":\"computer_science\",\"programmingVersion\":1,\"sessionMode\":\"contest\",\"languages\":[\"cpp20\",\"python3\"],\"questions\":[{\"id\":\"sum\",\"type\":\"programming\",\"revision\":1,\"prompt\":\"Sum\",\"timeLimitMs\":1000,\"memoryLimitMiB\":64,\"samples\":[{\"id\":\"sample-1\",\"input\":\"1\",\"output\":\"1\"}]}]}";
    assert(b.register_package(p.package_id, p.package_digest, manifest)); assert(last_route == "/v1/packages");
    assert(!b.register_package(p.package_id, p.package_digest, replace(manifest, "\"prompt\":\"Sum\"", "\"prompt\":\"Sum\",\"source\":\"SOURCE_CANARY\"")));
    assert(!b.register_package(p.package_id, p.package_digest, replace(manifest, "\"input\":\"1\"", "\"input\":\"1\",\"input\":\"2\"")));
    assert(!b.register_package(p.package_id, p.package_digest, replace(manifest, "\"title\":\"CS\"", "\"title\":\"CS\",\"id||title\":\"composite\"")));
    response = "{\"status\":\"registered\",\"gradingDigestHex\":\"04" + std::string(62, '0') + "\"}";
    assert(b.register_grading(p.package_id, p.package_digest, p.grading_digest,
        "[{\"id\":\"sum\",\"revision\":1,\"cases\":[{\"input\":\"PRIVATE_CANARY\",\"output\":\"1\"}]}]"));
    assert(last_route == "/v1/grading");
    const std::string valid_grading = "[{\"id\":\"sum\",\"revision\":1,\"cases\":[{\"input\":\"1\",\"output\":\"1\"}]}]";
    response = replace(response, "\"status\":\"registered\"", "\"status\":\"registered\",\"source\":\"SOURCE_CANARY\"");
    assert(!b.register_grading(p.package_id, p.package_digest, p.grading_digest, valid_grading));
    assert(!b.register_grading(p.package_id, p.package_digest, p.grading_digest, replace(valid_grading, "\"input\":\"1\"", "\"input\":\"1\",\"operatorToken\":\"SOURCE_CANARY\"")));
    response = "{\"status\":\"registered\",\"gradingDigestHex\":\"04" + std::string(62, '0') + "\"}";
    const std::string private_prefix = "{\"programmingVersion\":1,\"packageId\":\"cs-package\",\"packageDigestHex\":\"01" +
        std::string(62, '0') + "\",\"gradingDigestHex\":\"04" + std::string(62, '0') + "\",\"problems\":";
    std::string full_private = "[{\"id\":\"sum\",\"revision\":1,\"cases\":[";
    for (unsigned i = 0; i < 63; ++i) {
        if (i != 0) full_private += ',';
        full_private += "{\"input\":\"" + std::string(65536, 'f') + "\",\"output\":\"\"}";
    }
    full_private += ",{\"input\":\"TAIL\",\"output\":\"\"}]}]";
    const auto padding = 4u * 1024u * 1024u - (private_prefix.size() + full_private.size() + 1) + 4;
    assert(padding <= 65536);
    full_private.replace(full_private.find("TAIL"), 4, padding, 't');
    assert(b.register_grading(p.package_id, p.package_digest, p.grading_digest, full_private));
    assert(last_body.size() == 4u * 1024u * 1024u && last_body == private_prefix + full_private + '}');
    const auto accepted_private = last_body;
    full_private.insert(full_private.find(std::string(16, 't')), 1, 't');
    assert(!b.register_grading(p.package_id, p.package_digest, p.grading_digest, full_private));
    assert(last_body == accepted_private);
    b.stop();
    std::mutex grading_mutex; std::condition_variable grading_changed;
    bool grading_started = false, release_grading = false, ready_completed = false;
    nstu::exam::programming::Request ready;
    ready.context = c; ready.operation = nstu::exam::programming::Operation::ready; ready.request_id = "independent-ready";
    ProgrammingBroker independent(config, [&](std::string_view route, std::string_view) -> std::optional<std::string> {
        if (route == "/v1/programming") return nstu::exam::programming::unavailable_response_json(ready, "runner_unavailable");
        assert(route == "/v1/grading");
        std::unique_lock lock(grading_mutex); grading_started = true; grading_changed.notify_all();
        grading_changed.wait(lock, [&] { return release_grading; }); return response;
    });
    auto first_registration = std::async(std::launch::async, [&] {
        return independent.register_grading(p.package_id, p.package_digest, p.grading_digest, valid_grading);
    });
    {
        std::unique_lock lock(grading_mutex);
        assert(grading_changed.wait_for(lock, std::chrono::seconds(3), [&] { return grading_started; }));
    }
    assert(!independent.register_grading(p.package_id, p.package_digest, p.grading_digest, valid_grading));
    assert(independent.submit(ready, [&](std::string reply) {
        assert(nstu::exam::programming::parse_response_json(reply, &ready));
        std::scoped_lock lock(grading_mutex); ready_completed = true; grading_changed.notify_all();
    }));
    {
        std::unique_lock lock(grading_mutex);
        assert(grading_changed.wait_for(lock, std::chrono::seconds(3), [&] { return ready_completed; }));
        release_grading = true; grading_changed.notify_all();
    }
    assert(first_registration.get()); independent.stop();
}

void local_http_tests() {
    nstu::net::WinsockRuntime runtime; assert(runtime.ready());
    for (const unsigned scenario : {5u, 0u, 1u, 2u, 3u, 4u}) {
        nstu::net::TcpSocket listener(static_cast<std::uintptr_t>(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)));
        assert(listener.is_open());
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        assert(bind(static_cast<SOCKET>(listener.native_handle()), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
        assert(listen(static_cast<SOCKET>(listener.native_handle()), 2) == 0);
        std::string captured;
        const auto port = listener.local_port(); assert(port != 0);
        std::thread server([&] {
            assert(listener.wait_readable(5000));
            auto peer = listener.accept(); assert(peer.is_open()); assert(peer.set_io_timeouts(5000, 5000));
            std::array<std::byte, 4096> buffer{};
            for (;;) {
                const auto count = peer.receive(buffer);
                if (scenario == 3 && count <= 0) break;
                assert(count > 0);
                captured.append(reinterpret_cast<const char*>(buffer.data()), static_cast<std::size_t>(count));
                if (scenario == 3) break; // TLS must fail without a plaintext credential-bearing fallback.
                const auto headers = captured.find("\r\n\r\n");
                if (headers == std::string::npos) continue;
                const auto length = captured.find("Content-Length: "); assert(length != std::string::npos);
                const auto bytes = std::stoul(captured.substr(length + 16));
                if (captured.size() >= headers + 4 + bytes) break;
            }
            if (scenario == 4 || scenario == 5) {
                const std::string headers = scenario == 5 ? "HTTP/1.1 200 OK\r\nX-Drip: " :
                    "HTTP/1.1 200 OK\r\nContent-Length: 4096\r\nConnection: close\r\n\r\n";
                assert(peer.send_all(std::as_bytes(std::span(headers))) == static_cast<int>(headers.size()));
                const std::array<std::byte, 1> drip{std::byte{' '}};
                for (unsigned i = 0; i < (scenario == 5 ? 600u : 470u); ++i) {
                    if (peer.send_all(drip) != 1) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                peer.close(); return;
            }
            const auto body = scenario == 2 ? std::string(65537, ' ') : state_json();
            std::string reply = scenario == 1 ?
                "HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:" + std::to_string(port) + "/secret-path\r\nContent-Length: 0\r\nConnection: close\r\n\r\n" :
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
            if (scenario != 3) assert(peer.send_all(std::as_bytes(std::span(reply))) == static_cast<int>(reply.size()));
            if (scenario == 3) listener.close();
            peer.close();
            if (scenario == 1) assert(!listener.wait_readable(200));
        });
        const ProgrammingBrokerConfig config{std::string(scenario == 3 ? "https://" : "http://") +
            "127.0.0.1:" + std::to_string(port), std::string(32, 'e')};
        ProgrammingBroker b(config);
        const auto before = std::chrono::steady_clock::now();
        const auto reply = b.room_state(pins());
        assert(static_cast<bool>(reply) == (scenario == 0));
        if (scenario == 4 || scenario == 5) {
            assert(std::chrono::steady_clock::now() - before >= std::chrono::seconds(40));
            assert(std::chrono::steady_clock::now() - before < std::chrono::seconds(50));
        }
        else assert(std::chrono::steady_clock::now() - before < std::chrono::seconds(10));
        b.stop();
        assert(std::chrono::steady_clock::now() - before < std::chrono::seconds(50));
        server.join();
        assert(std::chrono::steady_clock::now() - before < std::chrono::seconds(50));
        if (scenario != 3) {
            assert(captured.starts_with("POST /v1/contest-rooms/state HTTP/1.1\r\n"));
            assert(captured.find("Authorization: Bearer " + std::string(32, 'e') + "\r\n") != std::string::npos);
            assert(captured.find("SOURCE_CANARY") == std::string::npos);
        } else assert(captured.find(std::string(32, 'e')) == std::string::npos);
    }
}
}

int main() {
    using namespace nstu::server;
    using namespace nstu::exam::programming;
    const ProgrammingBrokerConfig config{"https://judge.example.test:8443", std::string(32, 's')};
    assert(validate_programming_broker_config(config));
    assert(validate_programming_broker_config({"http://127.0.0.1:8080", std::string(32, 's')}));
    for (const std::string_view url : {"http://127.0.0.2:8080",
            "http://127.0.0.1.example.test", "http://user@127.0.0.1:8080", "http://127.0.0.1:8080?proxy=secret"}) {
        assert(!validate_programming_broker_config({std::string(url), std::string(32, 's')}));
    }
    for (const std::string& url : {std::string{"http://judge.example.test"}, std::string{"https://user:pass@judge.test"},
                                   std::string{"https://judge.test/private"}, std::string{"file:///judge"},
                                   std::string{"https://judge.test?token=secret"}}) {
        assert(!validate_programming_broker_config({url, std::string(32, 's')}));
    }
    assert(!validate_programming_broker_config({config.url, "short"}));
    Request request;
    request.context.package_id = "cs-package";
    request.context.candidate_id = "candidate-01";
    request.context.package_digest[0] = std::byte{1};
    request.context.client_id[0] = std::byte{2};
    request.context.session_id[0] = std::byte{3};
    request.operation = Operation::draft;
    request.request_id = "draft-01";
    request.problem_id = "sum";
    request.problem_revision = 2;
    request.language = "cpp20";
    request.draft_revision = 1;
    request.source = "int main() {}";
    auto cancel = request;
    cancel.operation = Operation::cancel;
    cancel.request_id = "cancel-01";
    cancel.draft_revision = 0;
    cancel.source.clear();
    cancel.target_run_id = "owned-run-01";
    auto ready = request;
    ready.operation = Operation::ready;
    ready.request_id = "ready-01";
    ready.problem_id.clear();
    ready.problem_revision = 1;
    ready.language.clear();
    ready.draft_revision = 0;
    ready.source.clear();
    std::mutex mutex;
    std::condition_variable changed;
    unsigned running = 0;
    unsigned completed = 0;
    unsigned maximum_running = 0;
    bool release = false;
    bool cancelled = false;
    ProgrammingBroker broker(config, [&](std::string_view route, std::string_view body) -> std::optional<std::string> {
        assert(route == "/v1/programming");
        assert(body.find("candidate-01") != std::string_view::npos);
        if (body.find("\"operation\":\"cancel\"") != std::string_view::npos) {
            return unavailable_response_json(cancel, "runner_unavailable");
        }
        std::unique_lock lock(mutex);
        ++running;
        maximum_running = std::max(maximum_running, running);
        changed.notify_all();
        changed.wait(lock, [&] { return release; });
        --running;
        return unavailable_response_json(request, "runner_unavailable");
    });
    const auto completion = [&](std::string json) {
        assert(parse_response_json(json, &request));
        std::scoped_lock lock(mutex);
        ++completed;
        changed.notify_all();
    };
    assert(broker.submit(request, completion));
    assert(broker.submit(request, completion));
    {
        std::unique_lock lock(mutex);
        assert(changed.wait_for(lock, std::chrono::seconds(3), [&] { return running == 2; }));
    }
    assert(broker.submit(cancel, [&](std::string json) {
        assert(parse_response_json(json, &cancel));
        std::scoped_lock lock(mutex);
        cancelled = true;
        changed.notify_all();
    }));
    {
        std::unique_lock lock(mutex);
        assert(changed.wait_for(lock, std::chrono::seconds(3), [&] { return cancelled; }));
        assert(running == 2);
    }
    for (unsigned index = 0; index < 64; ++index) assert(broker.submit(request, completion));
    assert(!broker.submit(request, completion));
    {
        std::scoped_lock lock(mutex);
        release = true;
        changed.notify_all();
    }
    {
        std::unique_lock lock(mutex);
        assert(changed.wait_for(lock, std::chrono::seconds(5), [&] { return completed == 66; }));
        assert(maximum_running == 2);
    }
    broker.stop();
    assert(!broker.submit(request, completion));
    auto recovery = request;
    recovery.operation = Operation::draft;
    recovery.problem_id = "sum";
    recovery.problem_revision = 2;
    recovery.language = "cpp20";
    recovery.draft_revision = 1;
    recovery.source = "int main() {}";
    unsigned routes = 0;
    bool recovered = false;
    ProgrammingBroker replay(config, [&](std::string_view route, std::string_view body) -> std::optional<std::string> {
        if (route == "/v1/validate-context") {
            assert(routes == 0);
            ++routes;
            auto known = context_json(recovery.context);
            known.pop_back();
            return known + ",\"programmingVersion\":1,\"status\":\"known\",\"serverTime\":1700000000000}";
        }
        assert(route == "/v1/programming" && routes == 1);
        assert(body.find("\"recoveryOnly\":true") != std::string_view::npos);
        ++routes;
        return unavailable_response_json(recovery, "runner_unavailable");
    });
    assert(!replay.submit(ready, [](auto) {}, true));
    assert(replay.submit(recovery, [&](std::string json) {
        assert(parse_response_json(json, &recovery));
        std::scoped_lock lock(mutex);
        recovered = true;
        changed.notify_all();
    }, true));
    {
        std::unique_lock lock(mutex);
        assert(changed.wait_for(lock, std::chrono::seconds(3), [&] { return recovered; }));
    }
    replay.stop();
    assert(routes == 2);
    for (const std::string_view status : {"unknown", "known"}) {
        bool returned = false;
        unsigned calls = 0;
        ProgrammingBroker rejected(config, [&](std::string_view route, std::string_view) -> std::optional<std::string> {
            assert(route == "/v1/validate-context");
            ++calls;
            auto invalid = context_json(recovery.context);
            invalid.pop_back();
            return invalid + ",\"programmingVersion\":1,\"status\":\"" + std::string(status) +
                "\",\"serverTime\":1700000000000,\"privateResult\":100}";
        });
        assert(rejected.submit(recovery, [&](std::string json) {
            const auto response = parse_response_json(json, &recovery);
            assert(response && response->status == "unavailable");
            std::scoped_lock lock(mutex);
            returned = true;
            changed.notify_all();
        }, true));
        {
            std::unique_lock lock(mutex);
            assert(changed.wait_for(lock, std::chrono::seconds(3), [&] { return returned; }));
        }
        rejected.stop();
        assert(calls == 1);
    }
    room_proxy_tests();
    local_http_tests();
    return 0;
}
