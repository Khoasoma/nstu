#include "nstu/control_channel.hpp"
#include "nstu/control_messages.hpp"
#include "nstu/multicast.hpp"
#include "nstu/secret_store.hpp"
#include "native_deadline_fixture.hpp"

#include <winsock2.h>
#include <windows.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

nstu::security::ClientId client_id() {
    nstu::security::ClientId id{};
    for (std::size_t index = 0; index < id.size(); ++index) {
        id[index] = static_cast<std::byte>(index + 1);
    }
    return id;
}

std::vector<std::byte> pre_shared_key() {
    std::vector<std::byte> key(nstu::security::kMinimumProtocolKeyBytes);
    for (std::size_t index = 0; index < key.size(); ++index) {
        key[index] = static_cast<std::byte>(0x40 + index);
    }
    return key;
}


bool receive_exact(nstu::net::TcpSocket& socket, std::span<std::byte> bytes,
                   std::string* error) {
    std::size_t received = 0;
    while (received < bytes.size()) {
        const int count = socket.receive(bytes.subspan(received), error);
        if (count <= 0) return false;
        received += static_cast<std::size_t>(count);
    }
    return true;
}

// Existing-API regression: WSAEventSelect prevents a blocking-mode restore
// until its association is cleared. A deadline send must report this failure.
void test_deadline_mode_restore_failure() {
    using namespace std::chrono_literals;
    std::string error;
    nstu::net::TcpSocket listener;
    assert(listener.listen(0, 4, &error));
    const auto port = listener.local_port(&error);
    std::promise<void> accepted;
    auto ready = accepted.get_future();
    std::thread receiver([&] {
        std::string receiver_error;
        auto socket = listener.accept(&receiver_error);
        assert(socket.is_open());
        assert(socket.set_io_timeouts(5000, 5000, &receiver_error));
        accepted.set_value();
        std::array<std::byte, 16> received{};
        assert(receive_exact(socket, received, &receiver_error));
        std::array<std::byte, 16> expected{};
        expected.fill(std::byte{0x7a});
        assert(received == expected);
    });
    nstu::net::TcpSocket socket;
    assert(socket.connect("127.0.0.1", port, &error));
    ready.get();
    assert(socket.wait_writable(5000, &error));
    struct Event {
        WSAEVENT handle = WSACreateEvent();
        ~Event() { if (handle != WSA_INVALID_EVENT) (void)WSACloseEvent(handle); }
    } event;
    assert(event.handle != WSA_INVALID_EVENT);
    assert(WSAEventSelect(static_cast<SOCKET>(socket.native_handle()),
                          event.handle, FD_WRITE) == 0);
    std::array<std::byte, 16> data{};
    data.fill(std::byte{0x7a});
    assert(socket.send_all(data, std::chrono::steady_clock::now() + 5s, &error) == -1);
    assert(!socket.is_open());
    assert(!error.empty());
    error.clear();
    assert(!socket.wait_writable(0, &error));
    assert(!error.empty());
    receiver.join();
}

// This scripted socket is the deterministic clock/I/O boundary of the exact
// deadline engine called by TcpSocket. No receiver sleeps or kernel capacity
// assumptions are used to establish partial progress or writable waits.
struct ScriptedDeadlineSocket {
    using Clock = std::chrono::steady_clock;
    struct SendStep { int bytes; bool would_block; std::chrono::milliseconds elapsed{0}; };
    struct WaitStep {
        std::chrono::milliseconds at;
        nstu::net::detail::DeadlineWaitResult result;
    };
    std::vector<SendStep> sends;
    std::vector<WaitStep> waits;
    std::array<Clock::time_point, 16> waited_deadlines{};
    std::array<bool, 4> mode_arguments{};
    std::size_t send_index = 0;
    std::size_t wait_index = 0;
    std::size_t mode_count = 0;
    std::size_t sent_bytes = 0;
    unsigned close_count = 0;
    bool open = true;
    bool blocking = true;
    bool fail_nonblocking = false;
    bool fail_restore = false;
    std::chrono::milliseconds current_time{0};

