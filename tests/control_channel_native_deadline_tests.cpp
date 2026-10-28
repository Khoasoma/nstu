#include "nstu/control_channel.hpp"
#include "nstu/multicast.hpp"
#include "native_deadline_fixture.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using Fixture = nstu::test::NativeDeadlineSetupFixture;
using Socket = nstu::net::TcpSocket;
using nstu::net::detail::DeadlineSendAttempt;
using nstu::net::detail::DeadlineSendOperations;
using nstu::net::detail::DeadlineWaitResult;
constexpr auto kTargetDuration = std::chrono::milliseconds{500};
constexpr auto kReceiverSlice = std::chrono::milliseconds{20};
constexpr auto kCleanupBudget = std::chrono::seconds{3};
constexpr std::size_t kSetupGrant = 16384;
constexpr std::size_t kTargetGrant = 1024;
constexpr std::size_t kTargetQuota = 16384;
constexpr std::size_t kEventCapacity = 2048;
constexpr std::size_t kUserBytes = nstu::protocol::kMaxCommandPayload -
    sizeof(std::uint64_t) - nstu::security::kControlAuthTagBytes;
constexpr std::uint64_t kRequestId = 0x1122334455667788ULL;
constexpr std::size_t kWireBytes = 4 + nstu::protocol::kCommandHeaderBytes +
    nstu::protocol::kMaxCommandPayload;
static_assert(kUserBytes == 65512 && kWireBytes == 65560);

SOCKET native_socket(const Socket& socket) noexcept {
    return static_cast<SOCKET>(socket.native_handle());
}

enum class EventKind { mode, send, writable_select, close, readable_select, recv };
struct Event {
    EventKind kind{};
    Clock::time_point began{}, ended{}, wait_deadline{};
    std::size_t requested{}, offset{};
    int returned{}, native_error{};
    bool would_block{}, native_select{}, nonblocking{}, logical_invalid{}, target{};
    DeadlineWaitResult wait_result{DeadlineWaitResult::failed};
};
struct Trace {
    std::span<Event> events;
    std::size_t count{};
    bool overflow{};
    void append(const Event& event) noexcept {
        if (count < events.size()) events[count++] = event;
        else overflow = true;
    }
};
struct Control {
    std::mutex mutex;
    std::condition_variable changed;
    bool parked{}, start{}, result{}, stop{}, receiver_failed{};
    std::uint64_t stall_generation{};
    Clock::time_point target_start{}, target_deadline{}, result_at{}, stop_requested{};
};
struct NativeAdapter {
    Socket* sender{};
    Control* control{};
    Trace trace;
    std::size_t accepted{};
    unsigned close_count{};
    bool closed_invalid{};
};

