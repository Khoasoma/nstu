#pragma once

#include "nstu/network.hpp"

#include <winsock2.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace nstu::test {

struct NativeDeadlineSetupEvidence {
    std::string failed_stage;
    std::string wrapper_error;
    int first_native_error{};
    int pressure_error{};
    int restore_error{};
    int listener_rcvbuf{};
    int sender_sndbuf{};
    int accepted_rcvbuf{};
    std::size_t accepted_bytes{};
    std::size_t attempts{};
    long long setup_start_ms{-1};
    long long stall_ms{-1};
    long long elapsed_ms{};
    bool sender_nonblocking{};
    bool actual_would_block{};
    bool restore_fionbio_zero_ok{};
    bool owners_closed{};
};

// Test-only, single-use fixture. All methods belong to its one caller thread.
// Preparation preserves the measured profile; it proves no target-frame progress.
class NativeDeadlineSetupFixture {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::byte setup_byte{0x7b};

    NativeDeadlineSetupFixture()
        : fixture_start_(Clock::now()),
          startup_deadline_(fixture_start_ + std::chrono::seconds(5)),
          overall_deadline_(fixture_start_ + std::chrono::seconds(15)) {}

    NativeDeadlineSetupFixture(const NativeDeadlineSetupFixture&) = delete;
    NativeDeadlineSetupFixture& operator=(const NativeDeadlineSetupFixture&) = delete;
    NativeDeadlineSetupFixture(NativeDeadlineSetupFixture&&) = delete;
    NativeDeadlineSetupFixture& operator=(NativeDeadlineSetupFixture&&) = delete;
    ~NativeDeadlineSetupFixture() noexcept { finish(); }

