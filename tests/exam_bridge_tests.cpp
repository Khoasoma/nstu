#include "nstu/exam_bridge.hpp"
#include "nstu/exam_telemetry.hpp"
#include "nstu/exam_host.hpp"
#include "nstu/exam_control.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

nstu::client::AgentMessage make_message(std::uint8_t marker) {
    return {nstu::client::AgentMessageType::exam_answer_ack,
            {static_cast<std::byte>(marker)}};
}

nstu::client::AgentMessage make_enable_message(std::uint8_t marker) {
    return {nstu::client::AgentMessageType::exam_telemetry_enable,
            {static_cast<std::byte>(marker)}};
}

void test_basic_bridge() {
    using namespace nstu::client;

    ExamBridge bridge;
    assert(bridge.size() == 0);
    assert(!bridge.publish({AgentMessageType::chat, {std::byte{1}}}));
    assert(bridge.publish(make_message(1)));
    assert(bridge.size() == 1);
    const auto first = bridge.try_pop();
    assert(first.has_value());
    assert(first->type == AgentMessageType::exam_answer_ack);
    assert(first->payload.size() == 1 &&
           first->payload.front() == std::byte{1});
    assert(!bridge.try_pop().has_value());

    // The synchronized-start exam_begin rides the same bridge as acks.
    assert(bridge.publish({AgentMessageType::exam_begin, {std::byte{9}}}));
    const auto begin = bridge.try_pop();
    assert(begin.has_value());
    assert(begin->type == AgentMessageType::exam_begin);
    assert(begin->payload.size() == 1 &&
           begin->payload.front() == std::byte{9});
    assert(!bridge.try_pop().has_value());

    for (std::size_t index = 0; index < kMaximumExamBridgeMessages + 5;
         ++index) {
        assert(bridge.publish(make_message(static_cast<std::uint8_t>(index))));
    }
    assert(bridge.size() == kMaximumExamBridgeMessages);
    const auto oldest_retained = bridge.try_pop();
    assert(oldest_retained.has_value());
    assert(oldest_retained->payload.front() ==
           static_cast<std::byte>(5));
    bridge.clear();
    assert(bridge.size() == 0);
}

void test_independent_coalesced_telemetry_enable() {
    using namespace nstu::client;

    ExamBridge bridge;
    assert(!bridge.has_telemetry_enable());
    assert(!bridge.try_pop_telemetry_enable().has_value());

    // 1. Publishing telemetry enable does not enter messages_ FIFO
    assert(bridge.publish_telemetry_enable(make_enable_message(101)));
    assert(bridge.has_telemetry_enable());
    assert(bridge.size() == 0);

    // 2. Generic publish with telemetry_enable routes to coalesced state
    assert(bridge.publish(make_enable_message(102)));
    assert(bridge.has_telemetry_enable());
    assert(bridge.size() == 0);

    // 3. Popping retrieves the coalesced enable and clears the slot
    const auto popped_enable = bridge.try_pop_telemetry_enable();
    assert(popped_enable.has_value());
    assert(popped_enable->type == AgentMessageType::exam_telemetry_enable);
    assert(popped_enable->payload.front() == static_cast<std::byte>(102));
    assert(!bridge.has_telemetry_enable());
    assert(!bridge.try_pop_telemetry_enable().has_value());
    assert(bridge.size() == 0);

    // 4. Saturated FIFO test: telemetry enable cannot evict answer ACK,
    // and a full answer ACK FIFO cannot evict or block telemetry enable.
    for (std::size_t i = 0; i < kMaximumExamBridgeMessages; ++i) {
        assert(bridge.publish(make_message(static_cast<std::uint8_t>(i))));
    }
    assert(bridge.size() == kMaximumExamBridgeMessages);

    // Publish telemetry enable while FIFO is completely saturated
    assert(bridge.publish_telemetry_enable(make_enable_message(200)));
    // Size of answer FIFO is still exactly kMaximumExamBridgeMessages (enable did NOT evict any ACK)
    assert(bridge.size() == kMaximumExamBridgeMessages);
    assert(bridge.has_telemetry_enable());

    // Push 10 more answer ACKs into the saturated FIFO
    for (std::size_t i = 0; i < 10; ++i) {
        assert(bridge.publish(make_message(static_cast<std::uint8_t>(100 + i))));
    }
    assert(bridge.size() == kMaximumExamBridgeMessages);

    // Enable is still present and was NOT evicted by answer ACKs
    assert(bridge.has_telemetry_enable());
    const auto popped_after_saturation = bridge.try_pop_telemetry_enable();
    assert(popped_after_saturation.has_value());
    assert(popped_after_saturation->payload.front() == static_cast<std::byte>(200));

    // Verify all answer ACKs in FIFO are preserved in correct order
    for (std::size_t i = 0; i < kMaximumExamBridgeMessages; ++i) {
        const auto msg = bridge.try_pop();
        assert(msg.has_value());
        assert(msg->type == AgentMessageType::exam_answer_ack);
        assert(msg->type != AgentMessageType::exam_telemetry_enable);
        const auto expected = i < kMaximumExamBridgeMessages - 10
            ? static_cast<std::byte>(i + 10)
            : static_cast<std::byte>(100 + i - (kMaximumExamBridgeMessages - 10));
        assert(msg->payload == std::vector<std::byte>{expected});
    }
    assert(!bridge.try_pop().has_value());
    assert(bridge.size() == 0);

    // 5. Rejection of invalid message types or oversized payloads
    assert(!bridge.publish_telemetry_enable({AgentMessageType::chat, {std::byte{1}}}));
    assert(!bridge.publish_telemetry_enable({AgentMessageType::exam_answer_ack, {std::byte{1}}}));
    std::vector<std::byte> oversized(kMaximumAgentPayloadBytes + 1, std::byte{0});
    assert(!bridge.publish_telemetry_enable({AgentMessageType::exam_telemetry_enable, oversized}));

    // 6. Clear removes both FIFO and coalesced enable
    assert(bridge.publish(make_message(1)));
    assert(bridge.publish_telemetry_enable(make_enable_message(1)));
    assert(bridge.size() == 1);
    assert(bridge.has_telemetry_enable());
    bridge.clear();
    assert(bridge.size() == 0);
    assert(!bridge.has_telemetry_enable());
}