Clock::time_point adapter_now(void*) noexcept { return Clock::now(); }
bool adapter_mode(void* context, bool nonblocking) noexcept {
    auto& adapter = *static_cast<NativeAdapter*>(context);
    Event event{};
    event.kind = EventKind::mode;
    event.nonblocking = nonblocking;
    event.began = Clock::now();
    u_long value = nonblocking ? 1UL : 0UL;
    const int result = ::ioctlsocket(native_socket(*adapter.sender), FIONBIO, &value);
    const int error = result == SOCKET_ERROR ? ::WSAGetLastError() : 0;
    event.ended = Clock::now();
    event.returned = result;
    event.native_error = error;
    adapter.trace.append(event);
    return result == 0;
}
DeadlineSendAttempt adapter_send(void* context, std::span<const std::byte> bytes) noexcept {
    auto& adapter = *static_cast<NativeAdapter*>(context);
    Event event{};
    event.kind = EventKind::send;
    event.requested = bytes.size();
    event.offset = adapter.accepted;
    event.began = Clock::now();
    // Exactly the engine's span, with no additional native send cap.
    const int result = ::send(native_socket(*adapter.sender),
        reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), 0);
    const int error = result == SOCKET_ERROR ? ::WSAGetLastError() : 0;
    event.ended = Clock::now();
    event.returned = result;
    event.native_error = error;
    event.would_block = result == SOCKET_ERROR && error == WSAEWOULDBLOCK;
    if (result > 0) adapter.accepted += static_cast<std::size_t>(result);
    adapter.trace.append(event);
    if (event.would_block) {
        {
            std::lock_guard lock(adapter.control->mutex);
            ++adapter.control->stall_generation;
        }
        adapter.control->changed.notify_all();
    }
    return {result, event.would_block};
}
DeadlineWaitResult adapter_wait(void* context, Clock::time_point deadline) noexcept {
    auto& adapter = *static_cast<NativeAdapter*>(context);
    Event event{};
    event.kind = EventKind::writable_select;
    event.wait_deadline = deadline;
    event.began = Clock::now();
    if (event.began >= deadline) {
        event.ended = Clock::now();
        event.wait_result = DeadlineWaitResult::timed_out;
        adapter.trace.append(event);
        return event.wait_result;
    }
    const auto wait = nstu::net::detail::deadline_wait_duration(deadline - event.began);
    timeval timeout{wait.seconds, wait.microseconds};
    fd_set writable{};
    FD_ZERO(&writable);
    const SOCKET handle = native_socket(*adapter.sender);
    FD_SET(handle, &writable);
    event.native_select = true;
    const int result = ::select(0, nullptr, &writable, nullptr, &timeout);
    const int error = result == SOCKET_ERROR ? ::WSAGetLastError() : 0;
    event.ended = Clock::now();
    event.returned = result;
    event.native_error = error;
    event.wait_result = result == SOCKET_ERROR ? DeadlineWaitResult::failed :
        (result == 0 ? DeadlineWaitResult::timed_out : DeadlineWaitResult::ready);
    adapter.trace.append(event);
    return event.wait_result;
}
void adapter_close(void* context) noexcept {
    auto& adapter = *static_cast<NativeAdapter*>(context);
    Event event{};
    event.kind = EventKind::close;
    event.began = Clock::now();
    ++adapter.close_count;
    adapter.sender->close();
    event.ended = Clock::now();
    event.logical_invalid = !adapter.sender->is_open();
    adapter.closed_invalid = event.logical_invalid;
    adapter.trace.append(event); // No claim about the unobserved closesocket return.
}

struct Receiver {
    Control* control{};
    std::span<const std::byte> wire;
    Clock::time_point overall_deadline{};
    std::size_t setup_expected{}, setup_received{}, target_received{}, qualifying_received{}, late_received{};
    std::array<std::byte, kSetupGrant> buffer{};
    std::array<std::byte, kTargetQuota> prefix{};
    Trace trace;
    const char* failure_phase{};
    int failure_native_error{};
    Clock::time_point first_qualifying{}, last_qualifying{}, completed{};
    bool closed_invalid{};
    bool stopping() {
        std::lock_guard lock(control->mutex);
        return control->stop || control->result || control->receiver_failed;
    }
    void fail(const char* phase, int error) {
        if (failure_phase == nullptr) {
            failure_phase = phase;
            failure_native_error = error;
        }
        {
            std::lock_guard lock(control->mutex);
            control->receiver_failed = true;
        }
        control->changed.notify_all();
    }
};