    [[nodiscard]] bool is_open() const noexcept { return open; }
    [[nodiscard]] Clock::time_point now() const noexcept {
        return Clock::time_point{} + current_time;
    }
    [[nodiscard]] nstu::net::detail::DeadlineSendOperations operations() noexcept {
        return {
            .context = this,
            .now = [](void* context) noexcept {
                return static_cast<ScriptedDeadlineSocket*>(context)->now();
            },
            .set_nonblocking = [](void* context, bool enabled) noexcept {
                auto& self = *static_cast<ScriptedDeadlineSocket*>(context);
                assert(self.mode_count < self.mode_arguments.size());
                self.mode_arguments[self.mode_count++] = enabled;
                if ((enabled && self.fail_nonblocking) || (!enabled && self.fail_restore)) return false;
                self.blocking = !enabled;
                return true;
            },
            .send = [](void* context, std::span<const std::byte> bytes) noexcept
                -> nstu::net::detail::DeadlineSendAttempt {
                auto& self = *static_cast<ScriptedDeadlineSocket*>(context);
                assert(self.open && !self.blocking);
                assert(self.send_index < self.sends.size());
                const auto step = self.sends[self.send_index++];
                self.current_time += step.elapsed;
                if (step.bytes > 0) {
                    assert(static_cast<std::size_t>(step.bytes) <= bytes.size());
                    self.sent_bytes += static_cast<std::size_t>(step.bytes);
                }
                return {step.bytes, step.would_block};
            },
            .wait_writable = [](void* context, Clock::time_point deadline) noexcept {
                auto& self = *static_cast<ScriptedDeadlineSocket*>(context);
                assert(self.open && !self.blocking);
                assert(self.wait_index < self.waits.size());
                assert(self.wait_index < self.waited_deadlines.size());
                self.waited_deadlines[self.wait_index] = deadline;
                const auto step = self.waits[self.wait_index++];
                self.current_time = step.at;
                return step.result;
            },
            .close = [](void* context) noexcept {
                auto& self = *static_cast<ScriptedDeadlineSocket*>(context);
                self.open = false;
                ++self.close_count;
            },
        };
    }
};

void test_scripted_deadline_success_and_failures() {
    using namespace std::chrono_literals;
    using nstu::net::detail::DeadlineWaitResult;
    std::array<std::byte, 16> data{};
    const auto deadline = ScriptedDeadlineSocket::Clock::time_point{} + 120ms;
    std::string error;
    {
        ScriptedDeadlineSocket socket;
        socket.sends = {{7, false}, {-1, true}, {9, false}};
        socket.waits = {{40ms, DeadlineWaitResult::ready}};
        assert(nstu::net::detail::send_all_with_deadline(
                   data, deadline, socket.operations(), &error) == 16);
        assert(socket.is_open());
        assert(socket.blocking);
        assert(socket.sent_bytes == data.size());
        assert(socket.wait_index == 1);
        assert(socket.waited_deadlines[0] == deadline);
        assert(socket.now() == ScriptedDeadlineSocket::Clock::time_point{} + 40ms);
        assert(socket.mode_count == 2);
        assert(socket.mode_arguments[0] && !socket.mode_arguments[1]);
        assert(socket.close_count == 0);
    }
    {
        // A bounded native wait may time out before a very distant absolute
        // deadline. That individual timeout must not disconnect the socket.
        ScriptedDeadlineSocket socket;
        socket.sends = {{-1, true}, {16, false}};
        socket.waits = {{40ms, DeadlineWaitResult::timed_out}};
        const auto distant_deadline = ScriptedDeadlineSocket::Clock::time_point::max();
        assert(nstu::net::detail::send_all_with_deadline(
                   data, distant_deadline, socket.operations(), &error) == 16);
        assert(socket.is_open() && socket.blocking);
        assert(socket.wait_index == 1 && socket.waited_deadlines[0] == distant_deadline);
        assert(socket.close_count == 0);
    }
    {
        ScriptedDeadlineSocket socket;
        socket.sends = {{16, false}};
        socket.fail_restore = true;
        assert(nstu::net::detail::send_all_with_deadline(
                   data, deadline, socket.operations(), &error) == -1);
        assert(!socket.is_open());
        assert(socket.sent_bytes == data.size());
        assert(socket.mode_count == 2);
        assert(socket.close_count == 1);
        assert(error == "TCP blocking mode restore failed after deadline send");
    }
    {
        ScriptedDeadlineSocket socket;
        socket.fail_nonblocking = true;
        assert(nstu::net::detail::send_all_with_deadline(
                   data, deadline, socket.operations(), &error) == -1);
        assert(!socket.is_open());
        assert(socket.send_index == 0);
        assert(socket.close_count == 1);
    }
    {
        ScriptedDeadlineSocket socket;
        socket.sends = {{4, false}, {-1, true}};
        socket.waits = {{40ms, DeadlineWaitResult::failed}};
        assert(nstu::net::detail::send_all_with_deadline(
                   data, deadline, socket.operations(), &error) == -1);
        assert(!socket.is_open());
        assert(socket.sent_bytes == 4);
        assert(socket.waited_deadlines[0] == deadline);
        assert(socket.close_count == 1);
    }
    for (const int result : {0, -1}) {
        ScriptedDeadlineSocket socket;
        socket.sends = {{result, false}};
        assert(nstu::net::detail::send_all_with_deadline(
                   data, deadline, socket.operations(), &error) == -1);
        assert(!socket.is_open());
        assert(socket.close_count == 1);
    }
    {
        ScriptedDeadlineSocket socket;
        assert(nstu::net::detail::send_all_with_deadline(
                   {}, ScriptedDeadlineSocket::Clock::time_point{} - 1ms,
                   socket.operations(), &error) == 0);
        assert(socket.is_open());
        assert(socket.mode_count == 0 && socket.close_count == 0);
    }
}

