#pragma once

#include "nstu/auth.hpp"
#include "nstu/exam_control.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace nstu::exam::auth {

inline constexpr std::size_t kMaximumInstructorIdBytes = 128;
inline constexpr std::size_t kMaximumPackageIdBytes = 128;
inline constexpr std::size_t kMaximumCandidateIdBytes = 128;
inline constexpr std::size_t kInstructorAuthSignatureBytes = 32;

struct InstructorAuthorizationTicket {
    std::string instructor_id;
    std::string package_id;
    security::Sha256Digest package_digest{};
    SessionId session_id{};
    std::string candidate_id;
    std::uint64_t valid_from_ms = 0;
    std::uint64_t valid_until_ms = 0;
    security::Sha256Digest instructor_signature{};
};

enum class TicketVerificationResult {
    valid,
    invalid_structure,
    expired,
    not_yet_valid,
    signature_mismatch,
    package_mismatch,
    session_mismatch,
    candidate_mismatch
};

[[nodiscard]] const char* ticket_verification_result_to_string(
    TicketVerificationResult result) noexcept;

[[nodiscard]] bool validate_ticket(
    const InstructorAuthorizationTicket& ticket,
    std::uint64_t current_time_ms = 0) noexcept;

[[nodiscard]] std::vector<std::byte> compute_ticket_canonical_transcript(
    const InstructorAuthorizationTicket& ticket);

[[nodiscard]] std::optional<security::Sha256Digest> compute_ticket_signature(
    std::span<const std::byte> key,
    const InstructorAuthorizationTicket& ticket);

[[nodiscard]] bool sign_ticket(
    std::span<const std::byte> key,
    InstructorAuthorizationTicket& ticket);

[[nodiscard]] bool verify_ticket_signature(
    std::span<const std::byte> key,
    const InstructorAuthorizationTicket& ticket) noexcept;

[[nodiscard]] TicketVerificationResult verify_ticket_for_exam(
    std::span<const std::byte> key,
    const InstructorAuthorizationTicket& ticket,
    const ExamStartRequest& request,
    std::uint64_t current_time_ms) noexcept;

[[nodiscard]] std::vector<std::byte> encode_instructor_ticket(
    const InstructorAuthorizationTicket& ticket);

[[nodiscard]] std::optional<InstructorAuthorizationTicket> decode_instructor_ticket(
    std::span<const std::byte> wire);

}  // namespace nstu::exam::auth