bool receive_grant(Socket& peer, Receiver& receiver, std::size_t grant, bool target,
                   Clock::time_point target_start, Clock::time_point deadline) {
    std::size_t consumed = 0;
    while (consumed < grant) {
        if (receiver.stopping()) return false;
        const auto now = Clock::now();
        if (now >= receiver.overall_deadline) {
            receiver.fail("receiver-overall-deadline", 0); return false;
        }
        if (now >= deadline) return true; // Park; do not close before the sender result.
        const auto slice_end = std::min(std::min(receiver.overall_deadline, deadline), now + kReceiverSlice);
        const auto wait = nstu::net::detail::deadline_wait_duration(slice_end - now);
        timeval timeout{wait.seconds, wait.microseconds};
        const SOCKET handle = native_socket(peer);
        fd_set readable{};
        FD_ZERO(&readable);
        FD_SET(handle, &readable);
        Event selected{};
        selected.kind = EventKind::readable_select;
        selected.target = target;
        selected.began = Clock::now();
        selected.native_select = true;
        selected.wait_deadline = slice_end;
        const int ready = ::select(0, &readable, nullptr, nullptr, &timeout);
        const int select_error = ready == SOCKET_ERROR ? ::WSAGetLastError() : 0;
        selected.ended = Clock::now();
        selected.returned = ready;
        selected.native_error = select_error;
        receiver.trace.append(selected);
        if (ready == SOCKET_ERROR) { receiver.fail("receiver-select", select_error); return false; }
        if (ready == 0) continue;
        if (FD_ISSET(handle, &readable) == 0) { receiver.fail("receiver-select-not-readable", 0); return false; }
        if (receiver.stopping()) return false;
        if (Clock::now() >= deadline) return true;
        // The grant's fixed phase prevents any setup read from spilling into target.
        const std::size_t requested = std::min(grant - consumed, receiver.buffer.size());
        Event received{};
        received.kind = EventKind::recv;
        received.target = target;
        received.requested = requested;
        received.offset = target ? receiver.target_received : receiver.setup_received;
        received.began = Clock::now();
        const int result = ::recv(handle, reinterpret_cast<char*>(receiver.buffer.data()),
            static_cast<int>(requested), 0);
        const int error = result == SOCKET_ERROR ? ::WSAGetLastError() : 0;
        received.ended = Clock::now();
        received.returned = result;
        received.native_error = error;
        received.would_block = result == SOCKET_ERROR && error == WSAEWOULDBLOCK;
        receiver.trace.append(received);
        if (received.would_block) continue;
        if (result == SOCKET_ERROR) { receiver.fail("receiver-recv", error); return false; }
        if (result == 0) { receiver.fail("receiver-eof-before-result", 0); return false; }
        const auto count = static_cast<std::size_t>(result);
        if (count > requested) { receiver.fail("receiver-invalid-count", 0); return false; }
        if (!target) {
            for (std::size_t index = 0; index < count; ++index) {
                if (receiver.buffer[index] != Fixture::setup_byte) {
                    receiver.fail("receiver-setup-prefix-mismatch", 0); return false;
                }
            }
            receiver.setup_received += count;
        } else {
            const auto offset = receiver.target_received;
            if (offset > kTargetQuota || count > kTargetQuota - offset ||
                offset > receiver.wire.size() || count > receiver.wire.size() - offset) {
                receiver.fail("receiver-target-bounds", 0); return false;
            }
            for (std::size_t index = 0; index < count; ++index) {
                if (receiver.buffer[index] != receiver.wire[offset + index]) {
                    receiver.fail("receiver-target-prefix-mismatch", 0); return false;
                }
                receiver.prefix[offset + index] = receiver.buffer[index];
            }
            if (received.ended >= target_start && received.ended < deadline &&
                receiver.qualifying_received == offset) {
                if (receiver.qualifying_received == 0) receiver.first_qualifying = received.ended;
                receiver.qualifying_received += count;
                receiver.last_qualifying = received.ended;
            } else receiver.late_received += count;
            receiver.target_received += count;
        }
        consumed += count;
    }
    return true;
}

void run_receiver(Socket peer, Receiver& receiver) noexcept {
    try {
        Event mode{};
        mode.kind = EventKind::mode;
        mode.nonblocking = true;
        mode.began = Clock::now();
        u_long value = 1UL;
        const int result = ::ioctlsocket(native_socket(peer), FIONBIO, &value);
        const int error = result == SOCKET_ERROR ? ::WSAGetLastError() : 0;
        mode.ended = Clock::now();
        mode.returned = result;
        mode.native_error = error;
        receiver.trace.append(mode);
        if (result != 0) receiver.fail("receiver-fionbio-one", error);
        else {
            { std::lock_guard lock(receiver.control->mutex); receiver.control->parked = true; }
            receiver.control->changed.notify_all();
        }
        std::uint64_t consumed_generation = 0;
        while (!receiver.stopping()) {
            std::size_t grant = 0;
            bool target = false;
            Clock::time_point target_start{}, deadline{};
            {
                std::unique_lock lock(receiver.control->mutex);
                const auto now = Clock::now();
                if (now >= receiver.overall_deadline) {
                    lock.unlock(); receiver.fail("receiver-overall-deadline", 0); break;
                }
                const bool may_drain = receiver.control->start &&
                    now < receiver.control->target_deadline && receiver.target_received < kTargetQuota;
                if (!may_drain || receiver.control->stall_generation == consumed_generation) {
                    receiver.control->changed.wait_until(lock, std::min(receiver.overall_deadline, now + kReceiverSlice));
                    continue;
                }
                // One bounded grant consumes the latest generation, including coalesced notifications.
                consumed_generation = receiver.control->stall_generation;
                target_start = receiver.control->target_start;
                deadline = receiver.control->target_deadline;
                if (receiver.setup_received < receiver.setup_expected)
                    grant = std::min(kSetupGrant, receiver.setup_expected - receiver.setup_received);
                else { target = true; grant = std::min(kTargetGrant, kTargetQuota - receiver.target_received); }
            }
            if (!receive_grant(peer, receiver, grant, target, target_start, deadline)) break;
        }
    } catch (...) { receiver.fail("receiver-exception", 0); }
    peer.close();
    receiver.closed_invalid = !peer.is_open();
    receiver.completed = Clock::now();
    receiver.control->changed.notify_all();
}