    bool prepare() {
        const auto fixture_start = fixture_start_;
        const auto startup_deadline = startup_deadline_;
        const auto overall_deadline = overall_deadline_;
        auto& evidence = evidence_;
        auto& listener = listener_;
        auto& sender = sender_;
        auto& accepted = accepted_;
        auto remaining_startup_ms = [&]() -> std::uint32_t {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::min(startup_deadline, overall_deadline) - Clock::now()).count();
            return remaining > 0 ? static_cast<std::uint32_t>(remaining) : 0U;
        };
        auto native_handle = [](const nstu::net::TcpSocket& socket) {
            return static_cast<SOCKET>(socket.native_handle());
        };
        auto profile = [&](nstu::net::TcpSocket& socket, int option, int& readback, const char* stage) {
            constexpr int requested = 4096;
            if (::setsockopt(native_handle(socket), SOL_SOCKET, option,
                reinterpret_cast<const char*>(&requested), static_cast<int>(sizeof(requested))) == SOCKET_ERROR) {
                const int error = ::WSAGetLastError();
                return fail(std::string(stage) + " setsockopt", error);
            }
            int bytes = static_cast<int>(sizeof(readback));
            if (::getsockopt(native_handle(socket), SOL_SOCKET, option,
                reinterpret_cast<char*>(&readback), &bytes) == SOCKET_ERROR) {
                const int error = ::WSAGetLastError();
                return fail(std::string(stage) + " getsockopt", error);
            }
            return bytes == static_cast<int>(sizeof(readback)) && readback > 0
                ? true : fail(std::string(stage) + " invalid readback");
        };
        auto mode = [&](nstu::net::TcpSocket& socket, u_long value, const char* stage, bool restoring = false) {
            if (::ioctlsocket(native_handle(socket), FIONBIO, &value) == SOCKET_ERROR) {
                const int error = ::WSAGetLastError();
                if (restoring) evidence.restore_error = error;
                return fail(stage, error);
            }
            if (restoring) evidence.restore_fionbio_zero_ok = true;
            return true;
        };
        bool fixture_ok = false;
        do {
            std::string error;
            if (!listener.listen(0, 4, &error)) { fail("listener listen wrapper", 0, error); break; }
            if (!profile(listener, SO_RCVBUF, evidence.listener_rcvbuf, "listener") ||
                !mode(listener, 1UL, "listener FIONBIO 1")) break;
            error.clear();
            const auto port = listener.local_port(&error);
            if (port == 0) { fail("listener local_port wrapper", 0, error); break; }
            auto timeout = remaining_startup_ms();
            if (timeout == 0U) { fail("startup budget before connect"); break; }
            error.clear();
            if (!sender.connect_with_timeout("127.0.0.1", port, timeout, &error)) {
                fail("sender connect_with_timeout wrapper", 0, error); break;
            }
            if (!profile(sender, SO_SNDBUF, evidence.sender_sndbuf, "sender")) break;
            timeout = remaining_startup_ms();
            if (timeout == 0U) { fail("startup budget before readiness"); break; }
            error.clear();
            if (!listener.wait_readable(timeout, &error)) { fail("listener wait_readable wrapper", 0, error); break; }
            const SOCKET accepted_native = ::accept(native_handle(listener), nullptr, nullptr);
            if (accepted_native == INVALID_SOCKET) {
                const int native_error = ::WSAGetLastError();
                fail("native accept", native_error); break;
            }
            accepted = nstu::net::TcpSocket(static_cast<std::uintptr_t>(accepted_native));
            if (!profile(accepted, SO_RCVBUF, evidence.accepted_rcvbuf, "accepted")) break;
            sockaddr_in peer{};
            int peer_bytes = static_cast<int>(sizeof(peer));
            if (::getpeername(native_handle(accepted), reinterpret_cast<sockaddr*>(&peer), &peer_bytes) == SOCKET_ERROR) {
                const int native_error = ::WSAGetLastError();
                fail("accepted getpeername", native_error); break;
            }
            error.clear();
            const auto sender_port = sender.local_port(&error);
            if (sender_port == 0) { fail("sender local_port wrapper", 0, error); break; }
            if (peer_bytes != static_cast<int>(sizeof(peer)) || peer.sin_family != AF_INET ||
                ::ntohl(peer.sin_addr.s_addr) != 0x7f000001UL || ::ntohs(peer.sin_port) != sender_port) {
                fail("accepted peer identity"); break;
            }
            if (Clock::now() >= startup_deadline || Clock::now() >= overall_deadline) {
                fail("startup absolute budget"); break;
            }
            if (!mode(sender, 1UL, "sender FIONBIO 1")) break;
            evidence.sender_nonblocking = true;
            const auto setup_start = Clock::now();
            if (setup_start >= std::min(startup_deadline, overall_deadline)) {
                fail("startup budget at setup transition"); break;
            }
            evidence.setup_start_ms = static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                setup_start - fixture_start).count());
            const auto setup_deadline = setup_start + std::chrono::seconds(5);
            std::array<std::byte, 16384> setup_buffer{};
            setup_buffer.fill(std::byte{0x7b});
            constexpr std::size_t byte_cap = 4U * 1024U * 1024U;
            while (true) {
                if (Clock::now() >= std::min(setup_deadline, overall_deadline)) { fail("pressure absolute budget"); break; }
                if (evidence.accepted_bytes >= byte_cap || evidence.attempts >= 4096U) {
                    fail("pressure cap without WSAEWOULDBLOCK"); break;
                }
                const int requested = static_cast<int>(std::min(setup_buffer.size(), byte_cap - evidence.accepted_bytes));
                ++evidence.attempts;
                const int sent = ::send(native_handle(sender), reinterpret_cast<const char*>(setup_buffer.data()), requested, 0);
                if (sent == SOCKET_ERROR) {
                    const int native_error = ::WSAGetLastError();
                    const auto pressure_observed = Clock::now();
                    evidence.pressure_error = native_error;
                    if (native_error == WSAEWOULDBLOCK) {
                        evidence.actual_would_block = true;
                        evidence.stall_ms = static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                            pressure_observed - fixture_start).count());
                        if (pressure_observed >= std::min(setup_deadline, overall_deadline)) {
                            fail("pressure observation outside absolute budget");
                        }
                        if (evidence.accepted_bytes == 0U) fail("pressure stalled before positive prefix");
                    } else { fail("pressure send", native_error); }
                    break;
                }
                if (sent <= 0 || sent > requested) { fail("pressure invalid send count"); break; }
                evidence.accepted_bytes += static_cast<std::size_t>(sent);
            }
            fixture_ok = evidence.failed_stage.empty() && evidence.actual_would_block && evidence.accepted_bytes > 0U;
        } while (false);
        if (evidence.sender_nonblocking && sender.is_open()) mode(sender, 0UL, "sender FIONBIO 0 restore", true);
        // Keep the original pre-restore result; the caller checks failure evidence.
        prepared_ = fixture_ok && evidence.failed_stage.empty();
        return fixture_ok;
    }

    // Cleanup only. The caller clears its key before recording completion time.
    void finish() noexcept {
        if (finished_) return;
        auto& accepted = accepted_;
        auto& sender = sender_;
        auto& listener = listener_;
        auto& evidence = evidence_;
        accepted.close();
        sender.close();
        listener.close();
        evidence.owners_closed = !accepted.is_open() && !sender.is_open() && !listener.is_open();
        finished_ = true;
    }

    void record_completion() {
        auto& evidence = evidence_;
        const auto overall_deadline = overall_deadline_;
        evidence.elapsed_ms = elapsed();
        if (Clock::now() >= overall_deadline) fail("overall fixture absolute budget");
    }

    const NativeDeadlineSetupEvidence& evidence() const noexcept { return evidence_; }
    // The caller retains exclusive ownership of sender operations/native handle.
    nstu::net::TcpSocket& sender() noexcept { return sender_; }
    // The returned peer has one new owner. finish() cannot cross-close it;
    // owners_closed describes only sockets still owned by this fixture.
    nstu::net::TcpSocket take_peer() noexcept {
        assert(prepared_ && !finished_ && accepted_.is_open());
        return std::move(accepted_);
    }
    Clock::time_point overall_deadline() const noexcept { return overall_deadline_; }

private:
    long long elapsed() const {
        const auto fixture_start = fixture_start_;
            return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - fixture_start).count());
    }
    bool fail(std::string stage, int native_error = 0, std::string wrapper_error = {}) {
        auto& evidence = evidence_;
            if (evidence.failed_stage.empty()) {
                evidence.failed_stage = std::move(stage);
                evidence.first_native_error = native_error;
                evidence.wrapper_error = std::move(wrapper_error);
            }
            return false;
    }

    Clock::time_point fixture_start_;
    Clock::time_point startup_deadline_;
    Clock::time_point overall_deadline_;
    NativeDeadlineSetupEvidence evidence_;
    nstu::net::TcpSocket listener_;
    nstu::net::TcpSocket sender_;
    nstu::net::TcpSocket accepted_;
    bool prepared_ = false;
    bool finished_ = false;
};

} // namespace nstu::test
