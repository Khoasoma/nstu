#include "nstu/contest_student_session.hpp"
#include "nstu/contest_room_server.hpp"
#include "nstu/multicast.hpp"
#include "nstu/secret_store.hpp"
#include <cassert>
#include <algorithm>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <set>
#include <thread>

namespace {
using namespace nstu;
using namespace nstu::client;
exam::StateRequest context() {
    exam::StateRequest c; c.package_id = "public-cs"; c.package_digest[0] = std::byte{1};
    c.client_id[0] = std::byte{2}; c.session_id[0] = std::byte{3}; c.candidate_id = "candidate-1"; return c;
}
std::string approval() {
    return "{\"status\":\"approved\",\"context\":" + exam::programming::context_json(context()) +
        ",\"recoveryOnly\":false,\"revision\":2,\"serverTime\":1700000000000,\"publicationPolicy\":\"teacher_only\"}";
}
void strict_metadata() {
    const auto approved = parse_student_approval(approval()); assert(approved && !approved->recovery_only);
    assert(exam::encode_state_request(approved->context) == exam::encode_state_request(context()));
    assert(!parse_student_approval(approval() + "x"));
    auto bad = approval(); bad.insert(1, "\"status\":\"approved\","); assert(!parse_student_approval(bad));
    bad = approval(); bad.insert(1, "\"privateCases\":[],"); assert(!parse_student_approval(bad));
    const auto life = parse_student_lifecycle("{\"phase\":\"running\",\"revision\":3,\"serverTime\":1700000001000,\"serverStart\":1700000000000,\"deadline\":1700000060000}");
    assert(life && life->phase == StudentRoomPhase::running);
    assert(!parse_student_lifecycle("{\"phase\":\"running\",\"revision\":3,\"serverTime\":1,\"serverStart\":0,\"deadline\":0}"));
    StudentLifecycle before{StudentRoomPhase::joining, 2, 1700000000000, 0, 0};
    assert(student_lifecycle_extends(before, *life)); auto changed = *life;
    --changed.revision; assert(!student_lifecycle_extends(*life, changed));
    changed = *life; ++changed.deadline; ++changed.revision; assert(!student_lifecycle_extends(*life, changed));
    changed = *life; changed.phase = StudentRoomPhase::closed; ++changed.revision; assert(student_lifecycle_extends(*life, changed));
    assert(!student_lifecycle_extends(changed, *life));
    const StudentLifecycle observed_closed{StudentRoomPhase::closed, 0, 0, 0, 0}; assert(!student_lifecycle_extends(observed_closed, *life));
    const contest_room::Realm realm{1, "teacher", std::string(32, '1'), std::string(32, '2')};
    const auto standings = "{\"roomId\":\"" + realm.room_id + "\",\"roundId\":\"" + realm.round_id +
        "\",\"revision\":3,\"generatedTime\":1700000000000,\"freshness\":\"current\",\"rows\":[{\"rank\":1,\"displayName\":\"Nguyễn\",\"solved\":2,\"penaltyMinutes\":41}]}";
    assert(parse_student_standings(standings, realm)); auto foreign = realm; foreign.round_id[0] = '3';
    assert(!parse_student_standings(standings, foreign));
    bad = standings; bad.insert(1, "\"source\":\"secret\","); assert(!parse_student_standings(bad, realm));
}
void durable_capture() {
    exam::programming::Request request; request.context = context(); request.operation = exam::programming::Operation::draft;
    request.request_id = "draft-1"; request.problem_id = "a"; request.language = "cpp20"; request.draft_revision = 9; request.source = "int main() {}";
    assert(exam::programming::validate_request(request));
    ExamCloseCaptureReport report; report.context = context(); report.generation = 1; report.nonce = std::string(64, 'a'); report.complete = true; report.local_saved = true;
    report.documents.push_back({"a", 1, "cpp20", 9, "", "draft-1", "received"});
    constexpr char hex[] = "0123456789abcdef"; const auto digest = security::sha256(std::as_bytes(std::span(request.source))); assert(digest);
    for (const auto b : *digest) { const auto value = std::to_integer<unsigned>(b); report.documents[0].source_digest += hex[value >> 4u]; report.documents[0].source_digest += hex[value & 15u]; }
    std::vector<exam::programming::Request> durable{request}; std::map<std::string, exam::programming::Request> pending{{request.request_id, request}};
    assert(student_capture_preserved(report, context(), durable, pending));
    assert(!student_capture_preserved(report, context(), {}, pending));
    auto bad = report; ++bad.documents[0].draft_revision; assert(!student_capture_preserved(bad, context(), durable, pending));
    bad = report; bad.documents[0].source_digest[0] ^= 1; assert(!student_capture_preserved(bad, context(), durable, pending));
    bad = report; bad.complete = false; assert(!student_capture_preserved(bad, context(), durable, pending));
    auto newer = request; ++newer.draft_revision; newer.request_id = "draft-2"; newer.source += '\n'; durable = {newer};
    assert(!student_capture_preserved(report, context(), durable, pending));
    durable = {request}; pending[request.request_id].source += '\n'; assert(!student_capture_preserved(report, context(), durable, pending));
    bad = report; bad.documents.push_back(bad.documents.front()); assert(!student_capture_preserved(bad, context(), durable, {}));
    bad = report; bad.nonce[0] = 'z'; assert(!student_capture_preserved(bad, context(), durable, {}));
}
}