nstu::exam::TelemetryEnable make_bound_enable() {
    nstu::exam::TelemetryEnable enable;
    enable.package_id = "startup-package";
    enable.package_digest[0] = std::byte{1};
    enable.client_id[0] = std::byte{2};
    enable.session_id[0] = std::byte{3};
    enable.candidate_id = "candidate-startup";
    enable.server_time_unix_milliseconds = 1700000000000ULL;
    return enable;
}

void test_matching_enable_survives_queued_start() {
    using namespace nstu::client;
    const auto enable = make_bound_enable();
    const auto encoded = nstu::exam::encode_telemetry_enable(enable);
    assert(!encoded.empty());
    nstu::exam::StateRequest context;
    context.package_id = enable.package_id;
    context.package_digest = enable.package_digest;
    context.client_id = enable.client_id;
    context.session_id = enable.session_id;
    context.candidate_id = enable.candidate_id;

    ExamBridge bridge;
    assert(bridge.publish(make_message(1)));
    assert(bridge.publish({AgentMessageType::exam_telemetry_enable, encoded}));
    // UI teardown preserves the slot published after the queued start command.
    bridge.clear_responses();
    assert(bridge.prepare_for_start(context));
    assert(bridge.size() == 0);
    const auto pending = bridge.try_pop_telemetry_enable();
    assert(pending.has_value());
    assert(pending->payload == encoded);
    const auto decoded = nstu::exam::decode_telemetry_enable(pending->payload);
    assert(decoded.has_value());
    assert(decoded->candidate_id == context.candidate_id);
    assert(!bridge.try_pop_telemetry_enable().has_value());

    auto wrong = enable;
    wrong.session_id[0] = std::byte{4};
    assert(bridge.publish({AgentMessageType::exam_telemetry_enable,
                           nstu::exam::encode_telemetry_enable(wrong)}));
    assert(bridge.prepare_for_start(context));
    assert(!bridge.has_telemetry_enable());
    assert(bridge.publish(make_enable_message(9)));
    assert(bridge.prepare_for_start(context));
    assert(!bridge.has_telemetry_enable());

    assert(bridge.publish({AgentMessageType::exam_telemetry_enable, encoded}));
    assert(bridge.publish({AgentMessageType::exam_telemetry_enable, encoded}));
    assert(bridge.prepare_for_start(context));
    assert(bridge.try_pop_telemetry_enable()->payload == encoded);
    assert(!bridge.try_pop_telemetry_enable().has_value());
    assert(bridge.publish({AgentMessageType::exam_telemetry_enable, encoded}));
    bridge.clear();
    assert(!bridge.has_telemetry_enable());
}

