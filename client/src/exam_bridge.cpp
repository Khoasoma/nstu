#include "nstu/exam_bridge.hpp"
#include "nstu/exam_telemetry.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace nstu::client {

namespace {
constexpr std::uint64_t kShortcutCountMask = 0xffffffffULL;
constexpr std::uint64_t kShortcutEnabled = 1ULL << 32;
constexpr std::uint64_t kShortcutGenerationStep = 1ULL << 33;
} // namespace

void ExamShortcutCounter::reset(bool enabled) noexcept {
    const auto previous = state_.load(std::memory_order_relaxed);
    const auto generation = (previous + kShortcutGenerationStep) &
                            ~(kShortcutCountMask | kShortcutEnabled);
    state_.store(generation | (enabled ? kShortcutEnabled : 0),
                 std::memory_order_relaxed);
}

void ExamShortcutCounter::record(std::uint32_t count) noexcept {
    auto previous = state_.load(std::memory_order_relaxed);
    if ((previous & kShortcutEnabled) == 0) return;
    const auto generation = previous & ~kShortcutCountMask;
    do {
        const auto current_count = previous & kShortcutCountMask;
        const auto next_count = std::min<std::uint64_t>(
            kShortcutCountMask, current_count + count);
        if (state_.compare_exchange_weak(previous, generation | next_count,
                                         std::memory_order_relaxed)) return;
    } while ((previous & ~kShortcutCountMask) == generation);
}

std::uint32_t ExamShortcutCounter::drain() noexcept {
    auto previous = state_.load(std::memory_order_relaxed);
    while (!state_.compare_exchange_weak(previous, previous & ~kShortcutCountMask,
                                         std::memory_order_relaxed)) {}
    return static_cast<std::uint32_t>(previous & kShortcutCountMask);
}

bool ExamShortcutCounter::enabled() const noexcept {
    return (state_.load(std::memory_order_relaxed) & kShortcutEnabled) != 0;
}

bool ExamBridge::publish(AgentMessage message) noexcept {
    if (message.type == AgentMessageType::exam_telemetry_enable) {
        return publish_telemetry_enable(std::move(message));
    }
    if ((message.type != AgentMessageType::exam_answer_ack &&
         message.type != AgentMessageType::exam_state_response &&
         message.type != AgentMessageType::exam_begin) ||
        message.payload.size() > kMaximumAgentPayloadBytes) {
        return false;
    }
    try {
        std::scoped_lock lock(mutex_);
        if (messages_.size() >= kMaximumExamBridgeMessages) {
            messages_.pop_front();
        }
        messages_.push_back(std::move(message));
        return true;
    } catch (...) {
        // The agent must not terminate because a diagnostic/UI bridge ran out
        // of memory. The durable service outbox remains authoritative.
        return false;
    }
}

bool ExamBridge::publish_telemetry_enable(AgentMessage message) noexcept {
    if (message.type != AgentMessageType::exam_telemetry_enable ||
        message.payload.size() > kMaximumAgentPayloadBytes) {
        return false;
    }
    try {
        std::scoped_lock lock(mutex_);
        if (command_publication_ == std::numeric_limits<std::uint64_t>::max()) return false;
        latest_telemetry_enable_ = std::move(message);
        enable_publication_ = command_publication_;
        return true;
    } catch (...) {
        return false;
    }
}