void test_deadline_wait_duration_bounds() {
    {
        ScriptedDeadlineSocket socket;
        const std::array<std::byte, 16> data{};
        const auto deadline = socket.now() + std::chrono::milliseconds(100);
        socket.sends = {{16, false, std::chrono::milliseconds(100)}};
        std::string error;
        assert(nstu::net::detail::send_all_with_deadline(data, deadline, socket.operations(), &error) == -1);
        assert(!socket.is_open() && socket.close_count == 1);
        assert(error == "TCP send deadline expired");
    }
    using namespace std::chrono_literals;
    const auto short_wait = nstu::net::detail::deadline_wait_duration(1500us);
    assert(short_wait.seconds == 0 && short_wait.microseconds == 1500);
    const auto fractional = nstu::net::detail::deadline_wait_duration(1ns);
    assert(fractional.seconds == 0 && fractional.microseconds == 1);
    const auto elapsed = nstu::net::detail::deadline_wait_duration(-1ms);
    assert(elapsed.seconds == 0 && elapsed.microseconds == 0);
    const auto large = nstu::net::detail::deadline_wait_duration(
        std::chrono::steady_clock::duration::max());
    assert(large.seconds >= 0 && large.seconds <= std::numeric_limits<long>::max());
    assert(large.microseconds >= 0 && large.microseconds < 1'000'000);
    if (std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::duration::max()).count() >
        std::numeric_limits<long>::max()) {
        assert(large.seconds == std::numeric_limits<long>::max());
    }
}

