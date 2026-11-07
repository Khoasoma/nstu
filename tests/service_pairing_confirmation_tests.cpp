#include "nstu/agent_protocol.hpp"
#include "nstu/client_pairing.hpp"
#include "nstu/control_plane.hpp"
#include "nstu/keyring.hpp"
#include "nstu/multicast.hpp"
#include "nstu/service_pairing_confirmation.hpp"
#include "nstu/agent_pairing_confirmation.hpp"
#include "../server/app/pairing_request_view.hpp"

#include <windows.h>

#include <cassert>
#include <atomic>
#include <algorithm>
#include <initializer_list>
#include <iostream>
#include <vector>
#include <filesystem>
#include <chrono>
#include <thread>
#include <limits>
#include <stdexcept>

namespace {

std::vector<std::byte> bytes(std::initializer_list<unsigned int> values) {
    std::vector<std::byte> output;
    for (const auto value : values) output.push_back(static_cast<std::byte>(value));
    return output;
}

void verify_confirmation_frame_types() {
    // Old decoders reject even a complete kind-42 request. New confirmation
    // kinds must additionally reject empty/malformed payloads at the frame
    // boundary; existing tests already require the empty kind-42 rejection.
    const std::vector<std::vector<std::byte>> payloads{
        bytes({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
               8, 7, 6, 5, 4, 3, 2, 1,
               6, 0, '1', '2', '3', '4', '5', '6', 3, 0, 'L', 'a', 'b'}),
        bytes({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
               1, 1, 6, 0, '1', '2', '3', '4', '5', '6'}),
        bytes({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16})};
    for (std::size_t index = 0; index < payloads.size(); ++index) {
        auto wire = bytes({0x4e, 0x49, 0x50, 0x43, 1, 0, 42, 0, 0, 0, 0, 0});
        const auto kind = static_cast<unsigned int>(42 + index);
        wire[6] = static_cast<std::byte>(kind);
        assert(!nstu::client::decode_agent_message(wire));
        assert(nstu::client::encode_agent_message(
            {static_cast<nstu::client::AgentMessageType>(kind), {}}).empty());
        wire[8] = static_cast<std::byte>(payloads[index].size());
        wire.insert(wire.end(), payloads[index].begin(), payloads[index].end());
        const auto decoded = nstu::client::decode_agent_message(wire);
        assert(decoded.has_value());
        assert(static_cast<unsigned int>(decoded->type) == kind);
        assert(nstu::client::encode_agent_message(*decoded) == wire);
        wire.back() = std::byte{0};
        if (kind != 44) assert(!nstu::client::decode_agent_message(wire));
    }
    const auto unknown = bytes({0x4e, 0x49, 0x50, 0x43, 1, 0, 45, 0, 0, 0, 0, 0});
    assert(!nstu::client::decode_agent_message(unknown));
}

void verify_existing_pairing_payload_compatibility() {
    using namespace nstu::client;
    assert(encode_agent_pairing_selection(3) == bytes({3, 0}));
    const std::vector<AgentPairingChoice> choices{{"Lab", "127.0.0.1", 47001}};
    assert(encode_agent_pairing_choices(choices) ==
           bytes({1, 3, 0, 'L', 'a', 'b', 9, 0, '1', '2', '7', '.', '0', '.',
                  '0', '.', '1', 0x99, 0xb7}));
    assert(encode_agent_pairing_code({"123456", "Lab"}) ==
           bytes({6, 0, '1', '2', '3', '4', '5', '6', 3, 0, 'L', 'a', 'b'}));
    assert(encode_agent_pairing_status({2, "No"}) == bytes({2, 2, 0, 'N', 'o'}));
    const AgentMessage old{AgentMessageType::pairing_select, bytes({3, 0})};
    assert(encode_agent_message(old) ==
           bytes({0x4e, 0x49, 0x50, 0x43, 1, 0, 26, 0, 2, 0, 0, 0, 3, 0}));
}

nstu::client::AgentPairingAttemptToken attempt_token() {
    nstu::client::AgentPairingAttemptToken token{};
    for (std::size_t index = 0; index < token.size(); ++index)
        token[index] = static_cast<std::byte>(index + 1);
    return token;
}

void verify_request_codec() {
    using namespace nstu::client;
    const auto wire = bytes({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
                            8, 7, 6, 5, 4, 3, 2, 1,
                            6, 0, '1', '2', '3', '4', '5', '6', 3, 0, 'L', 'a', 'b'});
    AgentPairingConfirmationRequest request{attempt_token(), 0x0102030405060708ull,
                                           "123456", "Lab"};
    assert(encode_agent_pairing_confirmation_request(request) == wire);
    const auto decoded = decode_agent_pairing_confirmation_request(wire);
    assert(decoded && decoded->attempt_token == attempt_token());
    assert(decoded->expires_at_ms == 0x0102030405060708ull);
    assert(decoded->code == "123456" && decoded->server_name == "Lab");
    for (std::size_t size = 0; size < wire.size(); ++size)
        assert(!decode_agent_pairing_confirmation_request(std::span(wire).first(size)));
    auto malformed = wire;
    malformed.push_back(std::byte{0});
    assert(!decode_agent_pairing_confirmation_request(malformed));
    malformed = wire;
    std::fill_n(malformed.begin(), 16, std::byte{0});
    assert(!decode_agent_pairing_confirmation_request(malformed));
    malformed = wire;
    std::fill_n(malformed.begin() + 16, 8, std::byte{0});
    assert(!decode_agent_pairing_confirmation_request(malformed));
    for (const std::size_t offset : {std::size_t{26}, std::size_t{34}}) {
        malformed = wire;
        malformed[offset] = std::byte{0x0a};
        assert(!decode_agent_pairing_confirmation_request(malformed));
    }
    malformed = wire;
    malformed[32] = std::byte{129};
    malformed.resize(34 + 129, std::byte{'X'});
    assert(!decode_agent_pairing_confirmation_request(malformed));
    request.attempt_token = {};
    assert(encode_agent_pairing_confirmation_request(request).empty());
    request.attempt_token = attempt_token();
    request.expires_at_ms = 0;
    assert(encode_agent_pairing_confirmation_request(request).empty());
    request.expires_at_ms = 1;
    request.code = "12345";
    assert(encode_agent_pairing_confirmation_request(request).empty());
    request.code = "123456";
    request.server_name.assign(129, 'X');
    assert(encode_agent_pairing_confirmation_request(request).empty());
    request.server_name = "";
    assert(encode_agent_pairing_confirmation_request(request).empty());
}

void verify_reply_and_end_codec() {
    using namespace nstu::client;
    using Choice = AgentPairingConfirmationChoice;
    const auto wire = bytes({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
                            1, 1, 6, 0, '1', '2', '3', '4', '5', '6'});
    AgentPairingConfirmationReply reply{attempt_token(), Choice::confirmed, true, "123456"};
    assert(encode_agent_pairing_confirmation_reply(reply) == wire);
    const auto decoded = decode_agent_pairing_confirmation_reply(wire);
    assert(decoded && decoded->attempt_token == attempt_token());
    assert(decoded->choice == Choice::confirmed && decoded->code_shown);
    assert(decoded->confirmed_code == "123456");
    for (std::size_t size = 0; size < wire.size(); ++size)
        assert(!decode_agent_pairing_confirmation_reply(std::span(wire).first(size)));
    auto malformed = wire;
    malformed.push_back(std::byte{0});
    assert(!decode_agent_pairing_confirmation_reply(malformed));
    for (const unsigned int choice : {0u, 7u, 255u}) {
        malformed = wire;
        malformed[16] = static_cast<std::byte>(choice);
        assert(!decode_agent_pairing_confirmation_reply(malformed));
    }
    for (const unsigned int flag : {0u, 2u, 255u}) {
        malformed = wire;
        malformed[17] = static_cast<std::byte>(flag);
        assert(!decode_agent_pairing_confirmation_reply(malformed));
    }
    malformed = wire;
    malformed[20] = std::byte{'x'};
    assert(!decode_agent_pairing_confirmation_reply(malformed));
    malformed = wire;
    malformed[18] = std::byte{128};
    malformed.resize(20 + 128, std::byte{'1'});
    assert(!decode_agent_pairing_confirmation_reply(malformed));
    malformed = wire;
    std::fill_n(malformed.begin(), 16, std::byte{0});
    assert(!decode_agent_pairing_confirmation_reply(malformed));
    reply.confirmed_code = "";
    assert(encode_agent_pairing_confirmation_reply(reply).empty());
    reply.confirmed_code = "123456";
    reply.code_shown = false;
    assert(encode_agent_pairing_confirmation_reply(reply).empty());
    for (const auto choice : {Choice::rejected, Choice::mismatched_code,
                             Choice::timed_out, Choice::no_ui, Choice::cancelled}) {
        reply.choice = choice;
        reply.confirmed_code.clear();
        const auto refused = decode_agent_pairing_confirmation_reply(
            encode_agent_pairing_confirmation_reply(reply));
        assert(refused && refused->choice == choice && !refused->code_shown);
        assert(refused->confirmed_code.empty());
    }
    reply.choice = static_cast<Choice>(7);
    assert(encode_agent_pairing_confirmation_reply(reply).empty());
    reply.choice = Choice::rejected;
    reply.confirmed_code = "12345x";
    assert(encode_agent_pairing_confirmation_reply(reply).empty());
    const auto end = bytes({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16});
    assert(encode_agent_pairing_confirmation_end(attempt_token()) == end);
    assert(decode_agent_pairing_confirmation_end(end) == attempt_token());
    for (std::size_t size = 0; size < end.size(); ++size)
        assert(!decode_agent_pairing_confirmation_end(std::span(end).first(size)));
    auto invalid_end = end;
    invalid_end.push_back(std::byte{0});
    assert(!decode_agent_pairing_confirmation_end(invalid_end));
    assert(encode_agent_pairing_confirmation_end({}).empty());
    assert(!decode_agent_pairing_confirmation_end(std::vector<std::byte>(16)));
}

class PairingServerFixture {
public:
    PairingServerFixture() : server(registry, key_store) {
        assert(winsock.ready());
        wchar_t directory[MAX_PATH]{};
        assert(GetTempPathW(MAX_PATH, directory) != 0);
        keyring_path = std::filesystem::path(directory) /
            (L"nstu-confirmation-test-" + std::to_wstring(GetCurrentProcessId()) + L".bin");
        nstu::server::ServerControlPlaneConfig config;
        config.port = 0;
        config.keyring_path = keyring_path.wstring();
        config.keyring_entropy = {std::byte{7}, std::byte{1}, std::byte{9}};
        std::string error;
        assert(server.start(std::move(config), &error));
        server.set_pairing_window(true, "Lab");
    }
    ~PairingServerFixture() {
        server.stop();
        (void)DeleteFileW(keyring_path.c_str());
    }
    [[nodiscard]] nstu::discovery::PairingCandidate candidate() const {
        return {"127.0.0.1", server.local_port(), "Lab"};
    }
    nstu::net::WinsockRuntime winsock;
    nstu::security::KeyStore key_store;
    nstu::server::ClientRegistry registry;
    nstu::server::ServerControlPlane server;
    std::filesystem::path keyring_path;
};

void verify_production_local_refusal() {
    PairingServerFixture fixture;
    bool displayed = false;
    std::string error;
    nstu::client::ServicePairingConfirmation broker;
    const auto peer = broker.connect_agent(7);
    const nstu::client::PairingAgentMessageSink reject = [&](nstu::client::AgentMessage message) {
            if (message.type == nstu::client::AgentMessageType::pairing_confirmation_request) {
                const auto request = nstu::client::decode_agent_pairing_confirmation_request(message.payload);
                assert(request);
                nstu::client::AgentPairingConfirmation agent;
                assert(agent.receive(*request, GetTickCount64()));
                assert(agent.mark_presented(request->attempt_token, GetTickCount64()));
                displayed = true;
                const auto reply = agent.choose(request->attempt_token,
                    nstu::client::AgentPairingConfirmationChoice::rejected, {}, GetTickCount64());
                assert(reply);
                assert(broker.receive_reply(peer,
                    nstu::client::encode_agent_pairing_confirmation_reply(*reply)));
            }
            return true;
        };
    const auto result = nstu::client::pair_with_interactive_agent(
        fixture.candidate(), "6F2C41A8-1B7D-4E55-9C03-8A1F27D0B4E6", "LAB-PC-07",
        broker, reject, {}, {}, &error);
    assert(displayed);
    assert(result.outcome == nstu::client::PairingOutcome::student_refused);
    assert(result.config.key_id == 0 && result.config.pre_shared_key.empty());
    assert(fixture.key_store.key_count() == 0);
    assert(fixture.server.pending_pairings().empty());
    nstu::client::PairingAttemptOptions extreme;
    extreme.approval_timeout = std::chrono::seconds::max();
    const auto overflow = nstu::client::pair_with_interactive_agent(
        fixture.candidate(), "6F2C41A8-1B7D-4E55-9C03-8A1F27D0B4E6", "LAB-PC-07",
        broker, reject, {}, extreme, &error);
    assert(overflow.outcome == nstu::client::PairingOutcome::student_refused);
    assert(overflow.config.key_id == 0 && overflow.config.pre_shared_key.empty());
}

void verify_agent_confirmation_state() {
    using namespace nstu::client;
    using Choice = AgentPairingConfirmationChoice;
    AgentPairingConfirmation agent;
    const AgentPairingConfirmationRequest request{attempt_token(), 1000, "123456", "Lab"};
    assert(agent.receive(request, 100));
    const auto unshown = agent.choose(attempt_token(), Choice::confirmed, "123456", 101);
    assert(unshown && unshown->choice == Choice::no_ui && !unshown->code_shown);
    assert(!agent.choose(attempt_token(), Choice::confirmed, "123456", 102));
    for (const std::string& code : {std::string{}, std::string("12345"), std::string("12345x"), std::string("654321")}) {
        assert(agent.receive(request, 100));
        assert(agent.mark_presented(attempt_token(), 101));
        const auto mismatch = agent.choose(attempt_token(), Choice::confirmed, code, 102);
        assert(mismatch && mismatch->choice == Choice::mismatched_code);
    }
    assert(agent.receive(request, 100));
    assert(agent.mark_presented(attempt_token(), 101));
    const auto matched = agent.choose(attempt_token(), Choice::confirmed, "123456", 102);
    assert(matched && matched->choice == Choice::confirmed && matched->code_shown);
    assert(matched->confirmed_code == "123456");
    assert(!agent.choose(attempt_token(), Choice::confirmed, "123456", 103));
    auto next = request;
    next.attempt_token[0] = std::byte{99};
    assert(agent.receive(request, 100));
    assert(agent.receive(next, 101));
    assert(!agent.end(attempt_token()));
    assert(!agent.mark_presented(attempt_token(), 102));
    assert(agent.request() && agent.request()->attempt_token == next.attempt_token);
    assert(agent.end(next.attempt_token));
    assert(!agent.request());
    assert(agent.receive(request, 100));
    assert(!agent.expire(999));
    const auto expired = agent.expire(1000);
    assert(expired && expired->choice == Choice::timed_out);
    assert(!agent.expire(1001));
    assert(agent.receive(request, 100));
    assert(agent.mark_presented(attempt_token(), 101));
    const auto late = agent.choose(attempt_token(), Choice::confirmed, "123456", 1000);
    assert(late && late->choice == Choice::timed_out);
    assert(agent.receive(request, 100));
    agent.disconnect();
    assert(!agent.choose(attempt_token(), Choice::confirmed, "123456", 101));
    assert(!agent.receive(request, 1000));
    next.expires_at_ms = std::numeric_limits<std::uint64_t>::max();
    assert(!agent.receive(next, 1));
    next.expires_at_ms = 180001;
    assert(agent.receive(next, 1));
    next.expires_at_ms = 180002;
    assert(!agent.receive(next, 1));
    assert(!agent.request());
    for (const auto choice : {Choice::rejected, Choice::cancelled, Choice::no_ui}) {
        assert(agent.receive(request, 100));
        const auto refused = agent.choose(attempt_token(), choice, {}, 101);
        assert(refused && refused->choice == choice && refused->confirmed_code.empty());
    }
}

void verify_broker_lifecycle() {
    using namespace nstu::client;
    using namespace std::chrono_literals;
    ServicePairingConfirmation broker;
    assert(broker.connect_agent(0).session_id == 0);
    const auto peer = broker.connect_agent(7);
    assert(peer.session_id == 7 && peer.connection_generation != 0);
    assert(!broker.begin_attempt({}, "123456", "Lab", 1s));
    assert(!broker.begin_attempt(peer, "12345", "Lab", 1s));
    assert(!broker.begin_attempt(peer, "123456", "Lab", 0ms));
    const auto request = broker.begin_attempt(peer, "123456", "Lab", 1s);
    assert(request && request->expires_at_ms > GetTickCount64());
    assert(broker.is_current_request(peer, request->attempt_token));
    auto stale = request->attempt_token;
    stale[0] ^= std::byte{0xff};
    const AgentPairingConfirmationReply old{stale, AgentPairingConfirmationChoice::rejected, false, {}};
    assert(!broker.receive_reply(peer, encode_agent_pairing_confirmation_reply(old)));
    const auto before = std::chrono::steady_clock::now();
    assert(broker.wait(request->attempt_token, before + 1ms, {}).choice == StudentConfirmationChoice::waiting);
    assert(std::chrono::steady_clock::now() - before < 250ms);
    assert(!broker.end_attempt(stale));
    assert(broker.is_current_request(peer, request->attempt_token));
    const auto replacement = broker.connect_agent(8);
    assert(replacement.connection_generation != peer.connection_generation);
    assert(!broker.receive_reply(peer, encode_agent_pairing_confirmation_reply(old)));
    assert(broker.wait(request->attempt_token, std::chrono::steady_clock::now() + 1s, {}).choice == StudentConfirmationChoice::no_ui);
    broker.disconnect_agent(peer); // A late old disconnect cannot drop the new peer.
    assert(broker.peer() == replacement);
    const auto capped = broker.begin_attempt(replacement, "123456", "Lab", std::chrono::milliseconds::max());
    assert(capped && capped->expires_at_ms - GetTickCount64() <= 180000);
    broker.cancel_attempt(capped->attempt_token);
    assert(broker.wait(capped->attempt_token, std::chrono::steady_clock::now() + 1s, {}).choice == StudentConfirmationChoice::rejected);
    assert(broker.end_attempt(capped->attempt_token));
    const auto expiring = broker.begin_attempt(replacement, "123456", "Lab", 1ms);
    assert(expiring);
    std::this_thread::sleep_for(2ms);
    const AgentPairingConfirmationReply late{expiring->attempt_token, AgentPairingConfirmationChoice::confirmed, true, "123456"};
    assert(!broker.receive_reply(replacement, encode_agent_pairing_confirmation_reply(late)));
    assert(broker.wait(expiring->attempt_token, std::chrono::steady_clock::now() + 1s, {}).choice == StudentConfirmationChoice::timed_out);
    assert(broker.end_attempt(expiring->attempt_token));
    const auto active = broker.begin_attempt(replacement, "123456", "Lab", 1s);
    assert(active);
    std::stop_source stop;
    stop.request_stop();
    assert(broker.wait(active->attempt_token, std::chrono::steady_clock::now() + 1s, stop.get_token()).choice == StudentConfirmationChoice::rejected);
    broker.disconnect_agent(replacement);
    assert(!broker.peer().session_id);
    assert(!broker.is_current_request(replacement, active->attempt_token));
    const auto revived = broker.connect_agent(7);
    const auto superseded = broker.begin_attempt(revived, "123456", "Lab", 1s);
    assert(superseded);
    assert(!broker.begin_attempt(revived, "invalid", "Lab", 1s));
    assert(broker.wait(superseded->attempt_token, std::chrono::steady_clock::now() + 1ms, {}).choice == StudentConfirmationChoice::no_ui);
    const auto mismatching = broker.begin_attempt(revived, "123456", "Lab", 1s);
    assert(mismatching);
    const AgentPairingConfirmationReply mismatch{mismatching->attempt_token,
        AgentPairingConfirmationChoice::confirmed, true, "654321"};
    assert(broker.receive_reply(revived, encode_agent_pairing_confirmation_reply(mismatch)));
    assert(broker.wait(mismatching->attempt_token, std::chrono::steady_clock::now() + 1ms, {}).choice == StudentConfirmationChoice::mismatched_code);
    assert(!broker.receive_reply(revived, encode_agent_pairing_confirmation_reply(mismatch)));
    assert(broker.end_attempt(mismatching->attempt_token));
    const auto malformed = broker.begin_attempt(revived, "123456", "Lab", 1s);
    assert(malformed);
    assert(!broker.receive_reply(revived, {}));
    assert(broker.wait(malformed->attempt_token, std::chrono::steady_clock::now() + 1ms, {}).choice == StudentConfirmationChoice::rejected);
    assert(broker.end_attempt(malformed->attempt_token));
    const auto waiting = broker.begin_attempt(revived, "123456", "Lab", 1s);
    assert(waiting);
    std::stop_source asynchronous_stop;
    StudentConfirmation cancelled;
    std::jthread waiter([&] {
        cancelled = broker.wait(waiting->attempt_token, std::chrono::steady_clock::now() + 1s,
                                asynchronous_stop.get_token());
    });
    asynchronous_stop.request_stop();
    waiter.join();
    assert(cancelled.choice == StudentConfirmationChoice::rejected);
    assert(broker.end_attempt(waiting->attempt_token));
    // Unit handoff uses fixed public fixture digits, not a hidden teacher SAS.
    for (const bool cancelled_before_handoff : {false, true}) {
        const auto ready = broker.begin_attempt(revived, "123456", "Lab", 1s);
        assert(ready);
        const AgentPairingConfirmationReply approved{ready->attempt_token,
            AgentPairingConfirmationChoice::confirmed, true, "123456"};
        assert(broker.receive_reply(revived, encode_agent_pairing_confirmation_reply(approved)));
        if (cancelled_before_handoff) broker.cancel_attempt(ready->attempt_token);
        const auto final = broker.finish_attempt(ready->attempt_token);
        assert(final.choice == (cancelled_before_handoff ? StudentConfirmationChoice::rejected
                                                         : StudentConfirmationChoice::confirmed));
        assert(broker.finish_attempt(ready->attempt_token).choice == StudentConfirmationChoice::no_ui);
    }
    const auto disconnected = broker.begin_attempt(revived, "123456", "Lab", 1s);
    assert(disconnected);
    assert(broker.receive_reply(revived, encode_agent_pairing_confirmation_reply(
        {disconnected->attempt_token, AgentPairingConfirmationChoice::confirmed, true, "123456"})));
    broker.disconnect_agent(revived);
    assert(broker.finish_attempt(disconnected->attempt_token).choice == StudentConfirmationChoice::no_ui);
}

void verify_wrapper_failure_paths() {
    using namespace nstu::client;
    PairingServerFixture fixture;
    enum class Failure { no_ui, display, mismatch, timeout, stale, disconnect, cancel, queue_failure };
    for (const auto failure : {Failure::no_ui, Failure::display, Failure::mismatch,
                              Failure::timeout, Failure::stale, Failure::disconnect,
                              Failure::cancel, Failure::queue_failure}) {
        ServicePairingConfirmation broker;
        const auto peer = failure == Failure::no_ui ? PairingAgentPeer{} : broker.connect_agent(7);
        std::stop_source stop;
        PairingAttemptOptions options;
        options.approval_timeout = std::chrono::seconds{1};
        options.heartbeat_interval_ms = 200;
        std::string error;
        const auto result = pair_with_interactive_agent(fixture.candidate(),
            "6F2C41A8-1B7D-4E55-9C03-8A1F27D0B4E6", "LAB-PC-07", broker,
            [&](AgentMessage message) {
                if (message.type != AgentMessageType::pairing_confirmation_request) return true;
                const auto request = decode_agent_pairing_confirmation_request(message.payload);
                assert(request);
                if (failure == Failure::queue_failure) return false;
                if (failure == Failure::disconnect) { broker.disconnect_agent(peer); return true; }
                if (failure == Failure::cancel) { stop.request_stop(); return true; }
                if (failure == Failure::timeout) return true;
                AgentPairingConfirmation agent;
                assert(agent.receive(*request, GetTickCount64()));
                if (failure != Failure::display)
                    assert(agent.mark_presented(request->attempt_token, GetTickCount64()));
                std::string entered = request->code;
                entered[0] = entered[0] == '9' ? '0' : static_cast<char>(entered[0] + 1);
                auto reply = agent.choose(request->attempt_token,
                    failure == Failure::stale ? AgentPairingConfirmationChoice::rejected
                                              : AgentPairingConfirmationChoice::confirmed,
                    entered, GetTickCount64());
                assert(reply);
                if (failure == Failure::stale) reply->attempt_token[0] ^= std::byte{0xff};
                const bool admitted = broker.receive_reply(peer, encode_agent_pairing_confirmation_reply(*reply));
                assert(admitted == (failure != Failure::stale));
                return true;
            }, stop.get_token(), options, &error);
        const auto expected = failure == Failure::cancel ? PairingOutcome::failed :
            failure == Failure::timeout || failure == Failure::stale ? PairingOutcome::timed_out
                                                                    : PairingOutcome::student_refused;
        assert(result.outcome == expected);
        assert(result.config.key_id == 0 && result.config.pre_shared_key.empty());
        assert(fixture.key_store.key_count() == 0);
        assert(fixture.server.pending_pairings().empty());
    }
}

void verify_confirmed_handoff_expiry() {
    using namespace nstu::client;
    using namespace std::chrono_literals;
    ServicePairingConfirmation broker;
    const auto peer = broker.connect_agent(7);
    const auto request = broker.begin_attempt(peer, "123456", "Lab", 200ms);
    assert(request);
    assert(broker.receive_reply(peer, encode_agent_pairing_confirmation_reply(
        {request->attempt_token, AgentPairingConfirmationChoice::confirmed, true, "123456"})));
    std::this_thread::sleep_for(220ms);
    assert(broker.finish_attempt(request->attempt_token).choice == StudentConfirmationChoice::timed_out);
    assert(broker.finish_attempt(request->attempt_token).choice == StudentConfirmationChoice::no_ui);
}

void verify_cleanup_before_handoff() {
    using namespace nstu::client;
    using namespace std::chrono_literals;
    enum class Action { none, stop, replace, cancel, failed_enqueue, exception, expire };
    const std::vector<std::pair<Action, StudentConfirmationChoice>> cases{
        {Action::none, StudentConfirmationChoice::confirmed},
        {Action::stop, StudentConfirmationChoice::rejected},
        {Action::replace, StudentConfirmationChoice::no_ui},
        {Action::cancel, StudentConfirmationChoice::rejected},
        {Action::failed_enqueue, StudentConfirmationChoice::rejected},
        {Action::exception, StudentConfirmationChoice::rejected},
        {Action::expire, StudentConfirmationChoice::timed_out}};
    for (const auto& [action, expected] : cases) {
        ServicePairingConfirmation broker;
        const auto peer = broker.connect_agent(7);
        const auto request = broker.begin_attempt(peer, "123456", "Lab", 200ms);
        assert(request);
        assert(broker.receive_reply(peer, encode_agent_pairing_confirmation_reply(
            {request->attempt_token, AgentPairingConfirmationChoice::confirmed, true, "123456"})));
        std::stop_source stop;
        const auto result = finish_interactive_confirmation(broker, request->attempt_token,
            [&](AgentMessage message) {
                assert(message.type == AgentMessageType::pairing_confirmation_end);
                assert(decode_agent_pairing_confirmation_end(message.payload) == request->attempt_token);
                // Re-entering the broker here also catches a sink call under its lock.
                assert(broker.peer() == peer);
                switch (action) {
                case Action::none: break;
                case Action::stop: stop.request_stop(); break;
                case Action::replace: (void)broker.connect_agent(8); break;
                case Action::cancel: broker.cancel_attempt(request->attempt_token); break;
                case Action::failed_enqueue: return false;
                case Action::exception: throw std::runtime_error("test queue failure");
                case Action::expire: std::this_thread::sleep_for(220ms); break;
                }
                return true;
            }, stop.get_token());
        assert(result.choice == expected);
        assert(broker.finish_attempt(request->attempt_token).choice == StudentConfirmationChoice::no_ui);
    }
}

void verify_confirmed_transport_cancellation() {
    using namespace nstu::client;
    using namespace std::chrono_literals;
    for (int action = 0; action < 5; ++action) {
        ServicePairingConfirmation broker;
        const auto peer = broker.connect_agent(7);
        std::stop_source transport;
        bool callback_ran = false;
        std::stop_callback reentrant(transport.get_token(), [&] {
            (void)broker.peer(); // request_stop must also occur outside locks.
            callback_ran = true;
        });
        const auto request = broker.begin_attempt(peer, "123456", "Lab", 1s, transport);
        assert(request);
        assert(broker.receive_reply(peer, encode_agent_pairing_confirmation_reply(
            {request->attempt_token, AgentPairingConfirmationChoice::confirmed, true, "123456"})));
        assert(!transport.stop_requested());
        if (action == 0) broker.cancel_attempt(request->attempt_token);
        if (action == 1) broker.disconnect_agent(peer);
        if (action == 2) (void)broker.connect_agent(8);
        if (action == 3) (void)broker.begin_attempt(peer, "654321", "Lab", 1s);
        if (action == 4) assert(broker.end_attempt(request->attempt_token));
        assert(transport.stop_requested());
        assert(callback_ran);
    }
}

void verify_teacher_early_view() {
    using namespace nstu::client;
    PairingServerFixture fixture;
    ServicePairingConfirmation broker;
    const auto peer = broker.connect_agent(7);
    bool observed = false;
    std::string error;
    const auto result = pair_with_interactive_agent(fixture.candidate(),
        "6F2C41A8-1B7D-4E55-9C03-8A1F27D0B4E6", "LAB-PC-07", broker,
        [&](AgentMessage message) {
            if (message.type != AgentMessageType::pairing_confirmation_request) return true;
            const auto rows = fixture.server.pairing_requests();
            assert(rows.size() == 1);
            assert(rows[0].stage == nstu::server::PairingRequestStage::awaiting_client_confirmation);
            assert(!nstu::ui::pairing_request_view(rows[0]).can_approve);
            assert(fixture.server.pending_pairings().empty());
            assert(!fixture.server.approve_pairing(rows[0].pairing_id, &error));
            assert(fixture.key_store.key_count() == 0);
            observed = true;
            const auto request = decode_agent_pairing_confirmation_request(message.payload);
            assert(request);
            assert(rows[0].short_authentication_string == request->code);
            const auto reread = fixture.server.pairing_requests();
            assert(reread.size() == 1 && reread[0].pairing_id == rows[0].pairing_id);
            assert(fixture.server.reject_pairing(rows[0].pairing_id, &error));
            assert(fixture.server.pairing_requests().empty());
            assert(broker.receive_reply(peer, encode_agent_pairing_confirmation_reply(
                {request->attempt_token, AgentPairingConfirmationChoice::rejected, false, {}})));
            return true;
        }, {}, {}, &error);
    assert(observed && result.outcome == PairingOutcome::student_refused);
    assert(result.config.key_id == 0 && result.config.pre_shared_key.empty());
    assert(fixture.key_store.key_count() == 0);
}

void verify_teacher_stage_gate() {
    nstu::server::PairingRequest request;
    request.pairing_id = 1;
    request.seconds_remaining = 10;
    request.hostname = "Unverified claimed name";
    request.stage = nstu::server::PairingRequestStage::awaiting_client_confirmation;
    assert(nstu::ui::pairing_request_view(request).awaiting_student);
    assert(!nstu::ui::pairing_request_view(request).can_approve);
    request.stage = nstu::server::PairingRequestStage::awaiting_teacher_approval;
    assert(nstu::ui::pairing_request_view(request).can_approve);
    request.seconds_remaining = 0;
    assert(!nstu::ui::pairing_request_view(request).can_approve);
    request.seconds_remaining = 10;
    request.pairing_id = 0;
    assert(!nstu::ui::pairing_request_view(request).can_approve);
    request.pairing_id = 1;
    request.stage = static_cast<nstu::server::PairingRequestStage>(99);
    assert(!nstu::ui::pairing_request_view(request).can_approve);
}

void verify_bilateral_crypto_from_teacher_api() {
    using namespace nstu::client;
    enum class Completion { success, stop_cleanup, replace_cleanup, expire_cleanup, fail_cleanup, cancel_pending };
    for (const auto completion : {Completion::success, Completion::stop_cleanup,
                                 Completion::replace_cleanup, Completion::expire_cleanup,
                                 Completion::fail_cleanup, Completion::cancel_pending}) {
        PairingServerFixture fixture;
        ServicePairingConfirmation broker;
        const auto peer = broker.connect_agent(7);
        std::stop_source stop;
        PairingAttemptOptions options;
        options.approval_timeout = std::chrono::seconds{2};
        options.heartbeat_interval_ms = 200;
        PairingAttemptResult result;
        std::string client_error;
        std::string teacher_error;
        std::jthread client([&] {
            result = pair_with_interactive_agent(fixture.candidate(),
                "6F2C41A8-1B7D-4E55-9C03-8A1F27D0B4E6", "LAB-PC-07", broker,
                [&](AgentMessage message) {
                    if (message.type == AgentMessageType::pairing_confirmation_end) {
                        if (completion == Completion::stop_cleanup) stop.request_stop();
                        if (completion == Completion::replace_cleanup) (void)broker.connect_agent(8);
                        if (completion == Completion::expire_cleanup)
                            std::this_thread::sleep_for(std::chrono::milliseconds{2100});
                        return completion != Completion::fail_cleanup;
                    }
                    assert(message.type == AgentMessageType::pairing_confirmation_request);
                    const auto prompt = decode_agent_pairing_confirmation_request(message.payload);
                    assert(prompt);
                    const auto teacher = fixture.server.pairing_requests();
                    assert(teacher.size() == 1);
                    assert(teacher[0].stage == nstu::server::PairingRequestStage::awaiting_client_confirmation);
                    assert(fixture.server.pending_pairings().empty());
                    assert(fixture.key_store.key_count() == 0);
                    AgentPairingConfirmation agent;
                    assert(agent.receive(*prompt, GetTickCount64()));
                    assert(agent.mark_presented(prompt->attempt_token, GetTickCount64()));
                    // Human input boundary in this real-crypto test reads the
                    // actual early teacher API, never a hidden test SAS or an
                    // echo of the client's computed prompt.
                    const auto student = agent.choose(prompt->attempt_token,
                        AgentPairingConfirmationChoice::confirmed,
                        teacher[0].short_authentication_string, GetTickCount64());
                    assert(student && student->choice == AgentPairingConfirmationChoice::confirmed);
                    assert(broker.receive_reply(peer, encode_agent_pairing_confirmation_reply(*student)));
                    return true;
                }, stop.get_token(), options, &client_error);
        });
        std::vector<nstu::server::PendingPairing> pending;
        for (int attempt = 0; attempt < 150; ++attempt) {
            pending = fixture.server.pending_pairings();
            if (!pending.empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        assert(pending.size() == 1);
        const auto teacher = fixture.server.pairing_requests();
        assert(teacher.size() == 1 && teacher[0].pairing_id == pending[0].pairing_id);
        assert(teacher[0].short_authentication_string == pending[0].short_authentication_string);
        assert(teacher[0].stage == nstu::server::PairingRequestStage::awaiting_teacher_approval);
        assert(nstu::ui::pairing_request_view(teacher[0]).can_approve);
        assert(fixture.key_store.key_count() == 0); // Student-only approval cannot enroll.
        if (completion == Completion::cancel_pending) broker.disconnect_agent(peer);
        else assert(fixture.server.approve_pairing(pending[0].pairing_id, &teacher_error));
        client.join();
        if (completion == Completion::success) {
            assert(result.outcome == PairingOutcome::enrolled);
            assert(result.config.key_id != 0 && result.config.pre_shared_key.size() == 32);
        } else {
            const auto expected = completion == Completion::expire_cleanup ? PairingOutcome::timed_out
                                                                           : PairingOutcome::failed;
            assert(result.outcome == expected);
            assert(result.config.key_id == 0 && result.config.pre_shared_key.empty());
        }
        // A late client abort cannot undo an already-issued server key. An
        // abort while teacher approval was still pending issues no key.
        assert(fixture.key_store.key_count() == (completion == Completion::cancel_pending ? 0u : 1u));
        for (int attempt = 0; attempt < 100 && !fixture.server.pairing_requests().empty(); ++attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        assert(fixture.server.pairing_requests().empty());
        clear_client_runtime_config(result.config);
    }
}

void verify_sorted_offers_and_window_close() {
    using namespace nstu::client;
    PairingServerFixture fixture;
    std::array<ServicePairingConfirmation, 2> brokers;
    std::array<PairingAttemptResult, 2> results;
    std::array<std::string, 2> errors;
    (void)brokers[0].connect_agent(7);
    (void)brokers[1].connect_agent(8);
    const auto run = [&](std::size_t index, const char* uuid) {
        PairingAttemptOptions options;
        options.approval_timeout = std::chrono::seconds{3};
        results[index] = pair_with_interactive_agent(fixture.candidate(), uuid, "Claimed workstation",
            brokers[index], [](AgentMessage) { return true; }, {}, options, &errors[index]);
    };
    std::jthread first([&] { run(0, "6F2C41A8-1B7D-4E55-9C03-8A1F27D0B4E6"); });
    std::jthread second([&] { run(1, "6F2C41A8-1B7D-4E55-9C03-8A1F27D0B4E7"); });
    std::vector<nstu::server::PairingRequest> offered;
    for (int attempt = 0; attempt < 200; ++attempt) {
        offered = fixture.server.pairing_requests();
        if (offered.size() == 2) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    assert(offered.size() == 2);
    assert(offered[0].pairing_id < offered[1].pairing_id);
    for (const auto& offer : offered) {
        assert(offer.stage == nstu::server::PairingRequestStage::awaiting_client_confirmation);
        assert(!nstu::ui::pairing_request_view(offer).can_approve);
    }
    assert(fixture.server.pending_pairings().empty());
    assert(fixture.key_store.key_count() == 0);
    fixture.server.set_pairing_window(false);
    assert(fixture.server.pairing_requests().empty());
    first.join();
    second.join();
    for (const auto& result : results) {
        assert(result.outcome != PairingOutcome::enrolled);
        assert(result.config.key_id == 0 && result.config.pre_shared_key.empty());
    }
    assert(fixture.key_store.key_count() == 0);
}

void verify_current_reply_revokes_affirmative() {
    using namespace nstu::client;
    ServicePairingConfirmation broker;
    const auto peer = broker.connect_agent(7);
    std::stop_source transport;
    const auto request = broker.begin_attempt(peer, "123456", "Lab",
        std::chrono::seconds{2}, transport);
    assert(request);
    const auto affirmative = encode_agent_pairing_confirmation_reply(
        {request->attempt_token, AgentPairingConfirmationChoice::confirmed, true, "123456"});
    assert(broker.receive_reply(peer, affirmative));
    assert(!broker.receive_reply(peer, affirmative));
    auto stale = request->attempt_token;
    stale[0] ^= std::byte{0xff};
    assert(!broker.receive_reply(peer, encode_agent_pairing_confirmation_reply(
        {stale, AgentPairingConfirmationChoice::cancelled, true, {}})));
    assert(!transport.stop_requested());
    std::stop_callback reenter(transport.get_token(), [&] { assert(broker.peer() == peer); });
    assert(broker.receive_reply(peer, encode_agent_pairing_confirmation_reply(
        {request->attempt_token, AgentPairingConfirmationChoice::cancelled, true, {}})));
    assert(transport.stop_requested());
    assert(!broker.receive_reply(peer, affirmative));
    assert(broker.finish_attempt(request->attempt_token).choice == StudentConfirmationChoice::rejected);
}

struct NativePairingFixture {
    nstu::client::PairingNativeControls controls;
    NativePairingFixture() {
        controls.window = CreateWindowExW(0, L"STATIC", L"NSTU pairing test",
            WS_POPUP, -10000, -10000, 500, 360, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        assert(controls.window);
        const auto child = [&](const wchar_t* type, const wchar_t* text) {
            const auto handle = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE,
                10, 210, 100, 28, controls.window, nullptr, GetModuleHandleW(nullptr), nullptr);
            assert(handle);
            return handle;
        };
        controls.teacher_input = child(L"EDIT", L"");
        controls.match = child(L"BUTTON", L"Codes match");
        controls.differ = child(L"BUTTON", L"Codes differ");
        controls.reject = child(L"BUTTON", L"Reject");
        controls.displayed_token = attempt_token();
        ShowWindow(controls.window, SW_SHOWNOACTIVATE);
    }
    ~NativePairingFixture() { DestroyWindow(controls.window); }
    bool draw_sas(std::string_view code) const {
        const auto device = GetDC(controls.window);
        assert(device);
        const std::wstring text(code.begin(), code.end());
        RECT area{10, 10, 490, 200};
        const bool drawn = DrawTextW(device, text.c_str(), -1, &area, DT_CENTER | DT_SINGLELINE) > 0;
        assert(ReleaseDC(controls.window, device));
        return drawn;
    }
};

void verify_native_input_and_presentation_dispatch() {
    using namespace nstu::client;
    NativePairingFixture native;
    AgentPairingNativeConfirmation agent;
    auto request = AgentPairingConfirmationRequest{attempt_token(), GetTickCount64() + 2000, "123456", "Lab"};
    assert(agent.receive(request, GetTickCount64()));
    SetWindowTextW(native.controls.teacher_input, L"123456");
    auto reply = agent.choose(native.controls, request.attempt_token,
        AgentPairingConfirmationChoice::confirmed, GetTickCount64());
    assert(reply && reply->choice == AgentPairingConfirmationChoice::no_ui);
    for (const wchar_t* input : {L"", L"12345", L"12345x", L"654321", L"1234567"}) {
        assert(agent.receive(request, GetTickCount64()));
        assert(!agent.present(native.controls, request.attempt_token, request.code, false, GetTickCount64()));
        assert(!agent.present(native.controls, request.attempt_token, "654321", true, GetTickCount64()));
        EnableWindow(native.controls.match, FALSE);
        assert(!agent.present(native.controls, request.attempt_token, request.code, true, GetTickCount64()));
        EnableWindow(native.controls.match, TRUE);
        assert(agent.present(native.controls, request.attempt_token, request.code, native.draw_sas(request.code), GetTickCount64()));
        SetWindowTextW(native.controls.teacher_input, input);
        reply = agent.choose(native.controls, request.attempt_token, AgentPairingConfirmationChoice::confirmed, GetTickCount64());
        assert(reply && reply->choice == AgentPairingConfirmationChoice::mismatched_code);
        assert(!agent.request());
    }
    assert(agent.receive(request, GetTickCount64()));
    assert(agent.present(native.controls, request.attempt_token, request.code, native.draw_sas(request.code), GetTickCount64()));
    SetWindowTextW(native.controls.teacher_input, L"123456");
    reply = agent.choose(native.controls, request.attempt_token, AgentPairingConfirmationChoice::confirmed, GetTickCount64());
    assert(reply && reply->choice == AgentPairingConfirmationChoice::confirmed && reply->confirmed_code == "123456");
    assert(agent.awaiting_teacher() && agent.request()->code == "123456");
    assert(!agent.choose(native.controls, request.attempt_token, AgentPairingConfirmationChoice::confirmed, GetTickCount64()));
    auto stale = request.attempt_token;
    stale[0] ^= std::byte{0xff};
    assert(!agent.end(stale));
    assert(agent.request());
    assert(!agent.choose(native.controls, stale, AgentPairingConfirmationChoice::cancelled, GetTickCount64()));
    reply = agent.choose(native.controls, request.attempt_token, AgentPairingConfirmationChoice::cancelled, GetTickCount64());
    assert(reply && reply->choice == AgentPairingConfirmationChoice::cancelled && !agent.request());
    assert(agent.receive(request, GetTickCount64()));
    assert(agent.present(native.controls, request.attempt_token, request.code, native.draw_sas(request.code), GetTickCount64()));
    ShowWindow(native.controls.window, SW_HIDE);
    reply = agent.choose(native.controls, request.attempt_token, AgentPairingConfirmationChoice::confirmed, GetTickCount64());
    assert(reply && reply->choice == AgentPairingConfirmationChoice::no_ui);
    ShowWindow(native.controls.window, SW_SHOWNOACTIVATE);
    for (const auto choice : {AgentPairingConfirmationChoice::cancelled, AgentPairingConfirmationChoice::rejected,
                             AgentPairingConfirmationChoice::mismatched_code}) {
        assert(agent.receive(request, GetTickCount64()));
        reply = agent.choose(native.controls, request.attempt_token, choice, GetTickCount64());
        assert(reply && reply->choice == choice && !agent.request());
    }
    assert(agent.receive(request, GetTickCount64()));
    reply = agent.expire(request.expires_at_ms);
    assert(reply && reply->choice == AgentPairingConfirmationChoice::timed_out);
    assert(agent.receive(request, GetTickCount64()));
    agent.disconnect();
    assert(!agent.choose(native.controls, request.attempt_token, AgentPairingConfirmationChoice::confirmed, GetTickCount64()));
    assert(agent.receive(request, GetTickCount64()));
    assert(agent.end(request.attempt_token));
    assert(!agent.request());
    // Pipe supersession precedes its posted native update: an old edit/code
    // must not present or approve the new token even if the six digits repeat.
    auto replacement = request;
    replacement.attempt_token[0] ^= std::byte{0xff};
    assert(agent.receive(replacement, GetTickCount64()));
    SetWindowTextW(native.controls.teacher_input, L"123456");
    assert(!agent.present(native.controls, replacement.attempt_token, replacement.code,
        native.draw_sas(replacement.code), GetTickCount64()));
    reply = agent.choose(native.controls, replacement.attempt_token,
        AgentPairingConfirmationChoice::confirmed, GetTickCount64());
    assert(reply && reply->choice == AgentPairingConfirmationChoice::no_ui);
    assert(agent.receive(replacement, GetTickCount64()));
    native.controls.displayed_token = replacement.attempt_token;
    SetWindowTextW(native.controls.teacher_input, L"");
    assert(agent.present(native.controls, replacement.attempt_token, replacement.code,
        native.draw_sas(replacement.code), GetTickCount64()));
    SetWindowTextW(native.controls.teacher_input, L"123456");
    reply = agent.choose(native.controls, replacement.attempt_token,
        AgentPairingConfirmationChoice::confirmed, GetTickCount64());
    assert(reply && reply->choice == AgentPairingConfirmationChoice::confirmed);
    reply = agent.expire(replacement.expires_at_ms);
    assert(reply && reply->choice == AgentPairingConfirmationChoice::cancelled && !agent.request());
}

void verify_pairing_queue_lifecycle() {
    using namespace nstu::client;
    ServicePairingConfirmation broker;
    const auto peer = broker.connect_agent(7);
    const auto request = broker.begin_attempt(peer, "123456", "Lab", std::chrono::seconds{2});
    assert(request);
    const AgentMessage prompt{AgentMessageType::pairing_confirmation_request,
        encode_agent_pairing_confirmation_request(*request)};
    std::deque<AgentMessage> queue{{AgentMessageType::lock, {}}, {AgentMessageType::unlock, {}}};
    assert(!enqueue_pairing_agent_message(broker, peer, queue, 2, prompt));
    assert(queue.size() == 2 && queue[0].type == AgentMessageType::lock && queue[1].type == AgentMessageType::unlock);
    assert(enqueue_pairing_agent_message(broker, peer, queue, 3, prompt));
    assert(pairing_agent_message_current(broker, peer, queue.back()));
    broker.cancel_attempt(request->attempt_token);
    assert(!pairing_agent_message_current(broker, peer, queue.back()));
    const AgentMessage end{AgentMessageType::pairing_confirmation_end,
        encode_agent_pairing_confirmation_end(request->attempt_token)};
    assert(enqueue_pairing_agent_message(broker, peer, queue, 3, end));
    assert(queue.size() == 3 && queue.back().type == AgentMessageType::pairing_confirmation_end);
    (void)broker.finish_attempt(request->attempt_token);
    assert(pairing_agent_message_current(broker, peer, queue.back()));
    const auto replacement = broker.connect_agent(7);
    assert(!pairing_agent_message_current(broker, peer, queue.back()));
    assert(!enqueue_pairing_agent_message(broker, peer, queue, 4, end));
    discard_pairing_confirmation_messages(queue);
    assert(queue.size() == 2 && queue[0].type == AgentMessageType::lock && queue[1].type == AgentMessageType::unlock);
    const auto newer = broker.begin_attempt(replacement, "654321", "Lab", std::chrono::seconds{2});
    assert(newer);
    queue.push_back({AgentMessageType::pairing_confirmation_reply, encode_agent_pairing_confirmation_reply(
        {request->attempt_token, AgentPairingConfirmationChoice::cancelled, true, {}})});
    queue.push_back({AgentMessageType::pairing_confirmation_reply, encode_agent_pairing_confirmation_reply(
        {newer->attempt_token, AgentPairingConfirmationChoice::cancelled, true, {}})});
    discard_pairing_confirmation_messages(queue, request->attempt_token);
    assert(queue.size() == 3);
    assert(decode_agent_pairing_confirmation_reply(queue.back().payload)->attempt_token == newer->attempt_token);
}

struct NativeResetBoundary {
    HWND input;
    WNDPROC previous = nullptr;
    std::optional<nstu::client::AgentPairingAttemptToken>& binding;
    int mode = 1;
    bool saw_unbound_reset = false;
    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        const auto state = reinterpret_cast<NativeResetBoundary*>(GetPropW(window, L"NstuPairingResetBoundary"));
        assert(state);
        if (message == WM_SETTEXT && lparam && *reinterpret_cast<const wchar_t*>(lparam) == L'\0') {
            state->saw_unbound_reset = !state->binding.has_value();
            // Both modes preserve the real old EDIT text: mode1 fails the
            // native reset, mode2 claims success without performing it.
            if (state->mode) return state->mode == 1 ? FALSE : TRUE;
        }
        return CallWindowProcW(state->previous, window, message, wparam, lparam);
    }
    NativeResetBoundary(HWND control, std::optional<nstu::client::AgentPairingAttemptToken>& token)
        : input(control), binding(token) {
        assert(SetPropW(input, L"NstuPairingResetBoundary", reinterpret_cast<HANDLE>(this)));
        previous = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(input, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(&proc)));
        assert(previous);
    }
    ~NativeResetBoundary() {
        SetWindowLongPtrW(input, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(previous));
        RemovePropW(input, L"NstuPairingResetBoundary");
    }
};

void verify_native_reset_failure_unbinds() {
    using namespace nstu::client;
    NativePairingFixture native;
    AgentPairingNativeConfirmation agent;
    auto replacement = AgentPairingConfirmationRequest{attempt_token(), GetTickCount64() + 2000, "123456", "Lab"};
    replacement.attempt_token[0] ^= std::byte{0xff};
    NativeResetBoundary reset(native.controls.teacher_input, native.controls.displayed_token);
    for (const int failure : {1, 2}) {
        native.controls.displayed_token = attempt_token();
        assert(SetWindowTextW(native.controls.teacher_input, L"123456"));
        assert(agent.receive(replacement, GetTickCount64()));
        reset.mode = failure;
        reset.saw_unbound_reset = false;
        assert(!reset_pairing_native_input(native.controls.teacher_input, replacement.attempt_token,
            native.controls.displayed_token));
        assert(reset.saw_unbound_reset && !native.controls.displayed_token);
        wchar_t retained[8]{};
        assert(GetWindowTextW(native.controls.teacher_input, retained, 8) == 6);
        assert(std::wstring_view(retained) == L"123456");
        assert(!agent.present(native.controls, replacement.attempt_token, replacement.code,
            native.draw_sas(replacement.code), GetTickCount64()));
        const auto reply = agent.choose(native.controls, replacement.attempt_token,
            AgentPairingConfirmationChoice::confirmed, GetTickCount64());
        assert(reply && reply->choice == AgentPairingConfirmationChoice::no_ui);
    }
    reset.mode = 0;
    reset.saw_unbound_reset = false;
    assert(agent.receive(replacement, GetTickCount64()));
    assert(reset_pairing_native_input(native.controls.teacher_input, replacement.attempt_token,
        native.controls.displayed_token));
    assert(reset.saw_unbound_reset && native.controls.displayed_token == replacement.attempt_token);
    wchar_t empty[2]{L'?', L'?'};
    assert(GetWindowTextLengthW(native.controls.teacher_input) == 0);
    assert(GetWindowTextW(native.controls.teacher_input, empty, 2) == 0 && empty[0] == L'\0');
    assert(agent.present(native.controls, replacement.attempt_token, replacement.code,
        native.draw_sas(replacement.code), GetTickCount64()));
    const auto without_input = agent.choose(native.controls, replacement.attempt_token,
        AgentPairingConfirmationChoice::confirmed, GetTickCount64());
    assert(without_input && without_input->choice == AgentPairingConfirmationChoice::mismatched_code);
    assert(agent.receive(replacement, GetTickCount64()));
    assert(reset_pairing_native_input(native.controls.teacher_input, replacement.attempt_token,
        native.controls.displayed_token));
    assert(agent.present(native.controls, replacement.attempt_token, replacement.code,
        native.draw_sas(replacement.code), GetTickCount64()));
    assert(SetWindowTextW(native.controls.teacher_input, L"123456"));
    const auto affirmative = agent.choose(native.controls, replacement.attempt_token,
        AgentPairingConfirmationChoice::confirmed, GetTickCount64());
    assert(affirmative && affirmative->choice == AgentPairingConfirmationChoice::confirmed);
    assert(!agent.choose(native.controls, replacement.attempt_token,
        AgentPairingConfirmationChoice::confirmed, GetTickCount64()));
    const auto cancellation = agent.choose(native.controls, replacement.attempt_token,
        AgentPairingConfirmationChoice::cancelled, GetTickCount64());
    assert(cancellation && cancellation->choice == AgentPairingConfirmationChoice::cancelled);
    native.controls.displayed_token = attempt_token();
    assert(!reset_pairing_native_input(nullptr, replacement.attempt_token, native.controls.displayed_token));
    assert(!native.controls.displayed_token);
    const auto destroyed = CreateWindowExW(0, L"EDIT", L"123456", WS_CHILD,
        0, 0, 100, 28, native.controls.window, nullptr, GetModuleHandleW(nullptr), nullptr);
    assert(destroyed && DestroyWindow(destroyed));
    native.controls.displayed_token = attempt_token();
    assert(!reset_pairing_native_input(destroyed, replacement.attempt_token, native.controls.displayed_token));
    assert(!native.controls.displayed_token);
}

// Dedicated pipe only: never opens the installed service's control pipe.
struct PairingPipeFixture {
    nstu::client::ServicePairingConfirmation broker;
    nstu::client::NamedPipe client_pipe;
    std::mutex queue_mutex;
    std::deque<nstu::client::AgentMessage> queue;
    std::atomic_bool connected = false;
    std::atomic_bool listening = false;
    std::atomic_uint replies_admitted = 0;
    std::jthread worker;
    explicit PairingPipeFixture(unsigned int sequence) {
        using namespace nstu::client;
        const auto name = L"\\\\.\\pipe\\nstu-pairing-test-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(sequence);
        wchar_t image[32768]{};
        const auto length = GetModuleFileNameW(nullptr, image, static_cast<DWORD>(std::size(image)));
        assert(length > 0 && length < std::size(image));
        const std::wstring expected_image(image, length);
        worker = std::jthread([&, name, expected_image](std::stop_token stop) {
            NamedPipe service_pipe;
            assert(service_pipe.create_server(name));
            listening = true;
            assert(service_pipe.wait_for_client());
            std::uint32_t session = 0;
            assert(service_pipe.validate_client_process(expected_image, &session));
            const auto peer = broker.connect_agent(session);
            connected = true;
            while (!stop.stop_requested()) {
                std::deque<AgentMessage> pending;
                { std::scoped_lock lock(queue_mutex); pending.swap(queue); }
                for (const auto& message : pending) {
                    if (!pairing_agent_message_current(broker, peer, message)) continue;
                    if (!send_agent_message(service_pipe, message)) break;
                }
                std::uint32_t available = 0;
                if (!service_pipe.available_bytes(available)) break;
                if (available) {
                    const auto reply = receive_agent_message(service_pipe);
                    assert(reply && reply->type == AgentMessageType::pairing_confirmation_reply);
                    std::uint32_t current_session = 0;
                    if (!service_pipe.validate_client_process(expected_image, &current_session) ||
                        current_session != peer.session_id) break;
                    if (broker.receive_reply(peer, reply->payload)) ++replies_admitted;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{2});
            }
            broker.disconnect_agent(peer);
            std::scoped_lock lock(queue_mutex);
            discard_pairing_confirmation_messages(queue);
        });
        for (int attempt = 0; attempt < 300 && !listening; ++attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        assert(listening);
        assert(client_pipe.connect_client(name, 3000));
        for (int attempt = 0; attempt < 300 && !connected; ++attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        assert(connected);
    }
    ~PairingPipeFixture() {
        client_pipe.close();
        worker.request_stop();
        worker.join();
    }
    bool enqueue(nstu::client::AgentMessage message) {
        const auto peer = broker.peer();
        std::scoped_lock lock(queue_mutex);
        return nstu::client::enqueue_pairing_agent_message(broker, peer, queue, 8, std::move(message));
    }
    nstu::client::AgentPairingConfirmationRequest receive_prompt() {
        std::uint32_t available = 0;
        for (int attempt = 0; attempt < 500; ++attempt) {
            assert(client_pipe.available_bytes(available));
            if (available) break;
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        assert(available);
        const auto message = nstu::client::receive_agent_message(client_pipe);
        assert(message && message->type == nstu::client::AgentMessageType::pairing_confirmation_request);
        const auto request = nstu::client::decode_agent_pairing_confirmation_request(message->payload);
        assert(request);
        return *request;
    }
    void send(const nstu::client::AgentPairingConfirmationReply& reply) {
        assert(nstu::client::send_agent_message(client_pipe,
            {nstu::client::AgentMessageType::pairing_confirmation_reply,
             nstu::client::encode_agent_pairing_confirmation_reply(reply)}));
    }
};

void verify_real_pipe_bilateral_dispatch() {
    using namespace nstu::client;
    enum class Action { success, no_presentation, mismatch, reject, cancel_pending, cancel_issued, disconnect };
    unsigned int sequence = 0;
    for (const auto action : {Action::success, Action::no_presentation, Action::mismatch, Action::reject,
                             Action::cancel_pending, Action::cancel_issued, Action::disconnect}) {
        PairingServerFixture teacher;
        PairingPipeFixture pipe(++sequence);
        NativePairingFixture native;
        AgentPairingNativeConfirmation agent;
        PairingAttemptOptions options;
        options.approval_timeout = std::chrono::seconds{3};
        options.heartbeat_interval_ms = 50;
        PairingAttemptResult result;
        std::atomic_bool cleanup_wait = false;
        std::atomic_bool cleanup_release = false;
        std::jthread exchange([&] {
            result = pair_with_interactive_agent(teacher.candidate(),
                "6F2C41A8-1B7D-4E55-9C03-8A1F27D0B4E6", "LAB-PC-07", pipe.broker,
                [&](AgentMessage message) {
                    if (message.type == AgentMessageType::pairing_confirmation_end && action == Action::cancel_issued) {
                        cleanup_wait = true;
                        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
                        while (!cleanup_release && std::chrono::steady_clock::now() < deadline)
                            std::this_thread::sleep_for(std::chrono::milliseconds{2});
                        assert(cleanup_release);
                    }
                    return pipe.enqueue(std::move(message));
                }, {}, options);
        });
        const auto prompt = pipe.receive_prompt();
        assert(agent.receive(prompt, GetTickCount64()));
        native.controls.displayed_token = prompt.attempt_token;
        std::vector<nstu::server::PairingRequest> offered;
        for (int attempt = 0; attempt < 300; ++attempt) {
            offered = teacher.server.pairing_requests();
            if (!offered.empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        assert(offered.size() == 1 && offered[0].short_authentication_string == prompt.code);
        assert(teacher.server.pending_pairings().empty());
        assert(!teacher.server.approve_pairing(offered[0].pairing_id));
        assert(teacher.key_store.key_count() == 0);
        // Deliberate native input comes from the actual early teacher API.
        const std::wstring teacher_digits(offered[0].short_authentication_string.begin(),
                                          offered[0].short_authentication_string.end());
        SetWindowTextW(native.controls.teacher_input, action == Action::mismatch ? L"000000" : teacher_digits.c_str());
        if (action == Action::mismatch && prompt.code == "000000") SetWindowTextW(native.controls.teacher_input, L"111111");
        if (action != Action::no_presentation)
            assert(agent.present(native.controls, prompt.attempt_token, prompt.code,
                native.draw_sas(prompt.code), GetTickCount64()));
        const auto reply = agent.choose(native.controls, prompt.attempt_token,
            action == Action::reject ? AgentPairingConfirmationChoice::rejected : AgentPairingConfirmationChoice::confirmed,
            GetTickCount64());
        assert(reply);
        if (action == Action::disconnect) { pipe.client_pipe.close(); agent.disconnect(); }
        else pipe.send(*reply);
        const bool affirmative = action == Action::success || action == Action::cancel_pending || action == Action::cancel_issued;
        if (affirmative) {
            std::vector<nstu::server::PendingPairing> pending;
            for (int attempt = 0; attempt < 300; ++attempt) {
                pending = teacher.server.pending_pairings();
                if (!pending.empty()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
            }
            assert(pending.size() == 1 && teacher.key_store.key_count() == 0);
            assert(agent.awaiting_teacher() && agent.request()->code == prompt.code);
            assert(!agent.choose(native.controls, prompt.attempt_token, AgentPairingConfirmationChoice::confirmed, GetTickCount64()));
            if (action != Action::cancel_pending) assert(teacher.server.approve_pairing(pending[0].pairing_id));
            if (action == Action::cancel_issued) {
                for (int attempt = 0; attempt < 300 && !cleanup_wait; ++attempt)
                    std::this_thread::sleep_for(std::chrono::milliseconds{5});
                assert(cleanup_wait && teacher.key_store.key_count() == 1);
            }
            if (action == Action::cancel_pending || action == Action::cancel_issued) {
                const auto cancellation = agent.choose(native.controls, prompt.attempt_token,
                    AgentPairingConfirmationChoice::cancelled, GetTickCount64());
                assert(cancellation);
                pipe.send(*cancellation);
                for (int attempt = 0; attempt < 300 && pipe.replies_admitted < 2; ++attempt)
                    std::this_thread::sleep_for(std::chrono::milliseconds{5});
                assert(pipe.replies_admitted == 2);
                cleanup_release = true;
            }
        }
        exchange.join();
        if (action == Action::success) {
            assert(result.outcome == PairingOutcome::enrolled);
            assert(result.config.key_id != 0 && result.config.pre_shared_key.size() == 32);
        } else {
            assert(result.outcome != PairingOutcome::enrolled);
            assert(result.config.key_id == 0 && result.config.pre_shared_key.empty());
        }
        assert(teacher.key_store.key_count() == (action == Action::success || action == Action::cancel_issued ? 1u : 0u));
        clear_client_runtime_config(result.config);
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--reset-only") {
        verify_native_reset_failure_unbinds();
        std::cout << "Pairing native input reset boundary: PASS\n";
        return 0;
    }
    verify_current_reply_revokes_affirmative();
    verify_native_input_and_presentation_dispatch();
    verify_pairing_queue_lifecycle();
    verify_native_reset_failure_unbinds();
    if (argc == 2 && std::string_view(argv[1]) == "--native-only") {
        std::cout << "Pairing native dispatch and queue: PASS\n";
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--pipe-only") {
        verify_real_pipe_bilateral_dispatch();
        std::cout << "Pairing real pipe bilateral dispatch: PASS\n";
        return 0;
    }
    verify_existing_pairing_payload_compatibility();
    verify_confirmation_frame_types();
    verify_request_codec();
    verify_reply_and_end_codec();
    verify_production_local_refusal();
    verify_agent_confirmation_state();
    verify_broker_lifecycle();
    verify_wrapper_failure_paths();
    verify_confirmed_handoff_expiry();
    verify_cleanup_before_handoff();
    verify_confirmed_transport_cancellation();
    verify_teacher_early_view();
    verify_teacher_stage_gate();
    verify_bilateral_crypto_from_teacher_api();
    verify_sorted_offers_and_window_close();
    verify_real_pipe_bilateral_dispatch();
    std::cout << "Pairing confirmation IPC compatibility: PASS\n";
}