void test_saturated_mixed_response_order() {
    using namespace nstu::client;
    ExamBridge bridge;
    std::vector<AgentMessage> expected;
    for (std::size_t i = 0; i < kMaximumExamBridgeMessages; ++i) {
        const auto type = i % 3 == 0 ? AgentMessageType::exam_answer_ack
            : i % 3 == 1 ? AgentMessageType::exam_state_response
                         : AgentMessageType::exam_begin;
        expected.push_back({type, {static_cast<std::byte>(i)}});
        assert(bridge.publish(expected.back()));
    }
    const auto enable_bytes = nstu::exam::encode_telemetry_enable(make_bound_enable());
    assert(bridge.publish({AgentMessageType::exam_telemetry_enable, enable_bytes}));
    assert(bridge.size() == expected.size());
    assert(bridge.try_pop_telemetry_enable()->payload == enable_bytes);
    for (const auto& item : expected) {
        const auto popped = bridge.try_pop();
        assert(popped.has_value());
        assert(popped->type == item.type);
        assert(popped->payload == item.payload);
    }
    assert(!bridge.try_pop().has_value());
}

void test_shortcut_collection_boundaries() {
    nstu::client::ExamShortcutCounter counter;
    counter.record(7);
    assert(counter.drain() == 0);
    counter.reset(true);
    counter.record(3);
    counter.record(2);
    assert(counter.drain() == 5);
    assert(counter.drain() == 0);
    counter.record(9);
    counter.reset();
    counter.record(11);
    assert(counter.drain() == 0);
    counter.reset(true);
    assert(counter.drain() == 0);
    counter.record(4);
    counter.reset(true);
    assert(counter.drain() == 0);
    counter.record(std::numeric_limits<std::uint32_t>::max());
    counter.record(1);
    assert(counter.drain() == std::numeric_limits<std::uint32_t>::max());
}

void test_stop_start_enable_publication_survives_ui_teardown() {
    using namespace nstu::client;
    auto& bridge = exam_bridge();
    bridge.clear();
    bridge.clear_exam_commands();
    const auto enable = make_bound_enable();
    nstu::exam::ExamStartRequest request;
    request.package_root = "C:/NSTU/exams/startup";
    request.web_root = "C:/NSTU/exams/startup/exam/web";
    request.package_id = enable.package_id;
    request.package_digest = enable.package_digest;
    request.client_id = enable.client_id;
    request.session_id = enable.session_id;
    request.candidate_id = enable.candidate_id;
    const auto start_bytes = nstu::exam::encode_exam_start_request(request);
    const auto enable_bytes = nstu::exam::encode_telemetry_enable(enable);
    assert(!start_bytes.empty());
    assert(!enable_bytes.empty());
    // The exact production command queue receives all three messages before
    // UI stop(old) runs. Its teardown must leave the later enable(new) intact.
    assert(bridge.publish_exam_command({AgentMessageType::exam_stop, {}}));
    assert(bridge.publish_exam_command({AgentMessageType::exam_start, start_bytes}));
    assert(bridge.publish_telemetry_enable({AgentMessageType::exam_telemetry_enable, enable_bytes}));
    const auto stop = bridge.try_pop_exam_command();
    assert(stop.has_value());
    assert(stop->message.type == AgentMessageType::exam_stop);
    ExamHost old_host;
    old_host.stop(stop->publication);
    assert(bridge.has_telemetry_enable());
    const auto start = bridge.try_pop_exam_command();
    assert(start.has_value());
    assert(start->message.type == AgentMessageType::exam_start);
    assert(start->publication > stop->publication);
    assert(start->message.payload == start_bytes);
    assert(!bridge.try_pop_exam_command().has_value());
    // The old UI generation may poll, but cannot consume the future enable.
    assert(!bridge.try_pop_telemetry_enable(stop->publication).has_value());
    assert(bridge.has_telemetry_enable());
    nstu::exam::StateRequest context;
    context.package_id = enable.package_id;
    context.package_digest = enable.package_digest;
    context.client_id = enable.client_id;
    context.session_id = enable.session_id;
    context.candidate_id = enable.candidate_id;
    assert(bridge.prepare_for_start(context, start->publication));
    const auto retained = bridge.try_pop_telemetry_enable(start->publication);
    assert(retained.has_value());
    assert(retained->payload == enable_bytes);

    ExamHostTelemetryState telemetry;
    assert(telemetry.start(context));
    assert(telemetry.apply_enable(enable));
    telemetry.record_blocked_shortcuts(9);
    telemetry.stop();
    assert(!telemetry.apply_enable(enable));
    assert(!telemetry.drain_blocked_shortcuts(100, 1000).has_value());
    assert(telemetry.start(context));
    assert(telemetry.apply_enable(enable));
    assert(!telemetry.drain_blocked_shortcuts(100, 1000).has_value());

    // Standalone explicit stop continues to invalidate all pending enable.
    assert(bridge.publish_telemetry_enable({AgentMessageType::exam_telemetry_enable, enable_bytes}));
    old_host.stop();
    assert(!bridge.has_telemetry_enable());
    bridge.clear();
}