void test_deadline_then_legacy_channel_commands() {
    using namespace std::chrono_literals;
    using nstu::protocol::CommandType;
    std::string error;
    nstu::security::Sha256Digest key{};
    key.fill(std::byte{0x31});
    nstu::net::TcpSocket listener;
    assert(listener.listen(0, 4, &error));
    const auto port = listener.local_port(&error);
    std::promise<void> accepted;
    auto ready = accepted.get_future();
    const std::array<std::byte, 2> first_payload{std::byte{0x10}, std::byte{0x20}};
    const std::array<std::byte, 3> second_payload{std::byte{0x30}, std::byte{0x40}, std::byte{0x50}};
    const std::array<std::byte, 1> response_payload{std::byte{0x01}};
    std::thread server([&] {
        std::string server_error;
        auto socket = listener.accept(&server_error);
        assert(socket.is_open());
        assert(socket.set_io_timeouts(5000, 5000, &server_error));
        nstu::control::AuthenticatedControlChannel channel(std::move(socket), key);
        accepted.set_value();
        for (std::uint64_t sequence = 0; sequence < 3; ++sequence) {
            const auto command = channel.receive(&server_error);
            assert(command.has_value());
            assert(command->sequence == sequence);
            assert(command->envelope.request_id == 300 + sequence);
            if (sequence == 0) {
                assert(command->envelope.type == CommandType::lock);
                assert(command->payload == std::vector<std::byte>(first_payload.begin(), first_payload.end()));
            } else if (sequence == 1) {
                assert(command->envelope.type == CommandType::chat);
                assert(command->payload == std::vector<std::byte>(second_payload.begin(), second_payload.end()));
            } else {
                assert(command->envelope.type == CommandType::unlock);
                assert(command->payload.empty());
            }
            assert(channel.send(CommandType::hello_ack, 300 + sequence,
                                response_payload, &server_error));
        }
    });
    nstu::net::TcpSocket socket;
    assert(socket.connect("127.0.0.1", port, &error));
    assert(socket.set_io_timeouts(5000, 5000, &error));
    ready.get();
    nstu::control::AuthenticatedControlChannel channel(std::move(socket), key);
    // A rejected legacy send must leave the channel usable and sequence zero.
    assert(!channel.send(CommandType::auth_hello, 999, {}, &error));
    assert(channel.is_open());
    assert(channel.send_with_deadline(CommandType::lock, 300, first_payload,
                                     std::chrono::steady_clock::now() + 5s, &error));
    for (std::uint64_t sequence = 0; sequence < 3; ++sequence) {
        if (sequence == 1) assert(channel.send(CommandType::chat, 301, second_payload, &error));
        if (sequence == 2) assert(channel.send(CommandType::unlock, 302, {}, &error));
        const auto response = channel.receive(&error);
        assert(response.has_value());
        assert(response->sequence == sequence);
        assert(response->envelope.type == CommandType::hello_ack);
        assert(response->envelope.request_id == 300 + sequence);
        assert(response->payload == std::vector<std::byte>(response_payload.begin(), response_payload.end()));
    }
    server.join();
    nstu::security::secure_zero(key);
}

