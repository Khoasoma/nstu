#include "nstu/contest_room_server.hpp"
#include "nstu/multicast.hpp"
#include <winsock2.h>
#include <windows.h>
#include <wincrypt.h>
#include <cassert>
#include <atomic>
#include <algorithm>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>

namespace {
using namespace nstu;
using namespace nstu::server;
using Clock = std::chrono::steady_clock;
using Kind = contest_room::Kind;
ImmutableRoomState pins() {
    ImmutableRoomState p; p.room_id = std::string(32, '1'); p.round_id = std::string(32, '2');
    p.owner_id = "teacher-01"; p.package_id = "cs-package"; p.package_digest[0] = std::byte{1};
    p.grading_digest[0] = std::byte{4}; p.runtime_identity = std::string(64, 'a'); p.duration_seconds = 3600; return p;
}
contest_room::Realm realm() { return {1, "teacher-01", std::string(32, '1'), std::string(32, '2')}; }
std::string name_envelope(std::string_view name) {
    DWORD size = 0;
    assert(CryptBinaryToStringA(reinterpret_cast<const BYTE*>(name.data()), static_cast<DWORD>(name.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &size));
    std::string s(size, '\0');
    assert(CryptBinaryToStringA(reinterpret_cast<const BYTE*>(name.data()), static_cast<DWORD>(name.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, s.data(), &size));
    s.resize(size); return "nstu-name-v1:" + s;
}
template<class F> bool wait_until(F f) {
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    while (Clock::now() < deadline) { if (f()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(5)); } return f();
}
std::string string_field(std::string_view body, std::string_view field) {
    const auto key = "\"" + std::string(field) + "\":\""; const auto pos = body.find(key); assert(pos != std::string_view::npos);
    const auto start = pos + key.size(); const auto end = body.find('"', start); assert(end != std::string_view::npos); return std::string(body.substr(start, end - start));
}
// Explicit memory fixtures; real TCP/CNG/AEAD does not prove durability/judging.
struct MemoryFixture {
    std::mutex mutex; std::condition_variable changed;
    std::vector<std::byte> profile; std::vector<std::string> members;
    std::string phase = "Draft", source_reply, source_body;
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
            if (route == "/v1/programming") { source_seen = true; source_body = body; changed.notify_all(); changed.wait(lock, [&] { return !source_blocked; }); return source_reply.empty() ? std::nullopt : std::optional{source_reply}; }
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
bool read_exact(net::TcpSocket& socket, std::span<std::byte> out) {
    std::size_t offset = 0; const auto deadline = Clock::now() + std::chrono::seconds(4);
    while (offset < out.size() && Clock::now() < deadline) { if (!socket.wait_readable(20)) continue; const auto n = socket.receive(out.subspan(offset)); if (n <= 0) return false; offset += static_cast<std::size_t>(n); }
    return offset == out.size();
}
std::optional<protocol::TcpFrame> read_frame(net::TcpSocket& socket) {
    std::array<std::byte, 4> length{}; if (!read_exact(socket, length)) return std::nullopt;
    std::uint32_t size = 0; for (unsigned i = 0; i < 4; ++i) size |= std::to_integer<std::uint32_t>(length[i]) << (8u * i);
    if (size < protocol::kCommandHeaderBytes || size > 4096) return std::nullopt;
    std::vector<std::byte> body(size); if (!read_exact(socket, body)) return std::nullopt;
    const auto envelope = protocol::decode_command_header(body); if (!envelope) return std::nullopt;
    return protocol::TcpFrame{*envelope, {body.begin() + protocol::kCommandHeaderBytes, body.end()}};
}
void send_frame(net::TcpSocket& socket, protocol::CommandType kind, std::span<const std::byte> payload) {
    const auto frame = protocol::encode_tcp_frame({1, kind, static_cast<std::uint32_t>(payload.size()), 91}, payload);
    assert(socket.send_all(frame, Clock::now() + std::chrono::seconds(1)) == static_cast<int>(frame.size()));
}
net::TcpSocket connect_preface(ContestRoomServer& teacher, contest_room::Realm selected = realm()) {
    net::TcpSocket s; assert(s.connect_with_timeout("127.0.0.1", teacher.snapshot().tcp_port, 1000)); assert(s.set_io_timeouts(1000, 1000));
    security::Nonce nonce{}; assert(security::generate_random(nonce)); assert(contest_room::send_preface(s, selected, contest_room::Side::candidate, nonce, Clock::now() + std::chrono::seconds(1)));
    if (!contest_room::receive_preface(s, selected, contest_room::Side::teacher, Clock::now() + std::chrono::seconds(2), nonce)) s.close();
    return s;
}
struct PairActor {
    net::TcpSocket socket; pairing::PairingHello hello; pairing::PairingTranscript transcript; pairing::PairingSecrets secrets; std::uint32_t key_id = 0;
    ~PairActor() { pairing::secure_zero(secrets); }
    PairActor(ContestRoomServer& teacher, unsigned identity = 1, std::string envelope = name_envelope("Nguyễn")) {
        socket = connect_preface(teacher); assert(socket.is_open()); auto ephemeral = pairing::EphemeralKeyPair::generate(); assert(ephemeral);
        hello.agreement = ephemeral->agreement(); hello.client_uuid = "PeerUserUUID-" + std::to_string(identity); hello.client_hostname = std::move(envelope);
        hello.client_public_key.assign(ephemeral->public_key().begin(), ephemeral->public_key().end()); assert(security::generate_random(hello.client_nonce));
        send_frame(socket, protocol::CommandType::pairing_hello, pairing::encode_pairing_hello(hello)); const auto frame = read_frame(socket);
        if (!frame) { socket.close(); return; } assert(frame->envelope.type == protocol::CommandType::pairing_offer);
        const auto offer = pairing::decode_pairing_offer(frame->payload); assert(offer); const auto made = pairing::make_transcript(hello, *offer); assert(made); transcript = *made;
        auto shared = ephemeral->agree(offer->server_public_key); assert(shared); const auto derived = contest_room::derive_room_secrets(*shared, realm(), transcript); assert(derived); secrets = *derived; security::secure_zero(*shared);
    }
    void confirm(bool correct = true) { auto tag = pairing::confirmation_tag(secrets, transcript, pairing::ConfirmationRole::client); assert(tag); if (!correct) (*tag)[0] ^= std::byte{1}; send_frame(socket, protocol::CommandType::pairing_confirm, pairing::encode_pairing_confirm({*tag})); }
    bool accept() { const auto f = read_frame(socket); if (!f || f->envelope.type != protocol::CommandType::pairing_accept) return false; const auto a = pairing::decode_pairing_accept(f->payload); assert(a); assert(pairing::verify_confirmation_tag(secrets, transcript, pairing::ConfirmationRole::server, a->server_tag)); key_id = a->key_id; return true; }
    std::uint64_t pending_id(ContestRoomServer& teacher) { assert(wait_until([&] { return !teacher.snapshot().pending.empty(); })); for (const auto& p : teacher.snapshot().pending) if (p.peer_user_uuid == hello.client_uuid) { assert(p.sas == secrets.short_authentication_string && p.display_name == "Nguyễn"); return p.peer_id; } assert(false); return 0; }
    std::optional<control::AuthenticatedSession> handshake(bool aead = true) {
        const auto id = pairing::client_id_from_uuid(hello.client_uuid); assert(id);
        return control::client_handshake(socket, *id, key_id, secrets.enrolled_key, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()), std::chrono::seconds(120), nullptr, aead);
    }
    void approve(ContestRoomServer& teacher) { const auto id = pending_id(teacher); assert(teacher.approve(id, secrets.short_authentication_string)); confirm(); assert(accept()); }
};
struct FixtureRoom {
    MemoryFixture memory; ProgrammingBroker broker{{"https://judge.example.test", std::string(32, 's')}, memory.transport()}; ContestRoomServer server{broker};
    explicit FixtureRoom(std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
        std::string error;
        const bool started = server.start(memory.config(timeout), &error);
        if (!started) std::cerr << error << '\n';
        assert(started);
    }
    ~FixtureRoom() { server.stop(); broker.stop(); }
};
contest_room::RoomChannel authenticated(PairActor& candidate, ContestRoomServer& teacher, bool resume = false) {
    if (resume) candidate.socket = connect_preface(teacher);
    auto session = candidate.handshake(); assert(session && session->aead_enabled);
    contest_room::RoomChannel c(std::move(candidate.socket), std::move(*session), realm(), contest_room::Side::candidate);
    const auto join = c.receive(Clock::now() + std::chrono::seconds(2)); assert(join && join->kind == Kind::join_status);
    const auto life = c.receive(Clock::now() + std::chrono::seconds(2)); assert(life && life->kind == Kind::lifecycle); return c;
}
void sas_rejections() {
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        FixtureRoom t(std::chrono::milliseconds(700)); PairActor c(t.server); const auto id = c.pending_id(t.server);
        if (scenario == 0) assert(t.server.reject(id));
        if (scenario == 1) {
            auto wrong = c.secrets.short_authentication_string;
            wrong[0] = wrong[0] == '0' ? '1' : '0';
            assert(t.server.approve(id, wrong));
        }
        if (scenario == 3) assert(t.server.approve(id, c.secrets.short_authentication_string));
        c.confirm(scenario != 3); assert(!c.accept());
        assert(wait_until([&] { return t.server.snapshot().pending.empty(); })); assert(t.server.snapshot().room->members.empty());
    }
    FixtureRoom t;
    for (const std::string& envelope : {std::string("managed-hostname"), std::string("nstu-name-v1:Tmd1eeG7hW4=\n"), name_envelope("bad\nname"), name_envelope("\xc0\xaf"), name_envelope(std::string(65, 'x'))}) {
        PairActor malformed(t.server, 9, envelope); assert(!malformed.socket.is_open()); assert(t.server.snapshot().pending.empty()); }
    auto wrong = realm(); wrong.round_id = std::string(32, '3'); auto foreign = connect_preface(t.server, wrong); assert(!foreign.is_open());
}
void source_and_recovery() {
    FixtureRoom t; PairActor c(t.server); c.approve(t.server); auto channel = authenticated(c, t.server);
    assert(wait_until([&] { return t.server.snapshot().peers.size() == 1; })); const auto member = t.server.snapshot().room->members.front();
    assert(member.context.client_id == *pairing::client_id_from_uuid(c.hello.client_uuid)); assert(member.context.candidate_id.starts_with("candidate-") && !member.prepared); assert(!t.memory.profile.empty());
    exam::programming::Request request; request.context = member.context; request.operation = exam::programming::Operation::draft; request.request_id = "draft-1";
    request.problem_id = "sum"; request.language = "cpp20"; request.draft_revision = 1; request.source = "int main() {}";
    { std::scoped_lock lock(t.memory.mutex); t.memory.source_reply = exam::programming::unavailable_response_json(request, "runner_unavailable"); t.memory.source_blocked = true; }
    assert(channel.send({Kind::programming_request, 7, exam::programming::encode_request(request)}, Clock::now() + std::chrono::seconds(1)));
    assert(wait_until([&] { std::scoped_lock lock(t.memory.mutex); return t.memory.source_seen; })); const auto start = Clock::now();
    for (unsigned i = 0; i < 100; ++i) (void)t.server.snapshot();
    assert(Clock::now() - start < std::chrono::milliseconds(100));
    assert(channel.send({Kind::heartbeat, 8, {}}, Clock::now() + std::chrono::seconds(1))); const auto heartbeat = channel.receive(Clock::now() + std::chrono::seconds(1)); assert(heartbeat && heartbeat->request_id == 8);
    { std::scoped_lock lock(t.memory.mutex); t.memory.source_blocked = false; t.memory.changed.notify_all(); }
    const auto reply = channel.receive(Clock::now() + std::chrono::seconds(2)); assert(reply && reply->kind == Kind::programming_reply && reply->request_id == 7);
    const auto parsed = exam::programming::parse_response_json({reinterpret_cast<const char*>(reply->payload.data()), reply->payload.size()}, &request); assert(parsed && parsed->status == "unavailable");
    assert(t.memory.source_body.find("int main() {}") != std::string::npos); std::atomic_uint final_ack{0};
    t.server.set_final_ack_handler([&](std::uint64_t, const exam::StateRequest& context, const security::Sha256Digest& digest) { assert(exam::encode_state_request(context) == exam::encode_state_request(member.context)); assert(digest == member.context.package_digest); ++final_ack; });
    assert(channel.send({Kind::prepared, 9, {member.context.package_digest.begin(), member.context.package_digest.end()}}, Clock::now() + std::chrono::seconds(1))); assert(wait_until([&] { return final_ack == 1; })); assert(!t.server.snapshot().room->members.front().prepared);
    assert(t.server.begin("no-ack-begin")); std::this_thread::sleep_for(std::chrono::milliseconds(50)); assert(t.server.snapshot().room->phase == RoomPhase::joining);
    channel.close(); t.server.stop(); assert(t.server.start(t.memory.config())); auto resumed = authenticated(c, t.server, true);
    assert(exam::encode_state_request(t.server.snapshot().room->members.front().context) == exam::encode_state_request(member.context));
    assert(t.server.finish("stop-1"));
    const auto closed_state = wait_until([&] { return t.server.snapshot().room->phase == RoomPhase::closed; });
    if (!closed_state) std::cerr << t.server.snapshot().error << " phase " << static_cast<unsigned>(t.server.snapshot().room->phase) << '\n';
    assert(closed_state);
    assert(resumed.send({Kind::heartbeat, 42, {}}, Clock::now() + std::chrono::seconds(1)));
    std::optional<contest_room::Message> closed_join;
    for (unsigned i = 0; i < 3; ++i) {
        auto message = resumed.receive(Clock::now() + std::chrono::seconds(2)); assert(message);
        if (message->kind == Kind::join_status) { closed_join = std::move(message); break; }
        assert(message->kind == Kind::lifecycle);
    }
    assert(closed_join && closed_join->request_id == 42);
    const std::string closed_payload(reinterpret_cast<const char*>(closed_join->payload.data()), closed_join->payload.size());
    assert(closed_payload.find("\"recoveryOnly\":true") != std::string::npos);
    resumed.close(); t.server.stop(); assert(t.server.start(t.memory.config())); auto recovery = authenticated(c, t.server, true);
    assert(wait_until([&] { return !t.server.snapshot().peers.empty(); })); assert(t.server.snapshot().peers.front().recovery_only);
    request.operation = exam::programming::Operation::run; request.request_id = "closed-run"; request.sample_id = "sample-1";
    assert(recovery.send({Kind::programming_request, 11, exam::programming::encode_request(request)}, Clock::now() + std::chrono::seconds(1))); assert(!recovery.receive(Clock::now() + std::chrono::seconds(2)));
    PairActor closed(t.server, 2); assert(!closed.socket.is_open()); assert(t.server.snapshot().room->members.size() == 1);
}
void foreign_management_and_downgrade() {
    for (unsigned field = 0; field < 5; ++field) {
        FixtureRoom t; PairActor c(t.server); c.approve(t.server); auto channel = authenticated(c, t.server); auto context = t.server.snapshot().room->members.front().context;
        if (field == 0) context.package_id = "foreign-package";
        if (field == 1) context.package_digest[0] ^= std::byte{2};
        if (field == 2) context.client_id[0] ^= std::byte{1};
        if (field == 3) context.session_id[0] ^= std::byte{1};
        if (field == 4) context.candidate_id = "foreign-candidate";
        exam::programming::Request r; r.context = context; r.operation = exam::programming::Operation::ready; r.request_id = "foreign";
        assert(channel.send({Kind::programming_request, 1, exam::programming::encode_request(r)}, Clock::now() + std::chrono::seconds(1))); assert(!channel.receive(Clock::now() + std::chrono::seconds(2))); assert(!t.memory.source_seen);
    }
    for (const bool downgrade : {false, true}) {
        FixtureRoom t; PairActor c(t.server); c.approve(t.server); auto session = c.handshake(!downgrade); assert(session); control::AuthenticatedControlChannel generic(std::move(c.socket), std::move(*session));
        if (!downgrade) { assert(generic.receive()); assert(generic.receive()); assert(generic.send(protocol::CommandType::remote_input, 1, {})); } assert(!generic.receive()); assert(wait_until([&] { return t.server.snapshot().peers.empty(); }));
    }
    FixtureRoom t; PairActor c(t.server); c.approve(t.server); auto channel = authenticated(c, t.server); const auto context = t.server.snapshot().room->members.front().context;
    assert(t.server.remove(context)); assert(wait_until([&] { return t.server.snapshot().room->members.empty(); })); assert(!channel.receive(Clock::now() + std::chrono::seconds(2)));
    c.socket = connect_preface(t.server); assert(!c.handshake()); t.server.stop(); assert(t.server.start(t.memory.config())); c.socket = connect_preface(t.server); assert(!c.handshake());
}
void bounded_and_durability() {
    FixtureRoom t(std::chrono::seconds(10)); std::vector<net::TcpSocket> connections;
    for (unsigned i = 0; i < 64; ++i) { auto connection = connect_preface(t.server); assert(connection.is_open()); connections.push_back(std::move(connection)); }
    auto overflow = connect_preface(t.server); assert(!overflow.is_open()); const auto start = Clock::now(); t.server.stop(); assert(Clock::now() - start < std::chrono::seconds(3));
    for (auto& connection : connections) connection.close();
    assert(!t.server.snapshot().running); assert(t.server.start(t.memory.config()));
    PairActor c(t.server); c.approve(t.server); auto channel = authenticated(c, t.server);
    { std::scoped_lock lock(t.memory.mutex); t.memory.state_seen = false; t.memory.state_blocked = true; }
    assert(t.server.refresh()); assert(wait_until([&] { std::scoped_lock lock(t.memory.mutex); return t.memory.state_seen; }));
    unsigned accepted = 0; for (unsigned i = 0; i < 129; ++i) if (t.server.refresh()) ++accepted; assert(accepted == kMaximumRoomOperations); assert(t.server.snapshot().running);
    { std::scoped_lock lock(t.memory.mutex); t.memory.state_blocked = false; t.memory.changed.notify_all(); }
    channel.close(); t.server.stop();
    FixtureRoom failure; { std::scoped_lock lock(failure.memory.mutex); failure.memory.fail_save = true; }
    PairActor rejected(failure.server); const auto id = rejected.pending_id(failure.server); assert(failure.server.approve(id, rejected.secrets.short_authentication_string)); rejected.confirm(); assert(!rejected.accept()); assert(failure.server.snapshot().room->members.empty());
    { std::scoped_lock lock(failure.memory.mutex); assert(failure.memory.members.empty()); failure.memory.fail_save = false; }
    failure.server.stop(); assert(failure.server.start(failure.memory.config())); PairActor good(failure.server, 2); good.approve(failure.server); good.socket.close(); failure.server.stop();
    { std::scoped_lock lock(failure.memory.mutex); auto& m = failure.memory.members.front(); const auto pos = m.find("candidate-"); assert(pos != std::string::npos); m[pos] = 'x'; ++failure.memory.revision; }
    assert(!failure.server.start(failure.memory.config())); assert(!failure.memory.profile.empty());
}
void member_pending_limits_and_native_root() {
    {
        FixtureRoom t(std::chrono::seconds(60));
        std::vector<std::unique_ptr<PairActor>> actors;
        for (unsigned i = 0; i < 64; ++i) actors.push_back(std::make_unique<PairActor>(t.server, i + 100));
        assert(t.server.snapshot().pending.size() == kMaximumRoomMembers);
        auto overflow = connect_preface(t.server); assert(!overflow.is_open());
        const auto start = Clock::now(); t.server.stop();
        assert(Clock::now() - start < std::chrono::seconds(3));
        assert(t.server.snapshot().pending.empty());
    }
    {
        FixtureRoom t;
        for (unsigned i = 0; i < 64; ++i) {
            PairActor c(t.server, i + 200); c.approve(t.server); c.socket.close();
        }
        assert(t.server.snapshot().room->members.size() == kMaximumRoomMembers);
        // The 65th network attempt is refused by the minute attempt budget;
        // restart refreshes that transport budget but retains all 64 members.
        t.server.stop(); assert(t.server.start(t.memory.config()));
        PairActor overflow(t.server, 999); const auto id = overflow.pending_id(t.server);
        assert(t.server.approve(id, overflow.secrets.short_authentication_string)); overflow.confirm(); assert(!overflow.accept());
        assert(t.server.snapshot().room->members.size() == kMaximumRoomMembers);
        t.server.stop(); auto config = t.memory.config(); config.memory_fixture.reset();
        config.state_root = std::filesystem::current_path();
        std::string error; assert(!t.server.start(config, &error));
        assert(error == "room protected owner storage is unavailable");
    }
}
void peer_queue_limits_and_fairness() {
    for (const bool byte_limit : {false, true}) {
        FixtureRoom t; PairActor candidate(t.server); candidate.approve(t.server);
        auto channel = authenticated(candidate, t.server);
        std::mutex mutex; std::condition_variable changed; bool blocked = false, released = false;
        t.server.set_final_ack_handler([&](std::uint64_t, const exam::StateRequest&, const security::Sha256Digest&) {
            std::unique_lock lock(mutex); blocked = true; changed.notify_all(); changed.wait(lock, [&] { return released; });
        });
        const auto pin = t.server.public_package_pin(); assert(pin);
        assert(channel.send({Kind::prepared, 1, {pin->digest.begin(), pin->digest.end()}}, Clock::now() + std::chrono::seconds(1)));
        assert(wait_until([&] { std::scoped_lock lock(mutex); return blocked; }));
        assert(!t.server.snapshot().peers.empty());
        const auto peer = t.server.snapshot().peers.front().peer_id;
        contest_room::Message outgoing{Kind::heartbeat, 2, {}};
        unsigned count = 16;
        if (byte_limit) {
            contest_room::PackageChunk chunk; chunk.digest = pin->digest; chunk.total_bytes = 1024u * 1024u;
            chunk.data.assign(contest_room::kMaximumChunkDataBytes, std::byte{7});
            outgoing = {Kind::package_chunk, 2, contest_room::encode_chunk(chunk)};
            count = static_cast<unsigned>(kMaximumPeerQueueBytes / outgoing.payload.size());
        }
        for (unsigned i = 0; i < count; ++i) assert(t.server.queue_message(peer, outgoing));
        assert(!t.server.queue_message(peer, outgoing));
        // A blocked peer worker cannot hold the listener/UI state mutex.
        PairActor other(t.server, 2); other.approve(t.server);
        auto second = authenticated(other, t.server); assert(second.is_open());
        { std::scoped_lock lock(mutex); released = true; changed.notify_all(); }
        const auto last = channel.receive(Clock::now() + std::chrono::seconds(2));
        if (last) assert(last->kind == Kind::lifecycle);
        assert(!channel.receive(Clock::now() + std::chrono::seconds(2)));
    }
}
void admission_expiry_and_cancellation() {
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
        FixtureRoom t(std::chrono::milliseconds(700));
        const unsigned stage = scenario % 3;
        const bool cancel = scenario >= 3;
        PairActor c(t.server);
        { std::scoped_lock lock(t.memory.mutex);
          t.memory.prepare_seen = false; t.memory.approve_seen = false; t.memory.save_seen = false;
          t.memory.prepare_blocked = stage == 0; t.memory.approve_blocked = stage == 1; t.memory.save_blocked = stage == 2; }
        const auto id = c.pending_id(t.server); assert(t.server.approve(id, c.secrets.short_authentication_string)); c.confirm();
        assert(wait_until([&] { std::scoped_lock lock(t.memory.mutex); return stage == 0 ? t.memory.prepare_seen : stage == 1 ? t.memory.approve_seen : t.memory.save_seen; }));
        std::thread stopper;
        if (cancel) {
            stopper = std::thread([&] { t.server.stop(); });
            assert(wait_until([&] { return !t.server.snapshot().running; }));
        } else std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        { std::scoped_lock lock(t.memory.mutex); t.memory.prepare_blocked = false; t.memory.approve_blocked = false; t.memory.save_blocked = false; t.memory.changed.notify_all(); }
        assert(!c.accept());
        if (stopper.joinable()) stopper.join();
        assert(t.server.snapshot().room->members.empty());
        { std::scoped_lock lock(t.memory.mutex);
          assert(t.memory.members.empty());
          assert(std::search(t.memory.profile.begin(), t.memory.profile.end(), c.secrets.enrolled_key.begin(), c.secrets.enrolled_key.end()) == t.memory.profile.end()); }
    }
}
void restart_lifecycle_witness() {
    FixtureRoom t; PairActor c(t.server); c.approve(t.server); c.socket.close();
    assert(t.server.finish("closed-witness"));
    assert(wait_until([&] { return t.server.snapshot().room->phase == RoomPhase::closed; }));
    t.server.stop();
    { std::scoped_lock lock(t.memory.mutex); t.memory.phase = "Joining"; ++t.memory.revision; }
    assert(!t.server.start(t.memory.config()));
    { std::scoped_lock lock(t.memory.mutex); t.memory.phase = "Closed"; --t.memory.server_time; ++t.memory.revision; }
    assert(!t.server.start(t.memory.config()));
    { std::scoped_lock lock(t.memory.mutex); ++t.memory.server_time;
      t.memory.server_start = t.memory.server_time; t.memory.deadline = t.memory.server_start + 3600000; ++t.memory.revision; }
    assert(!t.server.start(t.memory.config()));
    assert(!t.memory.profile.empty());
}
void lifecycle_publication_waits_for_witness() {
    FixtureRoom t; PairActor c(t.server); c.approve(t.server); c.socket.close();
    { std::scoped_lock lock(t.memory.mutex); t.memory.save_seen = false; t.memory.save_blocked = true; }
    assert(t.server.finish("durable-close"));
    assert(wait_until([&] { std::scoped_lock lock(t.memory.mutex); return t.memory.save_seen; }));
    assert(t.server.snapshot().room->phase == RoomPhase::joining);
    { std::scoped_lock lock(t.memory.mutex); t.memory.save_blocked = false; t.memory.changed.notify_all(); }
    assert(wait_until([&] { return t.server.snapshot().room->phase == RoomPhase::closed; }));
    t.server.stop();
    { std::scoped_lock lock(t.memory.mutex); t.memory.phase = "Joining"; ++t.memory.revision; }
    assert(!t.server.start(t.memory.config()));
}

// These bytes are synthetic memory archive fixtures; only TCP/window/broker
// binding is exercised here, never ZIP/manifest/ACL or Linux judge readiness.
ContestRoomServerConfig archive_config(MemoryFixture& fixture) {
    auto config = fixture.config();
    RoomMemoryArchiveFixture archive;
    archive.archive.resize(7u * contest_package::kChunkDataBytes + 13u);
    for (std::size_t i = 0; i < archive.archive.size(); ++i) archive.archive[i] = static_cast<std::byte>(i % 251u);
    config.archive_fixture = std::move(archive);
    return config;
}
contest_room::Message receive_kind(contest_room::RoomChannel& channel, Kind kind) {
    for (unsigned i = 0; i < 8; ++i) {
        auto message = channel.receive(Clock::now() + std::chrono::seconds(2)); assert(message);
        if (message->kind == kind) return *message;
        assert(message->kind == Kind::lifecycle);
    }
    assert(false); return {};
}
void transfer_window_resume_and_final_ack() {
    FixtureRoom t; t.server.stop(); const auto config = archive_config(t.memory); assert(t.server.start(config));
    PairActor c(t.server); c.approve(t.server); auto channel = authenticated(c, t.server);
    const auto descriptor = receive_kind(channel, Kind::package_offer); assert(descriptor.request_id);
    assert(!t.server.queue_message(t.server.snapshot().peers.front().peer_id, descriptor));
    const auto content = std::string("01") + std::string(62, '0');
    const auto offer = contest_package::decode_offer(descriptor.payload, "cs-package", content); assert(offer);
    assert(offer->archive_byte_count == config.archive_fixture->archive.size());
    const auto archive_digest = security::sha256(config.archive_fixture->archive); assert(archive_digest);
    assert(contest_package::digest_from_hex(offer->archive_digest) == archive_digest);
    assert(!t.server.snapshot().room->members.front().prepared);
    assert(channel.send({Kind::package_ack, descriptor.request_id, contest_package::encode_ack({*archive_digest, 0})}, Clock::now() + std::chrono::seconds(1)));
    std::uint64_t offset = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const auto wire = receive_kind(channel, Kind::package_chunk); assert(wire.request_id == descriptor.request_id);
        const auto chunk = contest_package::decode_chunk(wire.payload); assert(chunk && chunk->offset == offset);
        assert(std::equal(chunk->data.begin(), chunk->data.end(), config.archive_fixture->archive.begin() + static_cast<std::ptrdiff_t>(offset)));
        offset += chunk->data.size();
    }
    // A full package window still services source and heartbeat traffic.
    const auto member = t.server.snapshot().room->members.front();
    exam::programming::Request source; source.context = member.context; source.operation = exam::programming::Operation::draft;
    source.request_id = "transfer-draft"; source.problem_id = "sum"; source.language = "cpp20"; source.draft_revision = 1; source.source = "int main() {}";
    { std::scoped_lock lock(t.memory.mutex); t.memory.source_reply = exam::programming::unavailable_response_json(source, "runner_unavailable"); }
    assert(channel.send({Kind::programming_request, 9001, exam::programming::encode_request(source)}, Clock::now() + std::chrono::seconds(1)));
    const auto source_reply = receive_kind(channel, Kind::programming_reply); assert(source_reply.request_id == 9001);
    assert(channel.send({Kind::heartbeat, 9002, {}}, Clock::now() + std::chrono::seconds(1)));
    const auto heartbeat = receive_kind(channel, Kind::join_status); assert(heartbeat.request_id == 9002);
    assert(std::string(reinterpret_cast<const char*>(heartbeat.payload.data()), heartbeat.payload.size()).find("\"publicationPolicy\":\"teacher_only\"") != std::string::npos);
    // Another authenticated member is not blocked by this unacknowledged window.
    PairActor second(t.server, 2); second.approve(t.server); auto other = authenticated(second, t.server);
    const auto other_offer = receive_kind(other, Kind::package_offer); assert(other_offer.request_id != descriptor.request_id);
    other.close();
    channel.close(); assert(wait_until([&] { return t.server.snapshot().peers.empty(); }));
    // A fresh native owner reconstructs progress and the original budget from
    // its protected witness; this is not merely the same object's restart.
    t.server.stop(); ContestRoomServer restarted(t.broker); assert(restarted.start(config)); channel = authenticated(c, restarted, true);
    const auto resumed = receive_kind(channel, Kind::package_offer); assert(resumed.request_id == descriptor.request_id && resumed.payload == descriptor.payload);
    assert(channel.send({Kind::package_ack, resumed.request_id, contest_package::encode_ack({*archive_digest, offset})}, Clock::now() + std::chrono::seconds(1)));
    while (offset < offer->archive_byte_count) {
        const auto wire = receive_kind(channel, Kind::package_chunk); assert(wire.request_id == descriptor.request_id);
        const auto chunk = contest_package::decode_chunk(wire.payload); assert(chunk && chunk->offset == offset);
        assert(std::equal(chunk->data.begin(), chunk->data.end(), config.archive_fixture->archive.begin() + static_cast<std::ptrdiff_t>(offset)));
        offset += chunk->data.size();
        assert(channel.send({Kind::package_ack, descriptor.request_id, contest_package::encode_ack({*archive_digest, offset})}, Clock::now() + std::chrono::seconds(1)));
    }
    assert(!restarted.snapshot().room->members.front().prepared);
    { std::scoped_lock lock(t.memory.mutex); t.memory.save_seen = false; t.memory.save_blocked = true; }
    assert(channel.send({Kind::prepared, descriptor.request_id, contest_package::encode_prepared({*offer})}, Clock::now() + std::chrono::seconds(1)));
    assert(wait_until([&] { std::scoped_lock lock(t.memory.mutex); return t.memory.save_seen; }));
    assert(!restarted.snapshot().room->members.front().prepared);
    { std::scoped_lock lock(t.memory.mutex); t.memory.save_blocked = false; t.memory.changed.notify_all(); }
    assert(wait_until([&] { return restarted.snapshot().room->members.front().prepared; }));
    assert(!restarted.snapshot().room->members.back().prepared);
    assert(channel.send({Kind::prepared, descriptor.request_id, contest_package::encode_prepared({*offer})}, Clock::now() + std::chrono::seconds(1)));
    assert(channel.send({Kind::heartbeat, 9003, {}}, Clock::now() + std::chrono::seconds(1)));
    assert(receive_kind(channel, Kind::join_status).request_id == 9003);
}
void transfer_rejects_unbound_and_partial_ack() {
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        FixtureRoom t; t.server.stop(); assert(t.server.start(archive_config(t.memory)));
        PairActor c(t.server); c.approve(t.server); auto channel = authenticated(c, t.server);
        const auto descriptor = receive_kind(channel, Kind::package_offer);
        const auto offer = contest_package::decode_offer(descriptor.payload, "cs-package", "01" + std::string(62, '0')); assert(offer);
        auto digest = *contest_package::digest_from_hex(offer->archive_digest);
        contest_room::Message invalid;
        if (scenario == 0) invalid = {Kind::package_ack, descriptor.request_id + 1, contest_package::encode_ack({digest, 0})};
        if (scenario == 1) { digest[0] ^= std::byte{1}; invalid = {Kind::package_ack, descriptor.request_id, contest_package::encode_ack({digest, 0})}; }
        if (scenario == 2) invalid = {Kind::prepared, descriptor.request_id, contest_package::encode_prepared({*offer})};
        if (scenario == 3) invalid = {Kind::package_ack, descriptor.request_id, contest_package::encode_ack({digest, offer->archive_byte_count})};
        assert(channel.send(invalid, Clock::now() + std::chrono::seconds(1)));
        assert(!channel.receive(Clock::now() + std::chrono::seconds(2)));
        assert(!t.server.snapshot().room->members.front().prepared);
    }
    FixtureRoom t; t.server.stop(); auto config = archive_config(t.memory); config.memory_fixture.reset();
    assert(!t.server.start(config));
    config = t.memory.config(); config.public_archive = RoomPublicArchiveSource{};
    assert(!t.server.start(config));
    config = archive_config(t.memory); config.archive_fixture->archive.clear();
    assert(!t.server.start(config));
}
void transfer_broker_failure_retry_and_cancel() {
    for (const bool cancel : {false, true}) {
        FixtureRoom t; t.server.stop(); auto config = archive_config(t.memory); config.archive_fixture->archive.resize(123); assert(t.server.start(config));
        PairActor c(t.server); c.approve(t.server); auto channel = authenticated(c, t.server);
        const auto descriptor = receive_kind(channel, Kind::package_offer);
        const auto offer = contest_package::decode_offer(descriptor.payload, "cs-package", "01" + std::string(62, '0')); assert(offer);
        const auto digest = *contest_package::digest_from_hex(offer->archive_digest);
        assert(channel.send({Kind::package_ack, descriptor.request_id, contest_package::encode_ack({digest, 0})}, Clock::now() + std::chrono::seconds(1)));
        const auto data = receive_kind(channel, Kind::package_chunk); const auto chunk = contest_package::decode_chunk(data.payload); assert(chunk && chunk->data.size() == 123);
        assert(channel.send({Kind::package_ack, descriptor.request_id, contest_package::encode_ack({digest, 123})}, Clock::now() + std::chrono::seconds(1)));
        { std::scoped_lock lock(t.memory.mutex); t.memory.member_prepare_seen = false; t.memory.fail_member_prepare = !cancel; t.memory.member_prepare_blocked = cancel; }
        assert(channel.send({Kind::prepared, descriptor.request_id, contest_package::encode_prepared({*offer})}, Clock::now() + std::chrono::seconds(1)));
        assert(wait_until([&] { std::scoped_lock lock(t.memory.mutex); return t.memory.member_prepare_seen; }));
        assert(!t.server.snapshot().room->members.front().prepared);
        if (cancel) {
            std::thread stop([&] { t.server.stop(); }); assert(wait_until([&] { return !t.server.snapshot().running; }));
            { std::scoped_lock lock(t.memory.mutex); t.memory.member_prepare_blocked = false; t.memory.changed.notify_all(); }
            stop.join(); assert(t.server.snapshot().room->members.empty());
            assert(t.server.start(config)); c.socket = connect_preface(t.server); assert(!c.handshake());
        } else {
            assert(wait_until([&] { return t.server.snapshot().error == "room broker state is unavailable"; }));
            channel.close(); assert(wait_until([&] { return t.server.snapshot().peers.empty(); }));
            { std::scoped_lock lock(t.memory.mutex); t.memory.fail_member_prepare = false; t.memory.member_prepare_seen = false; }
            channel = authenticated(c, t.server, true);
            const auto repeated = receive_kind(channel, Kind::package_offer); assert(repeated.request_id == descriptor.request_id);
            assert(channel.send({Kind::package_ack, descriptor.request_id, contest_package::encode_ack({digest, 123})}, Clock::now() + std::chrono::seconds(1)));
            assert(channel.send({Kind::prepared, descriptor.request_id, contest_package::encode_prepared({*offer})}, Clock::now() + std::chrono::seconds(1)));
            assert(wait_until([&] { return t.server.snapshot().room->members.front().prepared; }));
        }
    }
}
void preparation_stop_at_publication_boundary() {
    FixtureRoom t; t.server.stop(); auto config = archive_config(t.memory); config.archive_fixture->archive.resize(123);
    std::mutex mutex; std::condition_variable changed; bool reached = false, released = false;
    config.memory_fixture->before_preparation_publication = [&] {
        std::unique_lock lock(mutex); reached = true; changed.notify_all(); changed.wait(lock, [&] { return released; });
    };
    assert(t.server.start(config)); PairActor c(t.server); c.approve(t.server); auto channel = authenticated(c, t.server);
    const auto descriptor = receive_kind(channel, Kind::package_offer);
    const auto offer = contest_package::decode_offer(descriptor.payload, "cs-package", "01" + std::string(62, '0')); assert(offer);
    const auto digest = *contest_package::digest_from_hex(offer->archive_digest);
    assert(channel.send({Kind::package_ack, descriptor.request_id, contest_package::encode_ack({digest, 0})}, Clock::now() + std::chrono::seconds(1)));
    const auto data = receive_kind(channel, Kind::package_chunk); const auto chunk = contest_package::decode_chunk(data.payload); assert(chunk && chunk->data.size() == 123);
    assert(channel.send({Kind::package_ack, descriptor.request_id, contest_package::encode_ack({digest, 123})}, Clock::now() + std::chrono::seconds(1)));
    assert(channel.send({Kind::prepared, descriptor.request_id, contest_package::encode_prepared({*offer})}, Clock::now() + std::chrono::seconds(1)));
    assert(wait_until([&] { std::scoped_lock lock(mutex); return reached; }));
    assert(!t.server.snapshot().room->members.front().prepared);
    // Stop wins the state mutex after all prior active checks/flushes.
    std::thread stop([&] { t.server.stop(); }); assert(wait_until([&] { return !t.server.snapshot().running; }));
    { std::scoped_lock lock(mutex); released = true; changed.notify_all(); }
    stop.join(); const auto after = t.server.snapshot(); assert(!after.running);
    assert(std::none_of(after.room->members.begin(), after.room->members.end(), [](const RoomMember& member) { return member.prepared; }));
    assert(after.room->members.empty());
    assert(t.server.start(config)); c.socket = connect_preface(t.server); assert(!c.handshake());
}
std::size_t transfer_record_offset(const std::vector<std::byte>& profile) {
    std::size_t offset = 8;
    const auto skip_blob = [&] {
        assert(profile.size() - offset >= 4); std::uint32_t count = 0;
        for (unsigned i = 0; i < 4; ++i) count |= std::to_integer<std::uint32_t>(profile[offset++]) << (8u * i);
        assert(count <= profile.size() - offset); offset += count;
    };
    for (unsigned i = 0; i < 6; ++i) skip_blob();
    offset += 4; skip_blob(); offset += 4;
    assert(profile.size() - offset >= 32); return offset;
}
void transfer_restart_retains_budget_and_pin() {
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        FixtureRoom t; t.server.stop(); const auto config = archive_config(t.memory); assert(t.server.start(config));
        PairActor c(t.server); c.approve(t.server); auto channel = authenticated(c, t.server);
        const auto descriptor = receive_kind(channel, Kind::package_offer); assert(descriptor.request_id);
        channel.close(); t.server.stop();
        {
            std::scoped_lock lock(t.memory.mutex);
            const auto record = transfer_record_offset(t.memory.profile);
            const auto wall = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
            auto value = scenario == 0 ? wall - 600001 : wall + 60000;
            if (scenario < 2) for (unsigned i = 0; i < 8; ++i) t.memory.profile[record + 8 + i] = static_cast<std::byte>((value >> (8u * i)) & 255u);
            if (scenario == 2) t.memory.profile[record + 24 + 8 + 4] ^= std::byte{1};
            if (scenario == 3) for (unsigned i = 0; i < 8; ++i) t.memory.profile[record + 8 + i] = std::byte{0};
        }
        ContestRoomServer fresh(t.broker);
        if (scenario == 3) { assert(!fresh.start(config)); continue; }
        assert(fresh.start(config)); channel = authenticated(c, fresh, true);
        assert(wait_until([&] { return fresh.snapshot().error == "room transfer expired or archive/clock witness conflicts"; }));
        const auto pin = pins().package_digest;
        assert(channel.send({Kind::prepared, descriptor.request_id, {pin.begin(), pin.end()}}, Clock::now() + std::chrono::seconds(1)));
        assert(!channel.receive(Clock::now() + std::chrono::seconds(2)));
        assert(!fresh.snapshot().room->members.front().prepared);
    }
}
} // namespace
int main() {
    net::WinsockRuntime runtime; assert(runtime.ready());
    admission_expiry_and_cancellation();
    restart_lifecycle_witness();
    lifecycle_publication_waits_for_witness();
    transfer_window_resume_and_final_ack();
    transfer_rejects_unbound_and_partial_ack();
    transfer_broker_failure_retry_and_cancel();
    preparation_stop_at_publication_boundary();
    transfer_restart_retains_budget_and_pin();
    sas_rejections(); source_and_recovery(); foreign_management_and_downgrade(); bounded_and_durability();
    member_pending_limits_and_native_root();
    peer_queue_limits_and_fairness();
    std::cout << "public archive transfer: four-chunk window, actual byte/hash fixture, member/request/pin binding, fresh-owner resume, original budget, broker retry/cancel and durable final ACK passed\n";
    std::cout << "contest room listener tests passed (memory backend/profile fixtures, real native TCP/CNG/AEAD)\n";
}
