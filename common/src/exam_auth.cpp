#include "nstu/exam_auth.hpp"

#include <algorithm>
#include <cstring>

namespace nstu::exam::auth {

namespace {

inline constexpr std::uint32_t kTicketMagic = 0x5449434BU; // "TICK"
inline constexpr std::uint16_t kTicketVersion = 1u;
inline constexpr std::size_t kFixedHeaderBytes = 110u;
inline constexpr std::size_t kMaximumTicketPayloadBytes = 4096u;

template <typename T>
void append_le(std::vector<std::byte>& out, T val) {
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        out.push_back(static_cast<std::byte>((val >> (i * 8)) & 0xFF));
    }
}

template <typename T>
T read_le(std::span<const std::byte> bytes, std::size_t offset) {
    T val = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        val |= (static_cast<T>(std::to_integer<std::uint8_t>(bytes[offset + i])) << (i * 8));
    }
    return val;
}

bool non_zero(std::span<const std::byte> bytes) noexcept {
    return std::any_of(bytes.begin(), bytes.end(), [](std::byte b) {
        return b != std::byte{0};
    });
}

bool valid_utf8(std::string_view text, std::size_t max_bytes) noexcept {
    if (text.size() > max_bytes) return false;
    std::size_t i = 0;
    while (i < text.size()) {
        const auto byte = static_cast<unsigned char>(text[i]);
        if (byte <= 0x7F) {
            ++i;
        } else if ((byte & 0xE0) == 0xC0) {
            if (i + 1 >= text.size() ||
                (static_cast<unsigned char>(text[i + 1]) & 0xC0) != 0x80) {
                return false;
            }
            i += 2;
        } else if ((byte & 0xF0) == 0xE0) {
            if (i + 2 >= text.size() ||
                (static_cast<unsigned char>(text[i + 1]) & 0xC0) != 0x80 ||
                (static_cast<unsigned char>(text[i + 2]) & 0xC0) != 0x80) {
                return false;
            }
            i += 3;
        } else if ((byte & 0xF8) == 0xF0) {
            if (i + 3 >= text.size() ||
                (static_cast<unsigned char>(text[i + 1]) & 0xC0) != 0x80 ||
                (static_cast<unsigned char>(text[i + 2]) & 0xC0) != 0x80 ||
                (static_cast<unsigned char>(text[i + 3]) & 0xC0) != 0x80) {
                return false;
            }
            i += 4;
        } else {
            return false;
        }
    }
    return true;
}

}  // namespace

const char* ticket_verification_result_to_string(
    TicketVerificationResult result) noexcept {
    switch (result) {
        case TicketVerificationResult::valid:
            return "valid";
        case TicketVerificationResult::invalid_structure:
            return "invalid_structure";
        case TicketVerificationResult::expired:
            return "expired";
        case TicketVerificationResult::not_yet_valid:
            return "not_yet_valid";
        case TicketVerificationResult::signature_mismatch:
            return "signature_mismatch";
        case TicketVerificationResult::package_mismatch:
            return "package_mismatch";
        case TicketVerificationResult::session_mismatch:
            return "session_mismatch";
        case TicketVerificationResult::candidate_mismatch:
            return "candidate_mismatch";
    }
    return "unknown";
}

bool validate_ticket(
    const InstructorAuthorizationTicket& ticket,
    std::uint64_t current_time_ms) noexcept {
    if (ticket.instructor_id.empty() ||
        !valid_utf8(ticket.instructor_id, kMaximumInstructorIdBytes) ||
        ticket.package_id.empty() ||
        !valid_utf8(ticket.package_id, kMaximumPackageIdBytes) ||
        (!ticket.candidate_id.empty() &&
         !valid_utf8(ticket.candidate_id, kMaximumCandidateIdBytes))) {
        return false;
    }
    if (!non_zero(ticket.package_digest) || !non_zero(ticket.session_id)) {
        return false;
    }
    if (ticket.valid_from_ms >= ticket.valid_until_ms) {
        return false;
    }
    if (current_time_ms > 0) {
        if (current_time_ms < ticket.valid_from_ms ||
            current_time_ms > ticket.valid_until_ms) {
            return false;
        }
    }
    return true;
}

