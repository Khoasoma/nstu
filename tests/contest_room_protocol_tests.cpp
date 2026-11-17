#include "nstu/contest_room_protocol.hpp"
#include "nstu/multicast.hpp"
#include "nstu/programming.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <future>
#include <iostream>
#include <thread>

#if defined(_WIN32)
#include <winsock2.h>
#endif

namespace {
using namespace nstu;
using namespace nstu::contest_room;
using namespace std::chrono_literals;
Realm realm() { return {1, "teacher-01", std::string(32, '1'), std::string(32, '2')}; }
security::Nonce nonce(unsigned int value) { security::Nonce result{}; result.fill(static_cast<std::byte>(value)); return result; }
security::ClientId identity() { security::ClientId result{}; result[0] = std::byte{1}; return result; }

void codec_rejections() {
    const auto pinned = realm();
    constexpr char golden_realm[] = "\x0f\x00NSTU_CONTEST_V1\x02\x00\x01\x00\x0a\x00teacher-01\x20\x00"
        "11111111111111111111111111111111\x20\x00"
        "22222222222222222222222222222222\x11\x00" "contest_candidate";
    const auto canonical = encode_realm(pinned);
    const auto golden = std::as_bytes(std::span(golden_realm, sizeof(golden_realm) - 1));
    assert(canonical && std::equal(canonical->begin(), canonical->end(), golden.begin(), golden.end()));
    assert(canonical->size() == 120);
    const auto wire = encode_preface(pinned, Side::candidate, nonce(1));
    assert(!wire.empty());
    assert(decode_preface(wire, pinned, Side::candidate, nonce(1)));
    assert(!decode_preface(wire, pinned, Side::teacher));
    assert(!decode_preface(wire, pinned, Side::candidate, nonce(2)));
    assert(encode_preface(pinned, Side::candidate, {}).empty());
    auto zero_nonce = wire;
    std::fill_n(zero_nonce.begin() + 10, security::kNonceBytes, std::byte{0});
    assert(!decode_preface(zero_nonce, pinned, Side::candidate));
    auto wrong_authority = wire; wrong_authority.back() ^= std::byte{1};
    assert(!decode_preface(wrong_authority, pinned, Side::candidate));
    auto wrong_domain = wire; wrong_domain[kPrefaceHeaderBytes + 2] ^= std::byte{1};
    assert(!decode_preface(wrong_domain, pinned, Side::candidate));
    for (const std::size_t offset : {0u, 4u, 6u, 8u}) {
        auto wrong = wire; wrong[offset] ^= std::byte{0x80};
        assert(!decode_preface(wrong, pinned, Side::candidate));
    }
    for (std::size_t size = 0; size < wire.size(); ++size)
        assert(!decode_preface(std::span(wire).first(size), pinned, Side::candidate));
    auto trailing = wire; trailing.push_back(std::byte{0});
    assert(!decode_preface(trailing, pinned, Side::candidate));
    for (const auto side : {Side::candidate, Side::teacher}) {
        Message message{Kind::heartbeat, 42, {}};
        const auto payload = encode_message(pinned, side, message);
        auto decoded = decode_message(payload, pinned, side);
        assert(decoded && decoded->request_id == 42 && decoded->kind == Kind::heartbeat);
        for (std::size_t size = 0; size < payload.size(); ++size)
            assert(!decode_message(std::span(payload).first(size), pinned, side));
        for (const std::size_t offset : {0u, 4u, 6u, 8u, 10u, 12u}) {
            auto wrong = payload; wrong[offset] ^= std::byte{0x80};
            assert(!decode_message(wrong, pinned, side));
        }
        auto wrong_domain = payload; wrong_domain[kMessageHeaderBytes + 2] ^= std::byte{1};
        assert(!decode_message(wrong_domain, pinned, side));
        auto wrong_authority = payload; wrong_authority.back() ^= std::byte{1};
        assert(!decode_message(wrong_authority, pinned, side));
        auto extra = payload; extra.push_back(std::byte{0});
        assert(!decode_message(extra, pinned, side));
        auto other = pinned; other.round_id[0] = '3';
        assert(!decode_message(payload, other, side));
        other = pinned; other.room_id[0] = '3'; assert(!decode_message(payload, other, side));
        other = pinned; other.teacher_id = "other"; assert(!decode_message(payload, other, side));
        assert(!decode_message(payload, pinned, side == Side::candidate ? Side::teacher : Side::candidate));
        control::AuthenticatedCommand command;
        command.envelope.type = protocol::CommandType::heartbeat;
        command.envelope.request_id = 42;
        command.payload = payload;
        assert(decode_command(command, pinned, side));
        command.envelope.request_id = 43;
        assert(!decode_command(command, pinned, side));
        command.envelope.request_id = 42;
        for (unsigned int value = 0; value < 256; ++value) {
            if (value == static_cast<unsigned int>(protocol::CommandType::heartbeat)) continue;
            command.envelope.type = static_cast<protocol::CommandType>(value);
            assert(!decode_command(command, pinned, side));
        }
    }
    for (unsigned int value = 0; value < 256; ++value) {
        const auto kind = static_cast<Kind>(value);
        if (!allowed(Side::candidate, kind)) assert(encode_message(pinned, Side::candidate, {kind, 1, {}}).empty());
        if (!allowed(Side::teacher, kind)) assert(encode_message(pinned, Side::teacher, {kind, 1, {}}).empty());
    }
    auto invalid = pinned; invalid.version = 2; assert(!encode_realm(invalid));
    invalid = pinned; invalid.room_id.assign(32, '0'); assert(!encode_realm(invalid));
    invalid = pinned; invalid.room_id[0] = 'A'; assert(!encode_realm(invalid));
    invalid = pinned; invalid.teacher_id.assign(97, 'a'); assert(!encode_realm(invalid));
    invalid = pinned; invalid.teacher_id = "teacher\n"; assert(!encode_realm(invalid));
    invalid = pinned; invalid.teacher_id = "teacher name"; assert(!encode_realm(invalid));
    invalid = pinned; invalid.teacher_id = ".teacher"; assert(!encode_realm(invalid));
    invalid = pinned; invalid.teacher_id = "-teacher"; assert(!encode_realm(invalid));
    invalid = pinned; invalid.teacher_id.assign(96, '_'); assert(encode_realm(invalid));
    assert(!encode_realm({1, "", pinned.room_id, pinned.round_id}));
    assert(encode_message(pinned, Side::candidate, {Kind::heartbeat, 1, {std::byte{1}}}).empty());
    assert(encode_message(pinned, Side::candidate, {Kind::join, 1, std::vector<std::byte>(257)}).empty());
    assert(encode_message(pinned, Side::teacher, {Kind::standings, 1, std::vector<std::byte>(kMaximumSnapshotBytes + 1)}).empty());
    assert(encode_message(pinned, Side::teacher, {Kind::programming_reply, 1, std::vector<std::byte>(kMaximumMessageBytes + 1)}).empty());
    exam::programming::Request request;
    request.context = {"package", {}, {}, {}, "candidate"};
    request.context.package_digest[0] = std::byte{1}; request.context.client_id[0] = std::byte{2}; request.context.session_id[0] = std::byte{3};
    request.request_id = "request"; request.problem_id = "problem"; request.language = "cpp20"; request.draft_revision = 1;
    request.source.assign(exam::programming::kMaximumSourceBytes, 'a');
    const auto source = exam::programming::encode_request(request);
    assert(!source.empty());
    assert(!encode_message(pinned, Side::candidate, {Kind::programming_request, 1, source}).empty());
    auto bad_source = source; bad_source.push_back(std::byte{0});
    assert(encode_message(pinned, Side::candidate, {Kind::programming_request, 1, bad_source}).empty());
    request.workstation_policy_ready = true;
    assert(encode_message(pinned, Side::candidate, {Kind::programming_request, 1, exam::programming::encode_request(request)}).empty());
    PackageChunk chunk{nonce(1), 0, 65536, std::vector<std::byte>(kMaximumChunkDataBytes, std::byte{4})};
    std::vector<std::byte> pin(32, std::byte{1});
    assert(!encode_message(pinned, Side::candidate, {Kind::prepared, 1, pin}).empty());
    assert(!encode_message(pinned, Side::candidate, {Kind::join, 1, pin}).empty());
    assert(!encode_message(pinned, Side::teacher, {Kind::join_status, 1, pin}).empty());
    assert(!encode_message(pinned, Side::teacher, {Kind::lifecycle, 1, pin}).empty());
    pin.resize(40, std::byte{0}); pin[32] = std::byte{1};
    assert(!encode_message(pinned, Side::teacher, {Kind::package_offer, 1, pin}).empty());
    assert(!encode_message(pinned, Side::candidate, {Kind::package_ack, 1, pin}).empty());
    pin[32] = std::byte{0};
    assert(encode_message(pinned, Side::teacher, {Kind::package_offer, 1, pin}).empty());
    assert(!encode_message(pinned, Side::candidate, {Kind::package_ack, 1, pin}).empty());
    pin.push_back(std::byte{0});
    assert(encode_message(pinned, Side::candidate, {Kind::package_ack, 1, pin}).empty());
    const auto chunk_wire = encode_chunk(chunk);
    assert(decode_chunk(chunk_wire));
    assert(!encode_message(pinned, Side::teacher, {Kind::package_chunk, 1, chunk_wire}).empty());
    chunk.data.push_back(std::byte{4}); assert(encode_chunk(chunk).empty());
    chunk.data.resize(1); chunk.offset = chunk.total_bytes; assert(encode_chunk(chunk).empty());
    chunk.offset = 0; chunk.total_bytes = kMaximumArchiveBytes + 1; assert(encode_chunk(chunk).empty());
    chunk.total_bytes = 1; chunk.digest = {}; assert(encode_chunk(chunk).empty());
    auto oversized = chunk_wire; oversized.push_back(std::byte{0}); assert(!decode_chunk(oversized));
    const auto preamble = protocol::encode_connection_preamble({});
    assert(!decode_preface(preamble, pinned, Side::candidate));
    control::AuthenticatedSession fallback(security::Sha256Digest{}, identity(), 1);
    RoomChannel refused(net::TcpSocket{}, std::move(fallback), pinned, Side::candidate);
    assert(!refused.is_open());
}

#if defined(_WIN32)
#if defined(_WIN32)
void native_rejection_and_deadline_tests() {
    const auto pinned = realm();
    const auto key = nonce(9);
    const auto aead = control::derive_aead_session_keys(key); assert(aead);
    auto fixture = [&](auto exercise) {
        net::TcpSocket listener, sender; std::string error;
        assert(listener.listen(0, 4, &error));
        assert(sender.connect_with_timeout("127.0.0.1", listener.local_port(), 2000, &error));
        auto receiver = listener.accept(&error); assert(receiver.is_open());
        assert(sender.set_io_timeouts(2000, 2000, &error));
        exercise(sender, receiver);
    };
    auto raw_frame = [&](const Realm& sent_realm, Side side, protocol::CommandType outer, std::uint64_t sequence) {
        const auto payload = encode_message(sent_realm, side, {Kind::heartbeat, 55, {}}); assert(!payload.empty());
        const protocol::CommandEnvelope envelope{protocol::kCommandVersion, outer, static_cast<std::uint32_t>(payload.size()), 55};
        return control::encode_authenticated_command_aead(aead->client_to_server_key, aead->client_to_server_iv, envelope, sequence, payload);
    };
    for (unsigned int mode = 0; mode < 6; ++mode) fixture([&](net::TcpSocket& sender, net::TcpSocket& socket) {
        control::AuthenticatedSession session(key, identity(), 7, true, protocol::ConnectionRole::server);
        RoomChannel receiver(std::move(socket), std::move(session), pinned, Side::teacher);
        assert(receiver.is_open());
        auto changed = pinned; changed.round_id[0] = '3';
        auto wire = raw_frame(mode == 1 ? changed : pinned, mode == 2 ? Side::teacher : Side::candidate,
                              mode == 0 ? protocol::CommandType::lock : protocol::CommandType::heartbeat, 0);
        assert(!wire.empty());
        if (mode == 3) wire.back() ^= std::byte{1};
        if (mode == 4) wire[0] ^= std::byte{1};
        std::string error;
        assert(sender.send_all(wire, std::chrono::steady_clock::now() + 2s, &error) == static_cast<int>(wire.size()));
        if (mode == 5) {
            assert(receiver.receive(std::chrono::steady_clock::now() + 2s, &error));
            assert(sender.send_all(wire, std::chrono::steady_clock::now() + 2s, &error) == static_cast<int>(wire.size()));
        }
        assert(!receiver.receive(std::chrono::steady_clock::now() + 2s, &error));
        assert(!receiver.is_open());
        assert(!receiver.send({Kind::heartbeat, 1, {}}, std::chrono::steady_clock::now() + 2s, &error));
    });
    for (const std::size_t initially_sent : {1u, 24u}) fixture([&](net::TcpSocket& sender, net::TcpSocket& socket) {
        const auto wire = raw_frame(pinned, Side::candidate, protocol::CommandType::heartbeat, 0);
        control::AuthenticatedSession session(key, identity(), 7, true, protocol::ConnectionRole::server);
        RoomChannel receiver(std::move(socket), std::move(session), pinned, Side::teacher);
        std::promise<void> started; auto ready = started.get_future();
        std::thread drip([&] {
            std::string error;
            assert(sender.send_all(std::span(wire).first(initially_sent), std::chrono::steady_clock::now() + 1s, &error) == static_cast<int>(initially_sent));
            started.set_value();
            for (std::size_t i = initially_sent; i < initially_sent + 10; ++i) {
                std::this_thread::sleep_for(15ms);
                if (sender.send_all(std::span(wire).subspan(i, 1), std::chrono::steady_clock::now() + 1s, &error) != 1) break;
            }
        });
        ready.get();
        std::string error; const auto start = std::chrono::steady_clock::now();
        assert(!receiver.receive(start + 60ms, &error));
        const auto elapsed = std::chrono::steady_clock::now() - start;
        assert(elapsed < 500ms && !receiver.is_open());
        assert(error == "room receive deadline expired");
        drip.join();
    });
    fixture([&](net::TcpSocket& sender, net::TcpSocket& receiver) {
        std::string error;
        assert(send_preface(sender, pinned, Side::candidate, nonce(3), std::chrono::steady_clock::now() + 1s, &error));
        auto decoded = receive_preface(receiver, pinned, Side::candidate, std::chrono::steady_clock::now() + 1s, nonce(3), &error);
        assert(decoded && decoded->nonce == nonce(3));
        assert(send_preface(receiver, pinned, Side::teacher, decoded->nonce, std::chrono::steady_clock::now() + 1s, &error));
        assert(receive_preface(sender, pinned, Side::teacher, std::chrono::steady_clock::now() + 1s, nonce(3), &error));
    });
    fixture([&](net::TcpSocket& sender, net::TcpSocket& receiver) {
        std::string error;
        const auto managed = protocol::encode_connection_preamble({});
        assert(sender.send_all(managed, &error) == static_cast<int>(managed.size()));
        assert(!receive_preface(receiver, pinned, Side::candidate, std::chrono::steady_clock::now() + 1s, std::nullopt, &error));
        assert(!receiver.is_open());
    });
    fixture([&](net::TcpSocket& sender, net::TcpSocket& receiver) {
        std::string error;
        control::AuthenticatedSession session(key, identity(), 7, true, protocol::ConnectionRole::server);
        RoomChannel channel(std::move(receiver), std::move(session), pinned, Side::teacher);
        sender.close();
        assert(!channel.receive(std::chrono::steady_clock::now() + 1s, &error));
        assert(!channel.is_open());
    });
}
#endif

void real_crypto_and_transport() {
    auto client = pairing::EphemeralKeyPair::generate(); auto teacher = pairing::EphemeralKeyPair::generate();
    assert(client && teacher && client->agreement() == teacher->agreement());
    auto shared = client->agree(teacher->public_key()); auto peer_shared = teacher->agree(client->public_key());
    assert(shared && peer_shared && *shared == *peer_shared);
    pairing::PairingTranscript transcript;
    transcript.agreement = client->agreement(); transcript.client_uuid = "candidate-uuid"; transcript.client_hostname = "laptop";
    transcript.client_public_key.assign(client->public_key().begin(), client->public_key().end());
    transcript.server_public_key.assign(teacher->public_key().begin(), teacher->public_key().end());
    transcript.client_nonce = nonce(1); transcript.server_nonce = nonce(2);
    const auto pinned = realm();
    auto keys = derive_room_secrets(*shared, pinned, transcript); auto same = derive_room_secrets(*peer_shared, pinned, transcript);
    assert(keys && same && keys->enrolled_key == same->enrolled_key && keys->short_authentication_string == same->short_authentication_string);
    auto managed = pairing::derive_pairing_secrets(*shared, transcript); assert(managed && managed->enrolled_key != keys->enrolled_key);
    const auto tag = pairing::confirmation_tag(*keys, transcript, pairing::ConfirmationRole::client);
    assert(tag && pairing::verify_confirmation_tag(*same, transcript, pairing::ConfirmationRole::client, *tag));
    assert(!pairing::verify_confirmation_tag(*same, transcript, pairing::ConfirmationRole::server, *tag));
    for (unsigned int field = 0; field < 3; ++field) {
        auto changed = pinned;
        if (field == 0) changed.teacher_id = "teacher-02";
        if (field == 1) changed.room_id[0] = '3';
        if (field == 2) changed.round_id[0] = '3';
        auto other = derive_room_secrets(*shared, changed, transcript);
        assert(other && other->enrolled_key != keys->enrolled_key && other->short_authentication_string != keys->short_authentication_string);
        assert(!pairing::verify_confirmation_tag(*other, transcript, pairing::ConfirmationRole::client, *tag));
        pairing::secure_zero(*other);
    }
    auto invalid = pinned; invalid.version = 2; assert(!derive_room_secrets(*shared, invalid, transcript));
    auto bad_transcript = transcript; bad_transcript.client_nonce = {}; assert(!derive_room_secrets(*shared, pinned, bad_transcript));
    bad_transcript = transcript; bad_transcript.version = 2; assert(!derive_room_secrets(*shared, pinned, bad_transcript));
    bad_transcript = transcript; bad_transcript.server_nonce = nonce(4);
    auto fresh = derive_room_secrets(*shared, pinned, bad_transcript);
    assert(fresh && fresh->enrolled_key != keys->enrolled_key);
    assert(!pairing::verify_confirmation_tag(*fresh, bad_transcript, pairing::ConfirmationRole::client, *tag));
    pairing::secure_zero(*fresh);
    const auto room_key = keys->enrolled_key; const auto managed_key = managed->enrolled_key;
    auto handshake = [&](const security::Sha256Digest& presented, const security::Sha256Digest& accepted, bool aead, bool succeeds) {
        net::TcpSocket listener; std::string error; assert(listener.listen(0, 4, &error));
        auto server = std::async(std::launch::async, [&] {
            std::string server_error; auto socket = listener.accept(&server_error); assert(socket.is_open());
            assert(socket.set_io_timeouts(2000, 2000, &server_error));
            security::ReplayProtector replay;
            const control::KeyResolver resolver = [&](const security::ClientId& id, std::uint32_t key_id) -> std::optional<std::vector<std::byte>> {
                if (id != identity() || key_id != 7) return std::nullopt;
                return std::vector<std::byte>(accepted.begin(), accepted.end());
            };
            auto session = control::server_handshake(socket, resolver, replay, 1000, &server_error, aead);
            if (!succeeds) { assert(!session); return; }
            assert(session);
            RoomChannel channel(std::move(socket), std::move(*session), pinned, Side::teacher);
            if (!aead) { assert(!channel.is_open()); return; }
            auto command = channel.receive(std::chrono::steady_clock::now() + 2s, &server_error);
            assert(command && command->kind == Kind::heartbeat);
            assert(channel.send({Kind::heartbeat, command->request_id, {}}, std::chrono::steady_clock::now() + 2s, &server_error));
        });
        net::TcpSocket socket; assert(socket.connect_with_timeout("127.0.0.1", listener.local_port(), 2000, &error));
        assert(socket.set_io_timeouts(2000, 2000, &error));
        auto session = control::client_handshake(socket, identity(), 7, presented, 1000, 120s, &error, aead);
        if (!succeeds) { assert(!session); server.get(); return; }
        assert(session);
        RoomChannel channel(std::move(socket), std::move(*session), pinned, Side::candidate);
        if (!aead) { assert(!channel.is_open()); server.get(); return; }
        assert(channel.send({Kind::heartbeat, 1, {}}, std::chrono::steady_clock::now() + 2s, &error));
        assert(channel.receive(std::chrono::steady_clock::now() + 2s, &error));
        const auto before = std::chrono::steady_clock::now();
        assert(!channel.receive(before + 30ms, &error));
        assert(!channel.is_open());
        server.get();
    };
    handshake(room_key, room_key, true, true);
    handshake(room_key, room_key, false, true);
    handshake(managed_key, room_key, true, false);
    handshake(room_key, managed_key, true, false);
    pairing::secure_zero(*keys); pairing::secure_zero(*same); pairing::secure_zero(*managed);
    security::secure_zero(*shared); security::secure_zero(*peer_shared);
}
#endif
}
int main() {
    codec_rejections();
#if defined(_WIN32)
    nstu::net::WinsockRuntime runtime; assert(runtime.ready());
    real_crypto_and_transport();
    native_rejection_and_deadline_tests();
#endif
    std::cout << "contest room protocol tests passed\n";
}