void test_send_deadlines() {
    using namespace std::chrono_literals;
    std::string error;

    // 1. TcpSocket: successful send within deadline
    {
        nstu::net::TcpSocket listener;
        assert(listener.listen(0, 4, &error));
        const auto port = listener.local_port(&error);

        std::thread receiver_th([&] {
            std::string rec_err;
            auto server_sock = listener.accept(&rec_err);
            assert(server_sock.is_open());
            std::array<std::byte, 16> buf{};
            assert(server_sock.set_io_timeouts(5000, 5000, &rec_err));
            assert(receive_exact(server_sock, buf, &rec_err));
            std::array<std::byte, 16> expected{};
            expected.fill(std::byte{0x7a});
            assert(buf == expected);
        });

        nstu::net::TcpSocket client_sock;
        assert(client_sock.connect("127.0.0.1", port, &error));
        std::array<std::byte, 16> data{};
        data.fill(std::byte{0x7a});
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        assert(client_sock.send_all(data, deadline, &error) == 16);
        receiver_th.join();
    }

    // 2. TcpSocket: past deadline fails immediately and disconnects
    {
        nstu::net::TcpSocket listener;
        assert(listener.listen(0, 4, &error));
        const auto port = listener.local_port(&error);

        std::thread receiver_th([&] {
            std::string rec_err;
            auto server_sock = listener.accept(&rec_err);
            assert(server_sock.is_open());
        });

        nstu::net::TcpSocket client_sock;
        assert(client_sock.connect("127.0.0.1", port, &error));
        std::array<std::byte, 16> data{};
        const auto past_deadline = std::chrono::steady_clock::now() - 10ms;
        assert(client_sock.send_all(data, past_deadline, &error) == -1);
        assert(!error.empty());
        assert(!client_sock.is_open());
        receiver_th.join();
    }

    // 3. The production deadline engine receives deterministic partial-progress
    // and would-block events at 0/40/80 ms, then times out at the original 120 ms.
    {
        using nstu::net::detail::DeadlineWaitResult;
        ScriptedDeadlineSocket client_sock;
        client_sock.sends = {{16384, false}, {-1, true}, {16384, false},
                             {-1, true}, {16384, false}, {-1, true}};
        client_sock.waits = {{40ms, DeadlineWaitResult::ready},
                             {80ms, DeadlineWaitResult::ready},
                             {120ms, DeadlineWaitResult::timed_out}};
        // Preserve the 512 KiB frame, 16 KiB attempts, 120 ms deadline and
        // original <800 ms bound; the exact elapsed proof is now deterministic.
        std::vector<std::byte> large_payload(512 * 1024, std::byte{0x42});
        const auto start_time = client_sock.now();
        const auto deadline = start_time + 120ms;
        const int sent = nstu::net::detail::send_all_with_deadline(
            large_payload, deadline, client_sock.operations(), &error);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            client_sock.now() - start_time);
        std::atomic<std::size_t> bytes_read_by_receiver = client_sock.sent_bytes;

        assert(sent == -1);
        assert(!client_sock.is_open()); // Disconnects consistently
        assert(bytes_read_by_receiver.load() > 0); // Partial progress occurred
        assert(bytes_read_by_receiver.load() < large_payload.size()); // Did not complete
        assert(elapsed.count() < 800); // Bounded elapsed time
        assert(elapsed == 120ms);
        assert(client_sock.sent_bytes == 3 * 16384);
        assert(client_sock.wait_index == 3);
        assert(client_sock.close_count == 1);
        for (std::size_t index = 0; index < client_sock.wait_index; ++index) {
            assert(client_sock.waited_deadlines[index] == deadline);
        }
    }

    // 4. AuthenticatedControlChannel: whole-frame deadline expiry disconnects channel
    {
        const auto id = client_id();
        auto key = pre_shared_key();
        constexpr std::uint32_t key_id = 42;
        constexpr std::uint64_t now = 20'000;

        nstu::net::TcpSocket listener;
        assert(listener.listen(0, 4, &error));
        const auto port = listener.local_port(&error);

        std::promise<void> stop_server;
        auto server_done = stop_server.get_future();
        std::thread server_th([&] {
            std::string server_err;
            auto socket = listener.accept(&server_err);
            assert(socket.is_open());
            assert(socket.set_io_timeouts(1000, 1000, &server_err));
            nstu::security::ReplayProtector replay;
            const nstu::control::KeyResolver resolver =
                [&](const nstu::security::ClientId& requested_id,
                    std::uint32_t requested_key_id)
                -> std::optional<std::vector<std::byte>> {
                if (requested_id != id || requested_key_id != key_id) {
                    return std::nullopt;
                }
                return key;
            };
            auto session = nstu::control::server_handshake(
                socket, resolver, replay, now, &server_err);
            assert(session.has_value());
            nstu::control::AuthenticatedControlChannel server_channel(
                std::move(socket), std::move(*session));
            const auto cmd1 = server_channel.receive(&server_err);
            assert(cmd1.has_value());
            assert(cmd1->envelope.type == nstu::protocol::CommandType::lock);
            server_done.wait();
        });

        nstu::net::TcpSocket client_socket;
        assert(client_socket.connect("127.0.0.1", port, &error));
        assert(client_socket.set_io_timeouts(5000, 5000, &error));
        auto client_session = nstu::control::client_handshake(
            client_socket, id, key_id, key, now, std::chrono::seconds(120), &error);
        assert(client_session.has_value());
        nstu::control::AuthenticatedControlChannel client_channel(
            std::move(client_socket), std::move(*client_session));

        std::vector<std::byte> payload(64, std::byte{0x11});
        // Successful send with deadline in future
        assert(client_channel.send(nstu::protocol::CommandType::lock, 200, payload,
                                   std::chrono::steady_clock::now() + 5s, &error));

        // Past deadline fails immediately and disconnects channel
        const auto past_deadline = std::chrono::steady_clock::now() - 10ms;
        assert(!client_channel.send(nstu::protocol::CommandType::lock, 201, payload,
                                    past_deadline, &error));
        assert(!client_channel.is_open());

        // Subsequent send fails because channel is closed
        assert(!client_channel.send(nstu::protocol::CommandType::lock, 202, payload, &error));

        stop_server.set_value();
        server_th.join();
        nstu::security::secure_zero(key);
    }
}