class StopAndJoin {
public:
    StopAndJoin(Control& control, std::jthread& worker) noexcept : control_(control), worker_(worker) {}
    ~StopAndJoin() { stop_and_join(); }
    void stop_and_join() noexcept {
        {
            std::lock_guard lock(control_.mutex);
            if (!control_.stop) { control_.stop_requested = Clock::now(); control_.stop = true; }
        }
        control_.changed.notify_all();
        worker_.request_stop();
        if (worker_.joinable()) worker_.join();
    }
private:
    Control& control_;
    std::jthread& worker_;
};

long long relative_us(Clock::time_point point, Clock::time_point start) noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(point - start).count();
}
void print_fixture(const nstu::test::NativeDeadlineSetupEvidence& e) {
    std::cerr << "setup accepted=" << e.accepted_bytes << " attempts=" << e.attempts
        << " listener_buf=" << e.listener_rcvbuf << " sender_buf=" << e.sender_sndbuf
        << " receiver_buf=" << e.accepted_rcvbuf << " pressure_error=" << e.pressure_error
        << " restored=" << e.restore_fionbio_zero_ok << " restore_error=" << e.restore_error
        << " owners_closed=" << e.owners_closed << " elapsed_ms=" << e.elapsed_ms
        << " failed_stage=" << e.failed_stage << " first_native_error=" << e.first_native_error
        << " wrapper_error=" << e.wrapper_error << '\n';
}
void print_trace(const char* owner, const Trace& trace, Clock::time_point start) {
    for (std::size_t index = 0; index < trace.count; ++index) {
        const auto& e = trace.events[index];
        std::cerr << owner << '[' << index << "] kind=" << static_cast<int>(e.kind)
            << " begin_us=" << relative_us(e.began, start) << " end_us=" << relative_us(e.ended, start)
            << " requested=" << e.requested << " offset=" << e.offset;
        if (e.kind == EventKind::close || (e.kind == EventKind::writable_select && !e.native_select))
            std::cerr << " returned=unobserved native_error=unobserved";
        else std::cerr << " returned=" << e.returned << " native_error=" << e.native_error;
        std::cerr << " would_block=" << e.would_block
            << " native_select=" << e.native_select << " target=" << e.target
            << " nonblocking=" << e.nonblocking << " logical_invalid=" << e.logical_invalid;
        if (e.kind == EventKind::writable_select || e.kind == EventKind::readable_select)
            std::cerr << " wait_deadline_us=" << relative_us(e.wait_deadline, start);
        if (e.kind == EventKind::writable_select)
            std::cerr << " wait_result=" << static_cast<int>(e.wait_result);
        std::cerr << '\n';
    }
    std::cerr << owner << " trace_overflow=" << trace.overflow << '\n';
}
struct SendProof {
    std::size_t actual_positive_calls{}, qualifying_positive_calls{}, actual_short_calls{}, accepted_sum{};
    bool separated_progress{}, all_waits_original{true};
};
SendProof inspect_sends(const Trace& trace, Clock::time_point start, Clock::time_point deadline) noexcept {
    SendProof proof{};
    bool preceding_positive = false, intervening_block = false, intervening_ready = false;
    for (std::size_t index = 0; index < trace.count; ++index) {
        const auto& e = trace.events[index];
        if (e.kind == EventKind::writable_select) {
            proof.all_waits_original = proof.all_waits_original && e.wait_deadline == deadline;
            if (intervening_block && e.native_select && e.returned > 0 && e.wait_result == DeadlineWaitResult::ready &&
                e.ended >= start && e.ended < deadline) intervening_ready = true;
        } else if (e.kind == EventKind::send && e.returned > 0) {
            ++proof.actual_positive_calls;
            proof.accepted_sum += static_cast<std::size_t>(e.returned);
            if (static_cast<std::size_t>(e.returned) < e.requested) ++proof.actual_short_calls;
            if (e.ended >= start && e.ended < deadline) {
                ++proof.qualifying_positive_calls;
                if (preceding_positive && intervening_block && intervening_ready) proof.separated_progress = true;
                preceding_positive = true; intervening_block = false; intervening_ready = false;
            }
        } else if (e.kind == EventKind::send && e.would_block && e.returned == SOCKET_ERROR &&
                   e.native_error == WSAEWOULDBLOCK && preceding_positive && e.ended >= start && e.ended < deadline) {
            intervening_block = true; intervening_ready = false;
        }
    }
    return proof;
}
bool validate_wire(const nstu::security::Sha256Digest& key, const std::vector<std::byte>& payload,
                   std::span<const std::byte> wire, std::string& error) {
    nstu::protocol::TcpFrameParser parser;
    std::vector<nstu::protocol::TcpFrame> frames;
    if (wire.size() != kWireBytes || !parser.feed(wire, frames) || frames.size() != 1) return false;
    const auto decoded = nstu::control::decode_authenticated_command(key, frames.front(), &error);
    return decoded && frames.front().envelope.payload_bytes == nstu::protocol::kMaxCommandPayload &&
        decoded->envelope.version == nstu::protocol::kCommandVersion &&
        decoded->envelope.type == nstu::protocol::CommandType::lock &&
        decoded->envelope.payload_bytes == kUserBytes && decoded->envelope.request_id == kRequestId &&
        decoded->sequence == 0 && decoded->payload == payload;
}
} // namespace

