#pragma once

#include "nstu/agent_protocol.hpp"
#include "nstu/exam_sync.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

namespace nstu::client {

// The bridge is deliberately small: exam responses are acknowledgements or
// bounded state chunks, never an unbounded second copy of the answer journal.
inline constexpr std::size_t kMaximumExamBridgeMessages =
    nstu::exam::kMaximumStateChunks;
inline constexpr std::size_t kMaximumExamBridgeCommands = 4;

struct QueuedExamCommand {
    AgentMessage message;
    std::uint64_t publication = 0;
};

// The hook only touches this lock-free, bounded aggregate. Reset changes its
// generation so a concurrent record cannot cross an enable/start/stop boundary.
class ExamShortcutCounter {
public:
    void reset(bool enabled = false) noexcept;
    void record(std::uint32_t count = 1) noexcept;
    [[nodiscard]] std::uint32_t drain() noexcept;
    [[nodiscard]] bool enabled() const noexcept;

private:
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    std::atomic<std::uint64_t> state_{0};
};

class ExamBridge {
public:
    ExamBridge() = default;
    ExamBridge(const ExamBridge&) = delete;
    ExamBridge& operator=(const ExamBridge&) = delete;

    // Publishes only service-to-agent exam responses: answer acks, bounded
    // state chunks, and the synchronized-start exam_begin. Older responses are
    // discarded when the host is temporarily busy; durable state remains in
    // the service outbox/server journal and can be requested again.
    [[nodiscard]] bool publish(AgentMessage message) noexcept;
    [[nodiscard]] std::optional<AgentMessage> try_pop();
    [[nodiscard]] std::size_t size() const;

    // Publishes telemetry enable to an independent coalesced slot. Telemetry
    // enable never enters the answer-response FIFO, so it cannot evict answer
    // ACKs, state chunks, or exam_begin, and is never evicted by them.
    [[nodiscard]] bool publish_telemetry_enable(AgentMessage message) noexcept;
    [[nodiscard]] std::optional<AgentMessage> try_pop_telemetry_enable() noexcept;
    [[nodiscard]] std::optional<AgentMessage> try_pop_telemetry_enable(
        std::uint64_t command_publication) noexcept;
    [[nodiscard]] bool has_telemetry_enable() const noexcept;

    // Serialized pipe publication and UI consumption share this bounded
    // command boundary; stop supersedes old starts, while start keeps a stop.
    [[nodiscard]] bool publish_exam_command(AgentMessage message) noexcept;
    [[nodiscard]] std::optional<QueuedExamCommand> try_pop_exam_command() noexcept;
    void clear_exam_commands() noexcept;

    // Internal startup teardown discards old responses while retaining a
    // subsequently published enable. Validation then keeps only the new tuple.
    void clear_responses();
    [[nodiscard]] bool prepare_for_start(const exam::StateRequest& context,
                                        std::uint64_t command_publication = 0) noexcept;
    // A queued UI command may clear only data published at/before itself.
    // A standalone explicit clear still invalidates the complete enable slot.
    void clear_through_publication(std::uint64_t command_publication);

    void clear();

private:
    mutable std::mutex mutex_;
    std::deque<AgentMessage> messages_;
    std::optional<AgentMessage> latest_telemetry_enable_;
    std::deque<QueuedExamCommand> exam_commands_;
    std::uint64_t command_publication_ = 0;
    std::uint64_t enable_publication_ = 0;
};

// Function-local construction avoids static initialization ordering issues in
// the agent executable and keeps the optional native WebView2 host independent
// from the transport thread's lifetime.
[[nodiscard]] ExamBridge& exam_bridge();

} // namespace nstu::client
