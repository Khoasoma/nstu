#pragma once

#include "nstu/exam_sync.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nstu::exam::programming {

inline constexpr std::size_t kMaximumIdBytes = 96;
inline constexpr std::size_t kMaximumSourceBytes = 32u * 1024u;
inline constexpr std::size_t kMaximumInventoryEntries = 64;
inline constexpr std::uint64_t kMaximumDraftRevision = (1ULL << 53) - 1;

enum class Operation : std::uint8_t {
    draft = 1,
    submit = 2,
    run = 3,
    cancel = 4,
    recover = 5,
    finalize = 6,
    ready = 7,
};

struct Request {
    StateRequest context;
    Operation operation = Operation::draft;
    std::string request_id;
    std::string problem_id;
    std::uint32_t problem_revision = 1;
    std::string language;
    std::uint64_t draft_revision = 0;
    // An empty draft clears saved source. Submit and run require source.
    std::string source;
    std::string sample_id;
    std::string target_run_id;
    std::vector<std::string> receipt_ids;
    std::vector<std::string> no_source_problem_ids;
    // Only the LocalSystem service sets this bit after a protected policy probe.
    bool workstation_policy_ready = false;
};

[[nodiscard]] std::string_view operation_name(Operation operation) noexcept;
[[nodiscard]] bool validate_request(const Request& request);
[[nodiscard]] bool validate_service_request(const Request& request,
                                          const StateRequest& issued_context);
[[nodiscard]] std::vector<std::byte> encode_request(const Request& request);
[[nodiscard]] std::optional<Request> decode_request(
    std::span<const std::byte> payload);

struct Response {
    StateRequest context;
    Operation operation = Operation::ready;
    std::string request_id;
    std::string json;
    std::string status;
    std::uint64_t server_time = 0;
    bool runner_ready = false;
};

struct LifecycleReply {
    std::uint64_t server_start = 0;
    std::uint64_t deadline = 0;
    std::uint64_t server_time = 0;
};

[[nodiscard]] std::string context_json(const StateRequest& context);
[[nodiscard]] std::string request_json(const Request& request);
[[nodiscard]] std::string unavailable_response_json(const Request& request,
                                                   std::string_view reason);
[[nodiscard]] std::optional<Response> parse_response_json(
    std::string_view json, const Request* expected = nullptr);
[[nodiscard]] bool validate_prepared_json(std::string_view json,
                                        const StateRequest& context);
[[nodiscard]] bool validate_known_context_json(std::string_view json,
                                             const StateRequest& context);
[[nodiscard]] std::optional<LifecycleReply> parse_lifecycle_json(
    std::string_view json, const StateRequest& context,
    std::string_view request_id, bool begin);

} // namespace nstu::exam::programming
