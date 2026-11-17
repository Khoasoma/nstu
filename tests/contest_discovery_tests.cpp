#include "nstu/contest_discovery.hpp"
#include "nstu/multicast.hpp"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace cd = nstu::contest_discovery;
using namespace std::chrono_literals;

namespace {
cd::Probe probe() {
    cd::Probe value;
    value.nonce.fill(std::byte{0x31});
    return value;
}

cd::RoomAdvertisement advertisement(char id = '1') {
    return {std::string(32, id), std::string(32, '2'),
            "L\xe1\xba\xadp tr\xc3\xacnh", "Th\xe1\xba\xa7y An", cd::JoinState::open, 1, 47002, "teacher-01"};
}

void codec_tests() {
    const auto request = probe();
    const auto request_wire = cd::encode_probe(request);
    assert(request_wire.size() == cd::kProbeBytes);
    assert(request_wire[0] == std::byte{'N'} && request_wire[1] == std::byte{'S'} &&
           request_wire[2] == std::byte{'C'} && request_wire[3] == std::byte{'R'});
    assert(request_wire[4] == std::byte{2} && request_wire[5] == std::byte{0});
    assert(request_wire[6] == std::byte{1} && request_wire[7] == std::byte{0});
    assert(cd::decode_probe(request_wire)->nonce == request.nonce);
    assert(cd::encode_probe({}).empty());
    const auto ad = advertisement();
    const auto wire = cd::encode_reply(request, ad);
    assert(!wire.empty() && wire.size() <= cd::kMaximumDatagramBytes);
    assert(wire.size() == cd::kReplyHeaderBytes + ad.title.size() + ad.teacher_label.size() + ad.teacher_id.size());
    assert(wire[6] == std::byte{2} && wire[42] == std::byte{0x9a} && wire[43] == std::byte{0xb7});
    assert(wire[46] == static_cast<std::byte>(ad.title.size()) && wire[47] == std::byte{0});
    assert(cd::decode_reply(wire, request) == ad);
    auto other = request;
    other.nonce[0] ^= std::byte{1};
    assert(!cd::decode_reply(wire, other));
    assert(!cd::decode_probe(wire));
    assert(!cd::decode_reply(request_wire, request));

    for (std::size_t length = 0; length < wire.size(); ++length) {
        assert(!cd::decode_reply(std::span(wire).first(length), request));
    }
    for (std::size_t length = 0; length < request_wire.size(); ++length) {
        assert(!cd::decode_probe(std::span(request_wire).first(length)));
    }
    for (const auto index : {0u, 4u, 6u, 40u, 44u}) {
        auto changed = wire;
        changed[index] = std::byte{0};
        assert(!cd::decode_reply(changed, request));
    }
    auto zero_port = wire;
    zero_port[42] = zero_port[43] = std::byte{0};
    assert(!cd::decode_reply(zero_port, request));
    auto oversized = wire;
    oversized.resize(cd::kMaximumDatagramBytes + 1);
    assert(!cd::decode_reply(oversized, request));
    auto trailing = wire;
    trailing.push_back(std::byte{0});
    assert(!cd::decode_reply(trailing, request));
    for (const char magic_end : {'D', 'P'}) {
        auto managed = request_wire;
        managed[2] = std::byte{'T'};
        managed[3] = static_cast<std::byte>(magic_end);
        assert(!cd::decode_probe(managed));
        auto managed_reply = wire;
        managed_reply[2] = std::byte{'T'};
        managed_reply[3] = static_cast<std::byte>(magic_end);
        assert(!cd::decode_reply(managed_reply, request));
    }

    auto invalid = ad;
    for (const auto& teacher_id : {std::string{}, std::string(97, 'x'), std::string("teacher\n"),
                                   std::string("teacher name"), std::string("../teacher"), std::string("\xc3\xa1")}) {
        invalid = ad;
        invalid.teacher_id = teacher_id;
        assert(cd::encode_reply(request, invalid).empty());
    }
    invalid = ad;
    invalid.teacher_id = "teacher-" + std::string(88, 'a');
    assert(cd::decode_reply(cd::encode_reply(request, invalid), request) == invalid);
    auto legacy_probe = request_wire;
    legacy_probe[4] = std::byte{1};
    assert(!cd::decode_probe(legacy_probe));
    auto legacy_reply = wire;
    legacy_reply[4] = std::byte{1};
    assert(!cd::decode_reply(legacy_reply, request));
    invalid = ad;
    invalid.control_port = 0;
    assert(cd::encode_reply(request, invalid).empty());
    invalid = ad;
    invalid.protocol_version = 2;
    assert(cd::encode_reply(request, invalid).empty());
    invalid = ad;
    invalid.join_state = static_cast<cd::JoinState>(3);
    assert(cd::encode_reply(request, invalid).empty());
    for (const auto& id : {std::string(32, '0'), std::string(32, 'A'),
                           std::string(31, '1'), std::string(32, 'g')}) {
        invalid = ad;
        invalid.room_id = id;
        assert(cd::encode_reply(request, invalid).empty());
        invalid = ad;
        invalid.round_id = id;
        assert(cd::encode_reply(request, invalid).empty());
    }
    const std::vector<std::string> bad_labels{
        "", std::string(65, 'x'), "line\nfeed", std::string("zero\0tail", 9),
        "\x7f", "\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80",
        "\xe2\x82", "\x80", "\xc2\x85", "\xe2\x80\xae"};
    for (const auto& label : bad_labels) {
        invalid = ad;
        invalid.title = label;
        assert(cd::encode_reply(request, invalid).empty());
        invalid = ad;
        invalid.teacher_label = label;
        assert(cd::encode_reply(request, invalid).empty());
    }
    invalid = ad;
    invalid.title.assign(64, 'x');
    invalid.teacher_label.assign(64, 'y');
    assert(cd::decode_reply(cd::encode_reply(request, invalid), request) == invalid);
    auto bad_utf8_wire = wire;
    bad_utf8_wire[cd::kReplyHeaderBytes] = std::byte{0xff};
    assert(!cd::decode_reply(bad_utf8_wire, request));
    auto invalid_id_wire = wire;
    invalid_id_wire[50] = std::byte{'A'};
    assert(!cd::decode_reply(invalid_id_wire, request));
    invalid_id_wire = wire;
    std::fill_n(invalid_id_wire.begin() + 82, 32, std::byte{'0'});
    assert(!cd::decode_reply(invalid_id_wire, request));
    auto bad_length_wire = wire;
    bad_length_wire[46] = std::byte{65};
    assert(!cd::decode_reply(bad_length_wire, request));
    std::cout << "codec: nonce/domain/version/length/UTF-8/IDs/ports passed\n";
}

void candidate_tests() {
    const auto request = probe();
    const auto started = std::chrono::steady_clock::time_point{10s};
    const auto wire = cd::encode_reply(request, advertisement());
    cd::CandidateSet set(request, started);
    assert(set.accept(wire, "127.0.0.1", started));
    assert(!set.accept(wire, "127.0.0.1", started + 1ms));
    assert(!set.accept(wire, "127.0.0.2", started + 1ms));
    const auto same_endpoint = cd::encode_reply(request, advertisement('3'));
    assert(!set.accept(same_endpoint, "127.0.0.1", started));
    assert(!set.accept(same_endpoint, "hostname", started));
    assert(!set.accept(same_endpoint, "0.0.0.0", started));
    assert(!set.accept(same_endpoint, "224.0.0.1", started));
    assert(!set.accept(same_endpoint, "127.0.0.2", started - 1ms));
    assert(!set.accept(same_endpoint, "127.0.0.2", started + cd::kProbeBudget));
    assert(set.candidates(started + cd::kHintLifetime - 1ms).size() == 1);
    assert(set.candidates(started + cd::kHintLifetime).empty());

    cd::CandidateSet capped(request, started, 999);
    for (std::size_t i = 1; i <= cd::kMaximumCandidates + 1; ++i) {
        auto ad = advertisement();
        ad.room_id = std::string(30, '1') + "0123456789abcdef"[i / 16] +
                     "0123456789abcdef"[i % 16];
        ad.control_port = static_cast<std::uint16_t>(47002 + i);
        const auto accepted = capped.accept(cd::encode_reply(request, ad),
                                             "127.0.0.1", started);
        assert(accepted == (i <= cd::kMaximumCandidates));
    }
    assert(capped.candidates(started).size() == cd::kMaximumCandidates);
    std::cout << "candidates: endpoint/room dedup, budget, expiry, cap passed\n";
}

#if defined(_WIN32)
class Socket {
public:
    Socket() : value_(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) {
        assert(value_ != INVALID_SOCKET);
    }
    ~Socket() { closesocket(value_); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    SOCKET get() const { return value_; }
private:
    SOCKET value_;
};

sockaddr_in loopback(std::uint16_t port) {
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(port);
    assert(InetPtonA(AF_INET, "127.0.0.1", &target.sin_addr) == 1);
    return target;
}

void send(SOCKET socket, const sockaddr_in& target,
          std::span<const std::byte> wire) {
    assert(sendto(socket, reinterpret_cast<const char*>(wire.data()),
                  static_cast<int>(wire.size()), 0,
                  reinterpret_cast<const sockaddr*>(&target), sizeof(target)) ==
           static_cast<int>(wire.size()));
}

void native_tests() {
    nstu::net::WinsockRuntime winsock;
    assert(winsock.ready());
    cd::RoomDiscoveryResponder first;
    cd::RoomDiscoveryResponder second;
    std::string error;
    assert(first.start(0, &error));
    assert(second.start(0, &error));
    assert(first.running() && first.local_port() != 0);
    assert(!first.start(0, &error));
    cd::DiscoveryOptions options;
    options.timeout = 120ms;
    options.manual_targets = {{"127.0.0.1", first.local_port()},
                              {"127.0.0.1", second.local_port()}};
    assert(cd::discover_rooms(options, &error).empty());
    auto one = advertisement();
    auto two = advertisement('3');
    two.control_port = 47003;
    two.teacher_id = "teacher-02";
    assert(first.set_beacon(one, &error));
    assert(second.set_beacon(two, &error));
    auto rooms = cd::discover_rooms(options, &error);
    assert(error.empty() && rooms.size() == 2);
    assert(std::any_of(rooms.begin(), rooms.end(), [](const auto& candidate) {
        return candidate.room.teacher_id == "teacher-01";
    }));
    assert(std::any_of(rooms.begin(), rooms.end(), [](const auto& candidate) {
        return candidate.room.teacher_id == "teacher-02";
    }));
    assert(std::any_of(rooms.begin(), rooms.end(), [](const auto& candidate) {
        return candidate.room.control_port == 47002;
    }));
    assert(std::any_of(rooms.begin(), rooms.end(), [](const auto& candidate) {
        return candidate.room.control_port == 47003;
    }));
    options.maximum_candidates = 1;
    assert(cd::discover_rooms(options, &error).size() == 1);
    options.maximum_candidates = cd::kMaximumCandidates;
    one.join_state = cd::JoinState::join_closed;
    assert(first.set_beacon(one, &error));
    assert(second.set_beacon(std::nullopt, &error));
    rooms = cd::discover_rooms(options, &error);
    assert(rooms.size() == 1 && rooms[0].room.join_state == cd::JoinState::join_closed);
    one.title = "bad\nlabel";
    assert(!first.set_beacon(one, &error));
    assert(cd::discover_rooms(options, &error).empty());
    first.stop();
    second.stop();
    assert(!first.running() && first.local_port() == 0);
    assert(first.start(0, &error));
    options.manual_targets = {{"127.0.0.1", first.local_port()}};
    assert(cd::discover_rooms(options, &error).empty());
    options.manual_targets = {{"127.0.0.1", 0}};
    assert(cd::discover_rooms(options, &error).empty() && !error.empty());
    options.manual_targets = {{"localhost", 47002}};
    assert(cd::discover_rooms(options, &error).empty() && !error.empty());
    options.manual_targets.assign(33, {"127.0.0.1", 47002});
    assert(cd::discover_rooms(options, &error).empty() && !error.empty());
    options.manual_targets = {{"127.0.0.1", first.local_port()}};
    options.timeout = 10s;
    const auto before = std::chrono::steady_clock::now();
    assert(cd::discover_rooms(options, &error).empty());
    const auto elapsed = std::chrono::steady_clock::now() - before;
    assert(elapsed >= 1s && elapsed < 2s);
    first.stop();

    cd::RoomDiscoveryResponder limited;
    assert(limited.start(0, &error));
    assert(limited.set_beacon(advertisement(), &error));
    Socket sender;
    const auto target = loopback(limited.local_port());
    const auto request_wire = cd::encode_probe(probe());
    for (int i = 0; i < 100; ++i) send(sender.get(), target, request_wire);
    std::this_thread::sleep_for(150ms);
    assert(limited.beacons_sent() > 0 && limited.beacons_sent() <= 8);
    limited.stop();
    std::cout << "native UDP: two independent responders, disable/update/restart, budget, rate limit passed\n";

    Socket rogue;
    auto local = loopback(0);
    assert(bind(rogue.get(), reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == 0);
    int bytes = sizeof(local);
    assert(getsockname(rogue.get(), reinterpret_cast<sockaddr*>(&local), &bytes) == 0);
    std::jthread worker([&] {
        std::array<std::byte, cd::kMaximumDatagramBytes> buffer{};
        sockaddr_in source{};
        int source_bytes = sizeof(source);
        const auto received = recvfrom(rogue.get(), reinterpret_cast<char*>(buffer.data()),
                                      static_cast<int>(buffer.size()), 0,
                                      reinterpret_cast<sockaddr*>(&source), &source_bytes);
        assert(received == static_cast<int>(cd::kProbeBytes));
        const auto request = cd::decode_probe(std::span(buffer).first(static_cast<std::size_t>(received)));
        assert(request);
        auto wrong_nonce = *request;
        wrong_nonce.nonce[0] ^= std::byte{1};
        send(rogue.get(), source, cd::encode_reply(wrong_nonce, advertisement()));
        auto bad = cd::encode_reply(*request, advertisement());
        bad[2] = std::byte{'T'};
        bad[3] = std::byte{'P'};
        send(rogue.get(), source, bad);
        bad.resize(cd::kMaximumDatagramBytes + 1);
        send(rogue.get(), source, bad);
        const auto good = cd::encode_reply(*request, advertisement());
        send(rogue.get(), source, good);
        send(rogue.get(), source, good);
    });
    options.timeout = 150ms;
    options.manual_targets = {{"127.0.0.1", ntohs(local.sin_port)}};
    rooms = cd::discover_rooms(options, &error);
    assert(rooms.size() == 1 && rooms[0].room == advertisement());
    worker.join();
    std::cout << "native UDP: wrong nonce/managed/oversize replies rejected; duplicate suppressed\n";
}
#endif
} // namespace

int main() {
    codec_tests();
    candidate_tests();
#if defined(_WIN32)
    native_tests();
#endif
}