std::optional<AgentMessage> ExamBridge::try_pop_telemetry_enable() noexcept {
    try {
        std::scoped_lock lock(mutex_);
        if (!latest_telemetry_enable_.has_value()) return std::nullopt;
        auto result = std::move(latest_telemetry_enable_);
        latest_telemetry_enable_ = std::nullopt;
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<AgentMessage> ExamBridge::try_pop_telemetry_enable(
    std::uint64_t command_publication) noexcept {
    try {
        std::scoped_lock lock(mutex_);
        if (!latest_telemetry_enable_ || enable_publication_ > command_publication) {
            return std::nullopt;
        }
        if (enable_publication_ < command_publication) {
            latest_telemetry_enable_.reset();
            return std::nullopt;
        }
        auto result = std::move(latest_telemetry_enable_);
        latest_telemetry_enable_.reset();
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

bool ExamBridge::has_telemetry_enable() const noexcept {
    try {
        std::scoped_lock lock(mutex_);
        return latest_telemetry_enable_.has_value();
    } catch (...) {
        return false;
    }
}

bool ExamBridge::publish_exam_command(AgentMessage message) noexcept {
    if ((message.type != AgentMessageType::exam_start &&
         message.type != AgentMessageType::exam_stop) ||
        message.payload.size() > kMaximumAgentPayloadBytes) return false;
    try {
        std::scoped_lock lock(mutex_);
        if (command_publication_ == std::numeric_limits<std::uint64_t>::max()) {
            latest_telemetry_enable_.reset();
            exam_commands_.clear();
            return false;
        }
        const auto publication = ++command_publication_;
        // This is the serialized wire boundary, before deferred UI teardown.
        // A later enable belongs to this command; prior enable is obsolete.
        latest_telemetry_enable_.reset();
        if (message.type == AgentMessageType::exam_stop) {
            exam_commands_.clear();
        } else {
            std::erase_if(exam_commands_, [](const auto& command) {
                return command.message.type == AgentMessageType::exam_start;
            });
        }
        if (exam_commands_.size() >= kMaximumExamBridgeCommands) exam_commands_.pop_front();
        exam_commands_.push_back({std::move(message), publication});
        return true;
    } catch (...) {
        return false;
    }
}

std::optional<QueuedExamCommand> ExamBridge::try_pop_exam_command() noexcept {
    try {
        std::scoped_lock lock(mutex_);
        if (exam_commands_.empty()) return std::nullopt;
        auto command = std::move(exam_commands_.front());
        exam_commands_.pop_front();
        return command;
    } catch (...) {
        return std::nullopt;
    }
}

void ExamBridge::clear_exam_commands() noexcept {
    try {
        std::scoped_lock lock(mutex_);
        exam_commands_.clear();
    } catch (...) {}
}

void ExamBridge::clear_responses() {
    std::scoped_lock lock(mutex_);
    messages_.clear();
}

bool ExamBridge::prepare_for_start(const exam::StateRequest& context,
                                  std::uint64_t command_publication) noexcept {
    try {
        std::scoped_lock lock(mutex_);
        messages_.clear();
        if (latest_telemetry_enable_ && enable_publication_ <= command_publication) {
            const auto enable = exam::decode_telemetry_enable(latest_telemetry_enable_->payload);
            if (enable_publication_ < command_publication ||
                !enable || enable->package_id != context.package_id ||
                enable->package_digest != context.package_digest ||
                enable->client_id != context.client_id ||
                enable->session_id != context.session_id ||
                enable->candidate_id != context.candidate_id) {
                latest_telemetry_enable_.reset();
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

void ExamBridge::clear_through_publication(std::uint64_t command_publication) {
    std::scoped_lock lock(mutex_);
    messages_.clear();
    if (latest_telemetry_enable_ && enable_publication_ <= command_publication) {
        latest_telemetry_enable_.reset();
    }
}

std::optional<AgentMessage> ExamBridge::try_pop() {
    std::scoped_lock lock(mutex_);
    if (messages_.empty()) {
        return std::nullopt;
    }
    AgentMessage message = std::move(messages_.front());
    messages_.pop_front();
    return message;
}

std::size_t ExamBridge::size() const {
    std::scoped_lock lock(mutex_);
    return messages_.size();
}

void ExamBridge::clear() {
    std::scoped_lock lock(mutex_);
    messages_.clear();
    latest_telemetry_enable_ = std::nullopt;
}

ExamBridge& exam_bridge() {
    static ExamBridge bridge;
    return bridge;
}

} // namespace nstu::client