int main() {
    nstu::net::WinsockRuntime runtime;
    nstu::security::Sha256Digest key{};
    key.fill(std::byte{0x31});
    const std::vector<std::byte> payload(kUserBytes, std::byte{0x4d});
    const nstu::protocol::CommandEnvelope command{
        .version = nstu::protocol::kCommandVersion, .type = nstu::protocol::CommandType::lock,
        .payload_bytes = static_cast<std::uint32_t>(kUserBytes), .request_id = kRequestId};
    std::string framing_error;
    const auto wire = nstu::control::encode_authenticated_command(key, command, 0, payload);
    const bool framing_ok = runtime.ready() && validate_wire(key, payload, wire, framing_error);
    if (!framing_ok) {
        nstu::security::secure_zero(key);
        std::cerr << "native framing prerequisite failed: " << framing_error << '\n';
        assert(framing_ok); return 1;
    }
    Fixture fixture;
    const bool prepared = fixture.prepare();
    const auto& setup = fixture.evidence();
    const bool setup_ok = prepared && setup.failed_stage.empty() && setup.accepted_bytes > 0 &&
        setup.actual_would_block && setup.restore_fionbio_zero_ok;
    if (!setup_ok) {
        fixture.finish(); nstu::security::secure_zero(key); fixture.record_completion();
        print_fixture(fixture.evidence()); assert(setup_ok); return 1;
    }
    Control control;
    // One fixed store, with disjoint regions written by the two socket owners.
    std::array<Event, kEventCapacity> storage{};
    Receiver receiver;
    receiver.control = &control;
    receiver.wire = wire;
    receiver.overall_deadline = fixture.overall_deadline();
    receiver.setup_expected = setup.accepted_bytes;
    receiver.trace.events = std::span<Event>(storage).subspan(kEventCapacity / 2);
    NativeAdapter adapter;
    adapter.sender = &fixture.sender();
    adapter.control = &control;
    adapter.trace.events = std::span<Event>(storage).first(kEventCapacity / 2);
    const DeadlineSendOperations operations{.context = &adapter, .now = adapter_now,
        .set_nonblocking = adapter_mode, .send = adapter_send, .wait_writable = adapter_wait, .close = adapter_close};
    std::string send_error, error_snapshot;
    std::jthread worker;
    StopAndJoin cleanup(control, worker); // Lives until every referenced object can be safely destroyed.
    Clock::time_point target_start{}, target_end{}, original_deadline{};
    int send_result = 0;
    bool invoked = false, exception_seen = false;
    try {
        send_error.reserve(128); error_snapshot.reserve(128);
        worker = std::jthread([&receiver, peer = fixture.take_peer()]() mutable {
            run_receiver(std::move(peer), receiver);
        });
        bool parked = false;
        {
            std::unique_lock lock(control.mutex);
            while (!control.parked && !control.receiver_failed && Clock::now() < fixture.overall_deadline())
                control.changed.wait_until(lock, std::min(fixture.overall_deadline(), Clock::now() + kReceiverSlice));
            parked = control.parked && !control.receiver_failed;
        }
        const auto now = Clock::now();
        if (parked && now + kTargetDuration + kCleanupBudget < fixture.overall_deadline()) {
            target_start = now;
            original_deadline = target_start + kTargetDuration;
            {
                std::lock_guard lock(control.mutex);
                control.target_start = target_start; control.target_deadline = original_deadline; control.start = true;
            }
            control.changed.notify_all();
            invoked = true;
            send_result = nstu::net::detail::send_all_with_deadline(wire, original_deadline, operations, &send_error);
            target_end = Clock::now();
            error_snapshot = send_error;
            {
                std::lock_guard lock(control.mutex);
                control.result_at = target_end; control.result = true;
            }
            control.changed.notify_all();
        }
    } catch (...) { exception_seen = true; }
    const auto stop_begin = Clock::now();
    cleanup.stop_and_join();
    const auto joined = Clock::now();
    fixture.finish(); nstu::security::secure_zero(key); fixture.record_completion();
    const auto elapsed = target_end - target_start;
    const auto join_elapsed = joined - stop_begin;
    const auto receiver_stop_elapsed = receiver.completed > control.stop_requested ?
        receiver.completed - control.stop_requested : Clock::duration::zero();
    const auto proof = inspect_sends(adapter.trace, target_start, original_deadline);
    const bool overall_ok = Clock::now() < fixture.overall_deadline();
    const bool passed = invoked && !exception_seen && fixture.evidence().failed_stage.empty() &&
        fixture.evidence().owners_closed && send_result == -1 && error_snapshot == "TCP send deadline expired" &&
        elapsed >= kTargetDuration && elapsed < std::chrono::milliseconds{800} &&
        proof.qualifying_positive_calls >= 2 && proof.separated_progress && proof.all_waits_original &&
        proof.accepted_sum == adapter.accepted && adapter.accepted < wire.size() &&
        adapter.close_count == 1 && adapter.closed_invalid && receiver.failure_phase == nullptr &&
        receiver.setup_received == receiver.setup_expected && receiver.qualifying_received > 0 &&
        receiver.qualifying_received <= receiver.target_received && receiver.target_received <= kTargetQuota &&
        receiver.closed_invalid && receiver_stop_elapsed < kCleanupBudget && join_elapsed < kCleanupBudget &&
        !adapter.trace.overflow && !receiver.trace.overflow && overall_ok;
    print_fixture(fixture.evidence());
    std::cerr << "NATIVE-DEADLINE passed=" << passed << " invoked=" << invoked << " exception=" << exception_seen
        << " result=" << send_result << " error=" << error_snapshot << " elapsed_us=" << relative_us(target_end, target_start)
        << " target_accepted=" << adapter.accepted << " full_wire=" << wire.size()
        << " positive_calls=" << proof.actual_positive_calls << " qualifying_positive_calls=" << proof.qualifying_positive_calls
        << " observed_short_calls=" << proof.actual_short_calls << " separated_progress=" << proof.separated_progress
        << " all_waits_original=" << proof.all_waits_original << " close_count=" << adapter.close_count
        << " sender_invalid=" << adapter.closed_invalid << " setup_received=" << receiver.setup_received
        << " target_received=" << receiver.target_received << " qualifying_prefix=" << receiver.qualifying_received
        << " late_target_bytes=" << receiver.late_received << " first_qualifying_us="
        << (receiver.qualifying_received == 0 ? 0LL : relative_us(receiver.first_qualifying, target_start))
        << " last_qualifying_us=" << (receiver.qualifying_received == 0 ? 0LL : relative_us(receiver.last_qualifying, target_start))
        << " receiver_phase=" << (receiver.failure_phase == nullptr ? "" : receiver.failure_phase)
        << " receiver_native_error=" << receiver.failure_native_error << " receiver_invalid=" << receiver.closed_invalid
        << " receiver_stop_us=" << std::chrono::duration_cast<std::chrono::microseconds>(receiver_stop_elapsed).count()
        << " join_us=" << std::chrono::duration_cast<std::chrono::microseconds>(join_elapsed).count()
        << " overall_ok=" << overall_ok << '\n';
    print_trace("sender", adapter.trace, target_start);
    print_trace("receiver", receiver.trace, target_start);
    assert(passed);
    return passed ? 0 : 1;
}