void test_future_enable_survives_failed_earlier_start() {
    using namespace nstu::client;
    auto& bridge = exam_bridge();
    bridge.clear();
    bridge.clear_exam_commands();
    auto earlier = make_bound_enable();
    auto future = earlier;
    future.session_id[0] = std::byte{5};
    const auto make_start = [](const nstu::exam::TelemetryEnable& enable) {
        nstu::exam::ExamStartRequest request;
        request.package_root = "C:/NSTU/exams/startup";
        request.package_id = enable.package_id;
        request.package_digest = enable.package_digest;
        request.client_id = enable.client_id;
        request.session_id = enable.session_id;
        request.candidate_id = enable.candidate_id;
        return AgentMessage{AgentMessageType::exam_start,
                            nstu::exam::encode_exam_start_request(request)};
    };
    assert(bridge.publish_exam_command(make_start(earlier)));
    const auto old_start = bridge.try_pop_exam_command();
    assert(old_start.has_value());
    assert(bridge.publish_exam_command(make_start(future)));
    const auto future_bytes = nstu::exam::encode_telemetry_enable(future);
    assert(bridge.publish_telemetry_enable({AgentMessageType::exam_telemetry_enable, future_bytes}));

    nstu::exam::StateRequest old_context;
    old_context.package_id = earlier.package_id;
    old_context.package_digest = earlier.package_digest;
    old_context.client_id = earlier.client_id;
    old_context.session_id = earlier.session_id;
    old_context.candidate_id = earlier.candidate_id;
    assert(bridge.prepare_for_start(old_context, old_start->publication));
    assert(bridge.has_telemetry_enable());
    assert(!bridge.try_pop_telemetry_enable(old_start->publication).has_value());
    // A real failed start returns before any window/hook is created. Its
    // cleanup and subsequent disabled-host drain must retain the later slot.
    ExamHost host;
    std::string error;
    assert(!host.start(nullptr, {}, {}, &error, old_start->publication));
    assert(!error.empty());
    assert(host.command_publication() == old_start->publication);
    assert(!host.telemetry_enabled());
    host.drain_bridge();
    host.stop(old_start->publication);
    assert(bridge.has_telemetry_enable());
    const auto new_start = bridge.try_pop_exam_command();
    assert(new_start.has_value());
    assert(new_start->publication > old_start->publication);
    auto future_context = old_context;
    future_context.session_id = future.session_id;
    assert(bridge.prepare_for_start(future_context, new_start->publication));
    const auto enabled = bridge.try_pop_telemetry_enable(new_start->publication);
    assert(enabled.has_value());
    assert(enabled->payload == future_bytes);
    assert(!bridge.try_pop_telemetry_enable(new_start->publication).has_value());

    assert(bridge.publish_exam_command(make_start(future)));
    assert(bridge.publish_telemetry_enable({AgentMessageType::exam_telemetry_enable, future_bytes}));
    // A newer wire stop invalidates prior enable immediately, before UI drain.
    assert(bridge.publish_exam_command({AgentMessageType::exam_stop, {}}));
    assert(!bridge.has_telemetry_enable());
    const auto terminal = bridge.try_pop_exam_command();
    assert(terminal.has_value());
    assert(terminal->message.type == AgentMessageType::exam_stop);
    assert(!bridge.try_pop_exam_command().has_value());
    host.stop(terminal->publication);
    assert(!bridge.has_telemetry_enable());
    // An enable for another tuple is still refused within the current
    // publication; only a future publication receives preservation.
    assert(bridge.publish_exam_command(make_start(earlier)));
    const auto current = bridge.try_pop_exam_command();
    assert(current.has_value());
    assert(bridge.publish_telemetry_enable({AgentMessageType::exam_telemetry_enable, future_bytes}));
    assert(bridge.prepare_for_start(old_context, current->publication));
    assert(!bridge.has_telemetry_enable());
    assert(!bridge.publish_exam_command({AgentMessageType::chat, {}}));
    bridge.clear();
}

} // namespace

int main() {
    test_basic_bridge();
    test_independent_coalesced_telemetry_enable();
    test_matching_enable_survives_queued_start();
    test_saturated_mixed_response_order();
    test_shortcut_collection_boundaries();
    test_stop_start_enable_publication_survives_ui_teardown();
    test_future_enable_survives_failed_earlier_start();
    return 0;
}