// Microsoft Winsock documents partial/nonblocking send and buffer readback:
// https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-send
// https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-getsockopt
// This parked-peer profile proves setup pressure only, not target-frame progress.
void test_native_deadline_setup_profile() {
    nstu::security::Sha256Digest key{};
    key.fill(std::byte{0x31});
    constexpr std::size_t user_bytes = nstu::protocol::kMaxCommandPayload -
        sizeof(std::uint64_t) - nstu::security::kControlAuthTagBytes;
    constexpr std::uint64_t sequence = 0x0102030405060708ULL;
    constexpr std::uint64_t request_id = 0x123456789abcdef0ULL;
    const std::vector<std::byte> payload(user_bytes, std::byte{0x4d});
    const nstu::protocol::CommandEnvelope envelope{
        .version = nstu::protocol::kCommandVersion,
        .type = nstu::protocol::CommandType::lock,
        .payload_bytes = static_cast<std::uint32_t>(user_bytes),
        .request_id = request_id,
    };
    const auto wire = nstu::control::encode_authenticated_command(key, envelope, sequence, payload);
    assert(user_bytes == 65512U);
    assert(wire.size() == 4U + nstu::protocol::kCommandHeaderBytes + nstu::protocol::kMaxCommandPayload);
    assert(wire.size() == 65560U);
    std::uint32_t prefix{};
    for (unsigned index = 0; index != 4U; ++index) {
        prefix |= static_cast<std::uint32_t>(std::to_integer<unsigned>(wire[index])) << (8U * index);
    }
    assert(prefix == wire.size() - 4U);
    nstu::protocol::TcpFrameParser parser;
    std::vector<nstu::protocol::TcpFrame> frames;
    assert(parser.feed(wire, frames));
    assert(frames.size() == 1U);
    assert(frames.front().envelope.payload_bytes == nstu::protocol::kMaxCommandPayload);
    std::string decode_error;
    const auto decoded = nstu::control::decode_authenticated_command(key, frames.front(), &decode_error);
    assert(decoded.has_value());
    assert(decoded->envelope.version == envelope.version);
    assert(decoded->envelope.type == envelope.type);
    assert(decoded->envelope.payload_bytes == envelope.payload_bytes);
    assert(decoded->envelope.request_id == request_id);
    assert(decoded->sequence == sequence);
    assert(decoded->payload == payload);

    nstu::test::NativeDeadlineSetupFixture fixture;
    const auto& evidence = fixture.evidence();
    const bool fixture_ok = fixture.prepare();
    fixture.finish();
    nstu::security::secure_zero(key);
    fixture.record_completion();
    std::cout << "NATIVE-DEADLINE-SETUP wire=" << wire.size() << " requested-buf=4096"
              << " listener-rcvbuf=" << evidence.listener_rcvbuf << " sender-sndbuf=" << evidence.sender_sndbuf
              << " accepted-rcvbuf=" << evidence.accepted_rcvbuf << " accepted-prefix=" << evidence.accepted_bytes
              << " attempts=" << evidence.attempts << " setup-start-ms=" << evidence.setup_start_ms
              << " stall-ms=" << evidence.stall_ms << " elapsed-ms=" << evidence.elapsed_ms
              << " pressure-error=" << evidence.pressure_error << " restore-error=" << evidence.restore_error
              << " restored=" << evidence.restore_fionbio_zero_ok << " owners-closed=" << evidence.owners_closed
              << " failed-stage=" << evidence.failed_stage << " first-native-error=" << evidence.first_native_error
              << " wrapper-error=" << evidence.wrapper_error << std::endl;
    assert(fixture_ok && evidence.failed_stage.empty());
    assert(evidence.actual_would_block && evidence.accepted_bytes > 0U);
    assert(evidence.pressure_error == WSAEWOULDBLOCK);
    assert(evidence.restore_fionbio_zero_ok && evidence.restore_error == 0);
    assert(evidence.owners_closed && evidence.elapsed_ms < 15000);
}

} // namespace