std::vector<std::byte> compute_ticket_canonical_transcript(
    const InstructorAuthorizationTicket& ticket) {
    std::vector<std::byte> out;
    const std::string_view domain = "NSTU-INSTRUCTOR-TICKET-V1";
    out.reserve(256);
    for (char c : domain) {
        out.push_back(static_cast<std::byte>(c));
    }
    append_le<std::uint32_t>(out, kTicketMagic);
    append_le<std::uint16_t>(out, kTicketVersion);
    append_le<std::uint16_t>(out, 0); // reserved
    append_le<std::uint64_t>(out, ticket.valid_from_ms);
    append_le<std::uint64_t>(out, ticket.valid_until_ms);
    out.insert(out.end(), ticket.package_digest.begin(), ticket.package_digest.end());
    out.insert(out.end(), ticket.session_id.begin(), ticket.session_id.end());
    append_le<std::uint16_t>(out, static_cast<std::uint16_t>(ticket.instructor_id.size()));
    append_le<std::uint16_t>(out, static_cast<std::uint16_t>(ticket.package_id.size()));
    append_le<std::uint16_t>(out, static_cast<std::uint16_t>(ticket.candidate_id.size()));
    for (char c : ticket.instructor_id) {
        out.push_back(static_cast<std::byte>(c));
    }
    for (char c : ticket.package_id) {
        out.push_back(static_cast<std::byte>(c));
    }
    for (char c : ticket.candidate_id) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

std::optional<security::Sha256Digest> compute_ticket_signature(
    std::span<const std::byte> key,
    const InstructorAuthorizationTicket& ticket) {
    if (key.empty()) return std::nullopt;
    const auto transcript = compute_ticket_canonical_transcript(ticket);
    return security::hmac_sha256(key, transcript);
}

bool sign_ticket(
    std::span<const std::byte> key,
    InstructorAuthorizationTicket& ticket) {
    const auto sig = compute_ticket_signature(key, ticket);
    if (!sig) return false;
    ticket.instructor_signature = *sig;
    return true;
}

bool verify_ticket_signature(
    std::span<const std::byte> key,
    const InstructorAuthorizationTicket& ticket) noexcept {
    if (key.empty()) return false;
    const auto expected = compute_ticket_signature(key, ticket);
    if (!expected) return false;
    return security::constant_time_equal(ticket.instructor_signature, *expected);
}

TicketVerificationResult verify_ticket_for_exam(
    std::span<const std::byte> key,
    const InstructorAuthorizationTicket& ticket,
    const ExamStartRequest& request,
    std::uint64_t current_time_ms) noexcept {
    if (!validate_ticket(ticket, 0)) {
        return TicketVerificationResult::invalid_structure;
    }
    if (current_time_ms > 0) {
        if (current_time_ms < ticket.valid_from_ms) {
            return TicketVerificationResult::not_yet_valid;
        }
        if (current_time_ms > ticket.valid_until_ms) {
            return TicketVerificationResult::expired;
        }
    }
    if (!verify_ticket_signature(key, ticket)) {
        return TicketVerificationResult::signature_mismatch;
    }
    if (ticket.package_id != request.package_id ||
        ticket.package_digest != request.package_digest) {
        return TicketVerificationResult::package_mismatch;
    }
    if (ticket.session_id != request.session_id) {
        return TicketVerificationResult::session_mismatch;
    }
    if (!ticket.candidate_id.empty() && ticket.candidate_id != request.candidate_id) {
        return TicketVerificationResult::candidate_mismatch;
    }
    return TicketVerificationResult::valid;
}

std::vector<std::byte> encode_instructor_ticket(
    const InstructorAuthorizationTicket& ticket) {
    if (!validate_ticket(ticket, 0) ||
        ticket.instructor_id.size() > UINT16_MAX ||
        ticket.package_id.size() > UINT16_MAX ||
        ticket.candidate_id.size() > UINT16_MAX) {
        return {};
    }
    const std::size_t total = kFixedHeaderBytes +
                              ticket.instructor_id.size() +
                              ticket.package_id.size() +
                              ticket.candidate_id.size();
    if (total > kMaximumTicketPayloadBytes) return {};

    std::vector<std::byte> output;
    output.reserve(total);
    append_le<std::uint32_t>(output, kTicketMagic);
    append_le<std::uint16_t>(output, kTicketVersion);
    append_le<std::uint16_t>(output, 0); // reserved
    append_le<std::uint64_t>(output, ticket.valid_from_ms);
    append_le<std::uint64_t>(output, ticket.valid_until_ms);
    output.insert(output.end(), ticket.package_digest.begin(), ticket.package_digest.end());
    output.insert(output.end(), ticket.session_id.begin(), ticket.session_id.end());
    output.insert(output.end(), ticket.instructor_signature.begin(), ticket.instructor_signature.end());
    append_le<std::uint16_t>(output, static_cast<std::uint16_t>(ticket.instructor_id.size()));
    append_le<std::uint16_t>(output, static_cast<std::uint16_t>(ticket.package_id.size()));
    append_le<std::uint16_t>(output, static_cast<std::uint16_t>(ticket.candidate_id.size()));
    for (char c : ticket.instructor_id) output.push_back(static_cast<std::byte>(c));
    for (char c : ticket.package_id) output.push_back(static_cast<std::byte>(c));
    for (char c : ticket.candidate_id) output.push_back(static_cast<std::byte>(c));
    return output;
}

std::optional<InstructorAuthorizationTicket> decode_instructor_ticket(
    std::span<const std::byte> wire) {
    if (wire.size() < kFixedHeaderBytes || wire.size() > kMaximumTicketPayloadBytes) {
        return std::nullopt;
    }
    std::size_t offset = 0;
    const auto magic = read_le<std::uint32_t>(wire, offset);
    offset += 4;
    const auto version = read_le<std::uint16_t>(wire, offset);
    offset += 2;
    const auto reserved = read_le<std::uint16_t>(wire, offset);
    offset += 2;
    if (magic != kTicketMagic || version != kTicketVersion || reserved != 0) {
        return std::nullopt;
    }
    InstructorAuthorizationTicket ticket;
    ticket.valid_from_ms = read_le<std::uint64_t>(wire, offset);
    offset += 8;
    ticket.valid_until_ms = read_le<std::uint64_t>(wire, offset);
    offset += 8;
    std::memcpy(ticket.package_digest.data(), wire.data() + offset, ticket.package_digest.size());
    offset += ticket.package_digest.size();
    std::memcpy(ticket.session_id.data(), wire.data() + offset, ticket.session_id.size());
    offset += ticket.session_id.size();
    std::memcpy(ticket.instructor_signature.data(), wire.data() + offset, ticket.instructor_signature.size());
    offset += ticket.instructor_signature.size();

    const auto instructor_len = read_le<std::uint16_t>(wire, offset);
    offset += 2;
    const auto package_len = read_le<std::uint16_t>(wire, offset);
    offset += 2;
    const auto candidate_len = read_le<std::uint16_t>(wire, offset);
    offset += 2;

    if (offset + instructor_len + package_len + candidate_len != wire.size()) {
        return std::nullopt;
    }
    ticket.instructor_id.assign(reinterpret_cast<const char*>(wire.data() + offset), instructor_len);
    offset += instructor_len;
    ticket.package_id.assign(reinterpret_cast<const char*>(wire.data() + offset), package_len);
    offset += package_len;
    ticket.candidate_id.assign(reinterpret_cast<const char*>(wire.data() + offset), candidate_len);

    if (!validate_ticket(ticket, 0)) {
        return std::nullopt;
    }
    return ticket;
}

}  // namespace nstu::exam::auth