namespace {
using namespace nstu::server;
using Clock = std::chrono::steady_clock;
ImmutableRoomState pins() {
    ImmutableRoomState p; p.room_id = std::string(32, '1'); p.round_id = std::string(32, '2');
    p.owner_id = "teacher-01"; p.package_id = "cs-package"; p.package_digest[0] = std::byte{1};
    p.grading_digest[0] = std::byte{4}; p.runtime_identity = std::string(64, 'a'); p.duration_seconds = 3600; return p;
}
contest_room::Realm realm() { return {1, "teacher-01", std::string(32, '1'), std::string(32, '2')}; }
std::string string_field(std::string_view body, std::string_view field) {
    const auto key = "\"" + std::string(field) + "\":\""; const auto pos = body.find(key); assert(pos != std::string_view::npos);
    const auto start = pos + key.size(); const auto end = body.find('"', start); assert(end != std::string_view::npos); return std::string(body.substr(start, end - start));
}
// Explicit memory fixtures; real TCP/CNG/AEAD does not prove durability/judging.
struct MemoryFixture {
    std::mutex mutex; std::condition_variable changed;
    std::vector<std::byte> profile; std::vector<std::string> members;
    std::string phase = "Draft", source_reply, source_body;
    std::map<std::string, std::string> source_replies;
    std::uint64_t revision = 1;
    std::uint64_t server_time = 1700000000000, server_start = 0, deadline = 0;
    bool source_blocked = false, source_seen = false, fail_save = false, state_blocked = false, state_seen = false;
    bool prepare_blocked = false, prepare_seen = false, save_blocked = false, save_seen = false;
    bool approve_blocked = false, approve_seen = false;
    bool member_prepare_blocked = false, member_prepare_seen = false, fail_member_prepare = false;
    std::string state_json(std::string request_id = {}) {
        std::string roster = "[";
        for (std::size_t i = 0; i < members.size(); ++i) { if (i) roster += ','; roster += members[i]; } roster += ']';
        std::string s = "{\"programmingVersion\":1,\"roomId\":\"" + std::string(32, '1') + "\",\"roundId\":\"" + std::string(32, '2') +
            "\",\"ownerId\":\"teacher-01\",\"packageId\":\"cs-package\",\"packageDigestHex\":\"01" + std::string(62, '0') +
            "\",\"gradingDigestHex\":\"04" + std::string(62, '0') + "\",\"runtimeIdentity\":\"" + std::string(64, 'a') +
            "\",\"publicationPolicy\":\"teacher_only\",\"durationSeconds\":3600,\"state\":\"" + phase + "\",\"revision\":" +
            std::to_string(revision) + ",\"serverStartUnixMilliseconds\":" + std::to_string(server_start) +
            ",\"deadlineUnixMilliseconds\":" + std::to_string(deadline) + ",\"serverTime\":" + std::to_string(server_time) + ",\"members\":" + roster;
        if (!request_id.empty()) s += ",\"requestId\":\"" + request_id + "\"";
        return s + '}';
    }
    ProgrammingBroker::Transport transport() {
        return [this](std::string_view route, std::string_view body) -> std::optional<std::string> {
            std::unique_lock lock(mutex);
            if (route == "/v1/contest-rooms/state") { state_seen = true; changed.notify_all(); changed.wait(lock, [&] { return !state_blocked; }); return state_json(); }
            if (route == "/v1/contest-rooms/create") return state_json();
            if (route == "/v1/sessions") {
                prepare_seen = true; changed.notify_all(); changed.wait(lock, [&] { return !prepare_blocked; });
                const auto end = body.find(",\"programmingVersion\""); assert(end != std::string_view::npos);
                return std::string(body.substr(0, end)) + ",\"programmingVersion\":1,\"status\":\"prepared\"}"; }
            if (route == "/v1/contest-rooms/members") {
                const auto start = body.find("\"context\":{"); assert(start != std::string_view::npos); const auto end = body.find('}', start); assert(end != std::string_view::npos);
                const auto context = std::string(body.substr(start + 10, end - start - 9)); const auto action = string_field(body, "action");
                if (action == "approve") {
                    approve_seen = true; changed.notify_all(); changed.wait(lock, [&] { return !approve_blocked; });
                    members.push_back("{\"context\":" + context + ",\"displayName\":\"" + string_field(body, "displayName") +
                    "\",\"prepared\":false,\"approved\":true,\"memberVersion\":1}"); phase = "Joining"; }
                else if (action == "remove") std::erase_if(members, [&](const std::string& m) { return m.find(context) != std::string::npos; });
                else if (action == "prepare") {
                    member_prepare_seen = true; changed.notify_all(); changed.wait(lock, [&] { return !member_prepare_blocked; });
                    if (fail_member_prepare) return std::nullopt;
                    const auto found = std::find_if(members.begin(), members.end(), [&](const std::string& m) { return m.find(context) != std::string::npos; });
                    assert(found != members.end());
                    const auto pos = found->find("\"prepared\":false"); assert(pos != std::string::npos);
                    found->replace(pos, 16, "\"prepared\":true");
                    const auto version = found->find("\"memberVersion\":1"); assert(version != std::string::npos);
                    found->replace(version, 17, "\"memberVersion\":2");
                }
                else assert(false);
                ++revision; return state_json();
            }
            if (route == "/v1/programming") {
                source_seen = true; source_body = body; changed.notify_all(); changed.wait(lock, [&] { return !source_blocked; });
                const auto reply = source_replies.find(string_field(body, "requestId"));
                if (reply != source_replies.end()) return reply->second;
                return source_reply.empty() ? std::nullopt : std::optional{source_reply};
            }
            if (route == "/v1/validate-context") {
                auto response = std::string(body); response.pop_back(); return response + ",\"status\":\"known\",\"serverTime\":" + std::to_string(server_time) + '}';
            }
            if (route == "/v1/contest-rooms/begin") return std::nullopt;
            if (route == "/v1/contest-rooms/stop") { phase = "Closed"; ++revision; return state_json(string_field(body, "requestId")); }
            assert(false); return std::nullopt;
        };
    }
    ContestRoomServerConfig config(std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
        ContestRoomServerConfig c; c.room = pins(); c.title = "Luyện tập"; c.teacher_label = "Giáo viên"; c.tcp_port = 0; c.udp_port = 0;
        c.pairing_timeout = timeout; c.peer_timeout = std::chrono::seconds(10);
        c.memory_fixture = RoomMemoryPersistenceFixture{
            [this]() -> std::optional<std::vector<std::byte>> { std::scoped_lock lock(mutex); return profile; },
            [this](std::span<const std::byte> data) {
                std::unique_lock lock(mutex); save_seen = true; changed.notify_all(); changed.wait(lock, [&] { return !save_blocked; });
                if (fail_save) return false;
                profile.assign(data.begin(), data.end()); return true; }};
        return c;
    }
};

bool wait_until(const std::function<bool()>& predicate) {
    const auto deadline = Clock::now() + std::chrono::seconds(6);
    while (Clock::now() < deadline) { if (predicate()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    return predicate();
}
struct StudentMemory {
    std::mutex mutex;
    std::vector<std::byte> profile;
    std::vector<std::byte> remembered_rooms;
    std::vector<exam::programming::Request> requests;
    bool fail_save = false, verify = true;
    StudentMemoryFixture fixture() {
        return {
            [this]() -> std::optional<std::vector<std::byte>> { std::scoped_lock lock(mutex); return profile; },
            [this](std::span<const std::byte> bytes) { std::scoped_lock lock(mutex); if (fail_save) return false; profile.assign(bytes.begin(), bytes.end()); return true; },
            [this] { std::scoped_lock lock(mutex); return requests; },
            [this](const exam::programming::Request& request) {
                std::scoped_lock lock(mutex); if (fail_save) return false;
                for (auto& saved : requests) {
                    if (saved.request_id == request.request_id) return exam::programming::encode_request(saved) == exam::programming::encode_request(request);
                    if (request.operation == exam::programming::Operation::draft && saved.operation == request.operation && saved.problem_id == request.problem_id && saved.language == request.language) {
                        if (request.draft_revision < saved.draft_revision || (request.draft_revision == saved.draft_revision && request.source != saved.source)) return false;
                        saved = request; return true;
                    }
                }
                if (requests.size() >= 256) return false;
                requests.push_back(request); return true;
            },
            [this](const contest_package::PackageOffer& offer, std::span<const std::byte> archive) -> std::optional<ExamPackageReport> {
                std::scoped_lock lock(mutex); if (!verify || archive.size() != offer.archive_byte_count || security::sha256(archive) != contest_package::digest_from_hex(offer.archive_digest)) return {};
                ExamPackageReport report; report.digest_hex = offer.content_digest; return report;
            },
            [this]() -> std::optional<std::vector<std::byte>> { std::scoped_lock lock(mutex); return remembered_rooms; },
            [this](std::span<const std::byte> wire) { std::scoped_lock lock(mutex); if (fail_save) return false; remembered_rooms.assign(wire.begin(), wire.end()); return true; }};
    }
    ContestStudentConfig config(std::uint16_t port) {
        ContestStudentConfig c; c.realm = realm(); c.endpoint = {"127.0.0.1", port}; c.display_name = "Nguyễn"; c.memory_fixture = fixture(); c.pairing_timeout = std::chrono::seconds(3); return c;
    }
};
void admit(ContestStudentSession& student, ContestRoomServer& teacher) {
    assert(wait_until([&] { return student.snapshot().phase == StudentPhase::confirm_teacher && !teacher.snapshot().pending.empty(); }));
    const auto pending = teacher.snapshot().pending.front(); assert(student.snapshot().sas == pending.sas);
    assert(!student.confirm_sas("0000000")); assert(student.confirm_sas(pending.sas)); assert(teacher.approve(pending.peer_id, pending.sas));
}
ExamCloseCaptureReport capture_for(const exam::programming::Request& request) {
    ExamCloseCaptureReport report; report.context = request.context; report.generation = 1; report.nonce = std::string(64, 'a'); report.complete = true; report.local_saved = true;
    std::string digest; constexpr char digits[] = "0123456789abcdef"; const auto hash = security::sha256(std::as_bytes(std::span(request.source))); assert(hash);
    for (const auto b : *hash) { const auto n = std::to_integer<unsigned>(b); digest += digits[n >> 4u]; digest += digits[n & 15u]; }
    report.documents.push_back({request.problem_id, request.problem_revision, request.language, request.draft_revision, digest, request.request_id, "forwarded"}); return report;
}
std::string saved_reply(const exam::programming::Request& request) {
    auto reply = exam::programming::unavailable_response_json(request, "memory_fixture");
    const auto unavailable = reply.find("\"status\":\"unavailable\""); assert(unavailable != std::string::npos);
    reply.replace(unavailable, 22, "\"status\":\"saved\""); reply.pop_back();
    reply += ",\"sourceDigest\":\"" + capture_for(request).documents.front().source_digest + "\"}";
    assert(exam::programming::parse_response_json(reply, &request)); return reply;
}
void loopback_session() {
    MemoryFixture backend; ProgrammingBroker broker{{"https://judge.example.test", std::string(32, 's')}, backend.transport()}; ContestRoomServer teacher{broker};
    auto teacher_config = backend.config(); RoomMemoryArchiveFixture archive; archive.archive.resize(7 * contest_package::kChunkDataBytes + 13);
    for (std::size_t i = 0; i < archive.archive.size(); ++i) archive.archive[i] = static_cast<std::byte>(i % 251);
    teacher_config.archive_fixture = archive; assert(teacher.start(teacher_config));
    StudentMemory memory; ContestStudentSession student; assert(student.start(memory.config(teacher.snapshot().tcp_port))); admit(student, teacher);
    assert(wait_until([&] { return student.snapshot().package_verified; }));
    assert(wait_until([&] { return !teacher.snapshot().room->members.empty() && teacher.snapshot().room->members.front().prepared; }));
    assert(student.snapshot().phase == StudentPhase::prepared && student.snapshot().received_bytes == archive.archive.size());
    { std::scoped_lock lock(memory.mutex); const auto remembered = decode_student_remembered_rooms(memory.remembered_rooms); assert(remembered && remembered->size() == 1 && remembered->front().realm == realm()); }
    assert(!student.snapshot().live_standings && !student.request_standings()); assert(!student.host_options());
    const auto issued = *student.snapshot().context;
    exam::programming::Request draft; draft.context = issued; draft.operation = exam::programming::Operation::draft; draft.request_id = "student-draft";
    draft.problem_id = "sum"; draft.language = "cpp20"; draft.draft_revision = 1; draft.source = "int main() {}";
    { std::scoped_lock lock(backend.mutex); backend.source_reply = saved_reply(draft); }
    auto foreign = draft; foreign.context.candidate_id = "foreign"; assert(!student.queue_programming(foreign));
    auto run = draft; run.operation = exam::programming::Operation::run; run.sample_id = "sample-1"; assert(!student.queue_programming(run));
    assert(student.queue_programming(draft)); assert(wait_until([&] { return student.snapshot().durable_requests == 1; }));
    assert(wait_until([&] { std::scoped_lock lock(backend.mutex); return backend.source_seen; }));
    assert(wait_until([&] { const auto replies = student.take_replies(); return std::any_of(replies.begin(), replies.end(), [&](const auto& reply) { return reply.request_id == draft.request_id && reply.status == "saved"; }); }));
    assert(student.snapshot().durable_requests == 1);
    auto conflict = draft; conflict.source += '\n'; assert(!student.queue_programming(conflict));
    const auto original = student.snapshot().context; student.disconnect(); assert(wait_until([&] { return !student.snapshot().connected; }));
    auto newer = draft; newer.request_id = "student-draft-newer"; newer.draft_revision = 2; newer.source += '\n';
    { std::scoped_lock lock(memory.mutex); memory.fail_save = true; }
    assert(student.queue_programming(newer)); assert(wait_until([&] { return !student.snapshot().error.empty(); }));
    const std::map<std::string, exam::programming::Request> newer_pending{{newer.request_id, newer}};
    assert(!student.preserve_capture(capture_for(newer), newer_pending));
    { std::scoped_lock lock(memory.mutex); memory.fail_save = false; }
    assert(wait_until([&] { std::scoped_lock lock(memory.mutex); return memory.requests.front().request_id == newer.request_id; }));
    assert(wait_until([&] { return student.preserve_capture(capture_for(newer), newer_pending); }));
    assert(!student.preserve_capture(capture_for(draft), {}));
    auto close_callbacks = student.host_callbacks(); close_callbacks.capture_updated(capture_for(newer)); assert(close_callbacks.before_close(newer_pending));
    auto failed_capture = capture_for(newer); failed_capture.complete = false; close_callbacks.capture_updated(failed_capture); assert(!close_callbacks.before_close(newer_pending));
    close_callbacks.capture_updated(std::nullopt); assert(!close_callbacks.before_close(newer_pending));
    assert(student.reconnect());
    assert(wait_until([&] { return student.snapshot().connected && student.snapshot().phase == StudentPhase::prepared; }));
    assert(exam::encode_state_request(*student.snapshot().context) == exam::encode_state_request(*original));
    assert(teacher.finish("close-student")); assert(wait_until([&] { return student.snapshot().phase == StudentPhase::closed; }));
    assert(student.snapshot().recovery_only); assert(!student.queue_programming(run));
    auto ready = draft; ready.operation = exam::programming::Operation::ready; ready.problem_id.clear(); ready.language.clear(); ready.source.clear(); ready.draft_revision = 0;
    assert(!student.queue_programming(ready));
    student.disconnect(); assert(wait_until([&] { return !student.snapshot().connected; })); assert(student.reconnect());
    assert(wait_until([&] { return student.snapshot().connected && student.snapshot().phase == StudentPhase::closed; }));
    assert(student.snapshot().lifecycle.deadline == 0 && student.snapshot().durable_requests == 1);
    const auto callbacks = student.host_callbacks(); student.stop(); assert(!callbacks.enqueue_programming_request(draft));
    ContestStudentSession restarted; assert(restarted.start(memory.config(teacher.snapshot().tcp_port)));
    assert(wait_until([&] { return restarted.snapshot().connected && restarted.snapshot().phase == StudentPhase::closed; }));
    assert(exam::encode_state_request(*restarted.snapshot().context) == exam::encode_state_request(issued)); assert(restarted.snapshot().durable_requests == 1);
    assert(!restarted.snapshot().package_verified && !restarted.host_options());
    restarted.stop(); teacher.stop(); broker.stop();
}
void common_clock_and_native_configuration() {
    ContestStudentSession invalid; ContestStudentConfig bad; assert(!invalid.start(bad));
    bad.realm = realm(); bad.display_name = "Student"; bad.endpoint = {"teacher.example.test", 47002}; assert(!invalid.start(bad));
    bad.endpoint.address = "127.0.0.1"; bad.user_root = "relative-user-root"; assert(invalid.start(bad));
    assert(wait_until([&] { return invalid.snapshot().phase == StudentPhase::failed; })); assert(!invalid.snapshot().context); invalid.stop();
    bad.user_root = std::filesystem::absolute(std::filesystem::path(__FILE__));
    assert(invalid.start(bad)); assert(wait_until([&] { return invalid.snapshot().phase == StudentPhase::failed; }));
    assert(std::filesystem::is_regular_file(bad.user_root) && !invalid.snapshot().context); invalid.stop();
    assert(!load_student_remembered_rooms(bad.user_root));
    MemoryFixture backend; ProgrammingBroker broker{{"https://judge.example.test", std::string(32, 's')}, backend.transport()}; ContestRoomServer teacher{broker};
    auto config = backend.config(); config.archive_fixture = RoomMemoryArchiveFixture{std::vector<std::byte>(123, std::byte{5})}; assert(teacher.start(config));
    StudentMemory memory; ContestStudentSession student; assert(student.start(memory.config(teacher.snapshot().tcp_port))); admit(student, teacher);
    assert(wait_until([&] { return student.snapshot().package_verified && teacher.snapshot().room->members.front().prepared; }));
    // Broker lifecycle fixture only: this is not a ready judge or judged Begin.
    { std::scoped_lock lock(backend.mutex); backend.phase = "Running"; ++backend.revision; backend.server_time += 1000; backend.server_start = backend.server_time; backend.deadline = backend.server_start + 3600000; }
    assert(teacher.refresh()); assert(wait_until([&] { return student.snapshot().phase == StudentPhase::running; }));
    const auto running = student.snapshot(); std::this_thread::sleep_for(std::chrono::milliseconds(30)); assert(student.snapshot().server_time > running.server_time);
    student.disconnect(); assert(wait_until([&] { return !student.snapshot().connected; })); assert(student.reconnect());
    assert(wait_until([&] { return student.snapshot().connected && student.snapshot().phase == StudentPhase::running; }));
    assert(student.snapshot().lifecycle.server_start == running.lifecycle.server_start && student.snapshot().lifecycle.deadline == running.lifecycle.deadline);
    assert(student.snapshot().server_time >= running.server_time);
    assert(teacher.finish("stop-running-student")); assert(wait_until([&] { return student.snapshot().phase == StudentPhase::closed; }));
    assert(student.snapshot().lifecycle.server_start == running.lifecycle.server_start && student.snapshot().lifecycle.deadline == running.lifecycle.deadline);
    student.stop(); teacher.stop(); broker.stop();
}
void immutable_receipt_replay() {
    MemoryFixture backend; ProgrammingBroker broker{{"https://judge.example.test", std::string(32, 's')}, backend.transport()}; ContestRoomServer teacher{broker};
    auto config = backend.config(); config.archive_fixture = RoomMemoryArchiveFixture{std::vector<std::byte>(123, std::byte{5})}; assert(teacher.start(config));
    StudentMemory memory; ContestStudentSession student; assert(student.start(memory.config(teacher.snapshot().tcp_port))); admit(student, teacher);
    assert(wait_until([&] { return student.snapshot().package_verified && teacher.snapshot().room->members.front().prepared; }));
    // Lifecycle/receipt memory fixture only; no submitted source is judged.
    const auto original_time = backend.server_time;
    { std::scoped_lock lock(backend.mutex); backend.phase = "Running"; ++backend.revision; backend.server_start = original_time; backend.deadline = original_time + 3600000; }
    assert(teacher.refresh()); assert(wait_until([&] { return student.snapshot().phase == StudentPhase::running; }));
    exam::programming::Request draft; draft.context = *student.snapshot().context; draft.operation = exam::programming::Operation::draft;
    draft.request_id = "immutable-draft"; draft.problem_id = "sum"; draft.language = "cpp20"; draft.draft_revision = 1; draft.source = "int main() {}";
    auto submit = draft; submit.operation = exam::programming::Operation::submit; submit.request_id = "immutable-submit";
    auto late = submit; late.request_id = "immutable-late";
    auto finalize = draft; finalize.operation = exam::programming::Operation::finalize; finalize.request_id = "immutable-finalize";
    finalize.problem_id.clear(); finalize.language.clear(); finalize.source.clear(); finalize.draft_revision = 0; finalize.receipt_ids = {std::string(32, 'a')};
    const std::vector<exam::programming::Request> requests{draft, submit, late, finalize};
    const std::vector<std::string> statuses{"saved", "received", "late", "finalized"};
    std::map<std::string, std::string> receipts;
    for (std::size_t i = 0; i < requests.size(); ++i) {
        auto reply = exam::programming::unavailable_response_json(requests[i], "memory_fixture");
        const auto status = reply.find("\"status\":\"unavailable\""); assert(status != std::string::npos);
        reply.replace(status, 22, "\"status\":\"" + statuses[i] + "\"");
        const auto timestamp = reply.find("\"serverTime\":") + std::string_view("\"serverTime\":").size();
        reply.replace(timestamp, reply.find_first_of(",}", timestamp) - timestamp, std::to_string(original_time)); reply.pop_back();
        if (requests[i].operation != exam::programming::Operation::finalize) reply += ",\"sourceDigest\":\"" + capture_for(requests[i]).documents.front().source_digest + "\"";
        if (i > 0) reply += ",\"receiptId\":\"" + std::string(32, 'a' + static_cast<char>(i - 1)) + "\"";
        reply += '}'; assert(exam::programming::parse_response_json(reply, &requests[i])); receipts[requests[i].request_id] = reply;
    }
    { std::scoped_lock lock(backend.mutex); backend.source_replies = receipts; }
    const auto take_all = [&](ContestStudentSession& session) {
        std::set<std::string> seen;
        assert(wait_until([&] {
            for (const auto& reply : session.take_replies()) {
                const auto found = receipts.find(reply.request_id); assert(found != receipts.end());
                assert(reply.server_time == original_time && reply.json == exam::programming::parse_response_json(found->second)->json);
                seen.insert(reply.request_id);
            }
            return seen.size() == requests.size();
        }));
    };
    for (const auto& request : requests) assert(student.queue_programming(request));
    take_all(student); assert(student.snapshot().durable_requests == requests.size());
    { std::scoped_lock lock(backend.mutex); ++backend.revision; backend.server_time += 1000; }
    assert(teacher.refresh()); assert(wait_until([&] { return student.snapshot().lifecycle.server_time == original_time + 1000; }));
    const auto newer = student.snapshot(); student.disconnect(); assert(wait_until([&] { return !student.snapshot().connected; })); assert(student.reconnect());
    take_all(student);
    assert(student.snapshot().connected && student.snapshot().lifecycle.server_time == newer.lifecycle.server_time && student.snapshot().server_time >= newer.server_time);
    auto ready = draft; ready.operation = exam::programming::Operation::ready; ready.request_id = "stale-readiness";
    ready.problem_id.clear(); ready.language.clear(); ready.source.clear(); ready.draft_revision = 0;
    auto stale = exam::programming::unavailable_response_json(ready, "memory_fixture");
    auto pos = stale.find("\"status\":\"unavailable\""); assert(pos != std::string::npos); stale.replace(pos, 22, "\"status\":\"ready\"");
    pos = stale.find("\"runnerReady\":false"); assert(pos != std::string::npos); stale.replace(pos, 19, "\"runnerReady\":true");
    pos = stale.find("\"serverTime\":") + std::string_view("\"serverTime\":").size(); stale.replace(pos, stale.find_first_of(",}", pos) - pos, std::to_string(original_time));
    assert(exam::programming::parse_response_json(stale, &ready));
    { std::scoped_lock lock(backend.mutex); backend.source_replies[ready.request_id] = stale; }
    assert(student.queue_programming(ready));
    assert(wait_until([&] { return student.snapshot().error == "teacher room message failed validation; source retained"; }));
    assert(student.snapshot().lifecycle.server_time == newer.lifecycle.server_time && student.take_replies().empty());
    student.disconnect(); { std::scoped_lock lock(backend.mutex); backend.source_replies.erase(ready.request_id); }
    assert(student.reconnect()); take_all(student);
    { std::scoped_lock lock(backend.mutex); backend.server_time += 1000; }
    assert(teacher.finish("close-immutable-replays")); assert(wait_until([&] { return student.snapshot().phase == StudentPhase::closed; }));
    student.disconnect(); assert(wait_until([&] { return !student.snapshot().connected; })); assert(student.reconnect()); take_all(student);
    const auto closed = student.snapshot(); assert(closed.recovery_only && closed.lifecycle.server_time == original_time + 2000);
    auto run = draft; run.operation = exam::programming::Operation::run; run.sample_id = "sample-1"; assert(!student.queue_programming(run));
    student.stop();
    ContestStudentSession restarted; assert(restarted.start(memory.config(teacher.snapshot().tcp_port))); take_all(restarted);
    assert(restarted.snapshot().phase == StudentPhase::closed && restarted.snapshot().lifecycle.server_time == closed.lifecycle.server_time && restarted.snapshot().durable_requests == requests.size());
    restarted.stop();
    for (const bool foreign_context : {false, true}) {
        MemoryFixture hostile_backend; ProgrammingBroker hostile_broker{{"https://judge.example.test", std::string(32, 's')}, hostile_backend.transport()}; ContestRoomServer hostile_teacher{hostile_broker};
        auto hostile_config = hostile_backend.config(); hostile_config.archive_fixture = config.archive_fixture; assert(hostile_teacher.start(hostile_config));
        StudentMemory hostile_memory; ContestStudentSession rejected; assert(rejected.start(hostile_memory.config(hostile_teacher.snapshot().tcp_port))); admit(rejected, hostile_teacher);
        assert(wait_until([&] { return rejected.snapshot().package_verified && hostile_teacher.snapshot().room->members.front().prepared; }));
        { std::scoped_lock lock(hostile_backend.mutex); ++hostile_backend.revision; hostile_backend.server_time += 1000; }
        assert(hostile_teacher.refresh()); assert(wait_until([&] { return rejected.snapshot().lifecycle.server_time == original_time + 1000; }));
        auto expected = draft; expected.context = *rejected.snapshot().context;
        auto bad = saved_reply(expected);
        const auto timestamp = bad.find("\"serverTime\":") + std::string_view("\"serverTime\":").size(); bad.replace(timestamp, bad.find_first_of(",}", timestamp) - timestamp, std::to_string(original_time));
        const auto field = foreign_context ? expected.context.candidate_id : capture_for(expected).documents.front().source_digest;
        const auto offset = bad.find(field); assert(offset != std::string::npos); bad[offset] = foreign_context ? 'x' : (bad[offset] == 'a' ? 'b' : 'a');
        assert(!exam::programming::parse_response_json(bad, &expected));
        { std::scoped_lock lock(hostile_backend.mutex); hostile_backend.source_blocked = true; }
        assert(rejected.queue_programming(expected));
        assert(wait_until([&] { std::scoped_lock lock(hostile_backend.mutex); return hostile_backend.source_seen; }));
        const auto peers = hostile_teacher.snapshot().peers;
        const auto peer = std::find_if(peers.begin(), peers.end(), [](const auto& value) { return value.connected; }); assert(peer != peers.end());
        const auto payload = std::as_bytes(std::span(bad));
        assert(hostile_teacher.queue_message(peer->peer_id, {contest_room::Kind::programming_reply, 1, {payload.begin(), payload.end()}}));
        assert(wait_until([&] { return rejected.snapshot().error == "teacher room message failed validation; source retained"; }));
        assert(rejected.snapshot().durable_requests == 1 && rejected.snapshot().lifecycle.server_time == original_time + 1000 && rejected.take_replies().empty()); rejected.stop();
        { std::scoped_lock lock(hostile_backend.mutex); hostile_backend.source_blocked = false; hostile_backend.changed.notify_all(); }
        hostile_teacher.stop(); hostile_broker.stop();
    }
    teacher.stop(); broker.stop();
}
void remembered_room_codec_and_dpapi() {
    std::vector<StudentRememberedRoom> rooms;
    constexpr char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; ++i) { auto selected = realm(); selected.room_id[30] = digits[i >> 4u]; selected.room_id[31] = digits[i & 15u]; rooms.push_back({selected, {"127.0.0.1", 47002}, "Phòng luyện tập"}); }
    const auto wire = encode_student_remembered_rooms(rooms); assert(!wire.empty()); const auto decoded = decode_student_remembered_rooms(wire);
    assert(decoded && decoded->size() == 32 && decoded->front().realm == rooms.front().realm && decoded->front().display_label == rooms.front().display_label);
    auto malformed = wire; malformed.push_back(std::byte{0}); assert(!decode_student_remembered_rooms(malformed));
    malformed = wire; malformed.pop_back(); assert(!decode_student_remembered_rooms(malformed));
    auto duplicate = rooms; duplicate.back() = duplicate.front(); assert(encode_student_remembered_rooms(duplicate).empty());
    auto extra = rooms; extra.push_back(rooms.front()); assert(encode_student_remembered_rooms(extra).empty());
    auto invalid = rooms; invalid.front().endpoint.address = "teacher.example.test"; assert(encode_student_remembered_rooms(invalid).empty());
    invalid = rooms; invalid.front().display_label += '\n'; assert(encode_student_remembered_rooms(invalid).empty());
    invalid = rooms; invalid.front().realm.teacher_id = std::string(97, 'x'); assert(encode_student_remembered_rooms(invalid).empty());
    const auto empty = encode_student_remembered_rooms({}); assert(!empty.empty() && decode_student_remembered_rooms(empty)->empty());
    constexpr char purpose[] = "NSTU-STUDENT-REMEMBERED-ROOMS-V1";
    const auto entropy = std::as_bytes(std::span(purpose, sizeof(purpose) - 1)); const auto protected_wire = security::protect_user_secret(wire, entropy); assert(!protected_wire.empty());
    assert(security::unprotect_user_secret(protected_wire, entropy) == wire);
    constexpr char other[] = "NSTU-STUDENT-PROFILE-V1"; assert(security::unprotect_user_secret(protected_wire, std::as_bytes(std::span(other, sizeof(other) - 1))).empty());
}
void cancel_and_verify_refusal() {
    for (const bool cancel : {true, false}) {
        MemoryFixture backend; ProgrammingBroker broker{{"https://judge.example.test", std::string(32, 's')}, backend.transport()}; ContestRoomServer teacher{broker};
        auto config = backend.config(); config.archive_fixture = RoomMemoryArchiveFixture{std::vector<std::byte>(123, std::byte{7})}; assert(teacher.start(config));
        StudentMemory memory; memory.verify = false; ContestStudentSession student; assert(student.start(memory.config(teacher.snapshot().tcp_port)));
        if (cancel) { assert(wait_until([&] { return student.snapshot().phase == StudentPhase::confirm_teacher; })); student.cancel_pairing();
            assert(wait_until([&] { return student.snapshot().phase == StudentPhase::disconnected; })); assert(teacher.snapshot().room->members.empty()); }
        else { admit(student, teacher); assert(wait_until([&] { return !student.snapshot().error.empty(); })); assert(!student.snapshot().package_verified);
            assert(!teacher.snapshot().room->members.front().prepared); }
        student.stop(); teacher.stop(); broker.stop();
        if (cancel) { std::scoped_lock lock(memory.mutex); assert(memory.remembered_rooms.empty()); }
    }
}
}
int main() {
    net::WinsockRuntime runtime; assert(runtime.ready()); remembered_room_codec_and_dpapi(); strict_metadata(); durable_capture(); loopback_session(); cancel_and_verify_refusal(); common_clock_and_native_configuration(); immutable_receipt_replay();
    std::cout << "student strict metadata and exact native durable capture: PASS\n";
    std::cout << "student real TCP/CNG/AEAD pairing, receiver final ACK, restart and Closed recovery: PASS (explicit memory persistence/archive verifier fixtures)\n";
}