int main() {
    nstu::net::WinsockRuntime winsock;
    assert(winsock.ready());
    test_native_deadline_setup_profile();
    test_deadline_mode_restore_failure();
    test_scripted_deadline_success_and_failures();
    test_deadline_wait_duration_bounds();
    test_deadline_then_legacy_channel_commands();
    const auto id = client_id();
    auto key = pre_shared_key();
    constexpr std::uint32_t key_id = 42;
    constexpr std::uint64_t now = 20'000;

    nstu::net::TcpSocket listener;
    std::string error;
    assert(listener.listen(0, 8, &error));
    const auto port = listener.local_port(&error);
    assert(port != 0);
    std::atomic_bool server_succeeded = false;
    std::thread server([&] {
        std::string server_error;
        auto socket = listener.accept(&server_error);
        assert(socket.is_open());
        assert(socket.set_io_timeouts(5000, 5000, &server_error));
        nstu::security::ReplayProtector replay;
        const nstu::control::KeyResolver resolver =
            [&](const nstu::security::ClientId& requested_id,
                std::uint32_t requested_key_id)
            -> std::optional<std::vector<std::byte>> {
            if (requested_id != id || requested_key_id != key_id) {
                return std::nullopt;
            }
            return key;
        };
        auto session = nstu::control::server_handshake(
            socket, resolver, replay, now, &server_error);
        assert(session.has_value());
        nstu::control::AuthenticatedControlChannel channel(
            std::move(socket), std::move(*session));
        const auto command = channel.receive(&server_error);
        assert(command.has_value());
        assert(command->envelope.type == nstu::protocol::CommandType::lock);
        assert(command->envelope.request_id == 99);
        assert(command->payload ==
               std::vector<std::byte>({std::byte{0x10}, std::byte{0x20}}));
        const std::array<std::byte, 1> response{std::byte{0x01}};
        assert(channel.send(nstu::protocol::CommandType::hello_ack, 99,
                            response, &server_error));
        server_succeeded = true;
    });

    nstu::net::TcpSocket client_socket;
    assert(client_socket.connect("127.0.0.1", port, &error));
    assert(client_socket.set_io_timeouts(5000, 5000, &error));
    auto client_session = nstu::control::client_handshake(
        client_socket, id, key_id, key, now, std::chrono::seconds(120), &error);
    assert(client_session.has_value());
    nstu::control::AuthenticatedControlChannel client_channel(
        std::move(client_socket), std::move(*client_session));
    const std::array<std::byte, 2> command_payload{
        std::byte{0x10}, std::byte{0x20}};
    assert(client_channel.send(nstu::protocol::CommandType::lock, 99,
                               command_payload, &error));
    const auto response = client_channel.receive(&error);
    assert(response.has_value());
    assert(response->envelope.type == nstu::protocol::CommandType::hello_ack);
    assert(response->sequence == 0);
    server.join();
    assert(server_succeeded);

    nstu::protocol::CommandEnvelope envelope{
        .version = nstu::protocol::kCommandVersion,
        .type = nstu::protocol::CommandType::chat,
        .payload_bytes = 2,
        .request_id = 123,
    };
    auto wire = nstu::control::encode_authenticated_command(
        key, envelope, 7, command_payload);
    assert(!wire.empty());
    nstu::protocol::TcpFrameParser parser;
    std::vector<nstu::protocol::TcpFrame> frames;
    assert(parser.feed(wire, frames));
    assert(frames.size() == 1);
    const auto decoded =
        nstu::control::decode_authenticated_command(key, frames[0], &error);
    assert(decoded.has_value());
    assert(decoded->sequence == 7);
    frames[0].payload.back() ^= std::byte{1};
    assert(!nstu::control::decode_authenticated_command(key, frames[0], &error)
                .has_value());
    envelope.type = nstu::protocol::CommandType::auth_hello;
    assert(nstu::control::encode_authenticated_command(
               key, envelope, 8, command_payload)
               .empty());

    const std::array<std::byte, 8> entropy{
        std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
        std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};
    const auto protected_blob =
        nstu::security::protect_machine_secret(key, entropy, &error);
    assert(!protected_blob.empty());
    auto unprotected =
        nstu::security::unprotect_machine_secret(protected_blob, entropy, &error);
    assert(unprotected == key);
    nstu::security::secure_zero(unprotected);

    wchar_t temporary_directory[MAX_PATH]{};
    assert(GetTempPathW(MAX_PATH, temporary_directory) != 0);
    const auto secret_path = std::filesystem::path(temporary_directory) /
        (L"nstu-secret-test-" + std::to_wstring(GetCurrentProcessId()) +
         L".bin");
    assert(nstu::security::save_machine_secret(secret_path.wstring(), key,
                                                entropy, &error));
    auto loaded = nstu::security::load_machine_secret(
        secret_path.wstring(), entropy, &error);
    assert(loaded == key);
    nstu::security::secure_zero(loaded);
    assert(DeleteFileW(secret_path.c_str()));
    nstu::security::secure_zero(key);

    nstu::net::IocpPort iocp;
    assert(iocp.create(1, &error));
    const nstu::net::IocpPort::Completion posted{
        .bytes_transferred = 17,
        .completion_key = 0x1234,
        .overlapped = 0,
        .error_code = 0,
    };
    assert(iocp.post(posted, &error));
    nstu::net::IocpPort::Completion received_completion{};
    assert(iocp.wait(received_completion, 1000, &error));
    assert(received_completion.bytes_transferred == posted.bytes_transferred);
    assert(received_completion.completion_key == posted.completion_key);
    assert(received_completion.overlapped == 0);
    assert(received_completion.error_code == 0);

    nstu::control::ClientStatusReport status;
    status.hostname = "SNAPSHOT-PC";
    status.snapshotting = true;
    status.snapshot_interval_seconds = 7;
    status.viewing_broadcast = true;
    const auto status_wire = nstu::control::encode_status_report(status);
    const auto decoded_status =
        nstu::control::decode_status_report(status_wire);
    assert(decoded_status.has_value());
    assert(decoded_status->snapshotting);
    assert(decoded_status->snapshot_interval_seconds == 7);
    assert(decoded_status->viewing_broadcast);

    const auto schedule = nstu::control::encode_snapshot_schedule(8);
    assert(nstu::control::decode_snapshot_schedule(schedule) == 8);
    assert(nstu::control::encode_snapshot_schedule(4).empty());

    nstu::control::SnapshotFrame snapshot;
    snapshot.width = 480;
    snapshot.height = 270;
    snapshot.captured_at_unix_milliseconds = 123'456;
    snapshot.jpeg = {std::byte{0xff}, std::byte{0xd8}, std::byte{0xff},
                     std::byte{0xd9}};
    const auto snapshot_wire =
        nstu::control::encode_snapshot_frame(snapshot);
    const auto decoded_snapshot =
        nstu::control::decode_snapshot_frame(snapshot_wire);
    assert(decoded_snapshot.has_value());
    assert(decoded_snapshot->width == snapshot.width);
    assert(decoded_snapshot->jpeg == snapshot.jpeg);
    auto corrupt_snapshot = snapshot_wire;
    corrupt_snapshot[6] = std::byte{1};
    assert(!nstu::control::decode_snapshot_frame(corrupt_snapshot)
                .has_value());
    auto oversized_snapshot = snapshot;
    oversized_snapshot.width =
        static_cast<std::uint16_t>(nstu::control::kMaximumSnapshotWidth + 1);
    assert(nstu::control::encode_snapshot_frame(oversized_snapshot).empty());
    oversized_snapshot = snapshot;
    oversized_snapshot.height =
        static_cast<std::uint16_t>(nstu::control::kMaximumSnapshotHeight + 1);
    assert(nstu::control::encode_snapshot_frame(oversized_snapshot).empty());
    auto oversized_wire = snapshot_wire;
    const auto oversized_width = static_cast<std::uint16_t>(
        nstu::control::kMaximumSnapshotWidth + 1);
    oversized_wire[2] = static_cast<std::byte>(oversized_width & 0xffu);
    oversized_wire[3] = static_cast<std::byte>(oversized_width >> 8u);
    assert(!nstu::control::decode_snapshot_frame(oversized_wire).has_value());

    const nstu::control::OverlayStroke stroke{
        .x0 = 100,
        .y0 = 200,
        .x1 = 300,
        .y1 = 400,
        .thickness = 5,
        .rgba = 0xff2020ffu,
    };
    const auto stroke_wire = nstu::control::encode_overlay_stroke(stroke);
    const auto decoded_stroke =
        nstu::control::decode_overlay_stroke(stroke_wire);
    assert(decoded_stroke.has_value());
    assert(decoded_stroke->x1 == stroke.x1);
    assert(decoded_stroke->rgba == stroke.rgba);

    test_send_deadlines();
    return 0;
}
