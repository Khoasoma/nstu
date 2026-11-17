#pragma once

#include "nstu/contest_room_protocol.hpp"

#include <deque>
#include <string_view>

namespace nstu::contest_package {

inline constexpr auto kMaximumArchiveBytes = contest_room::kMaximumArchiveBytes;
inline constexpr auto kChunkDataBytes = contest_room::kMaximumChunkDataBytes;
inline constexpr std::size_t kMaximumUnacknowledgedChunks = 4;
inline constexpr auto kAckTimeout = std::chrono::seconds(10);
inline constexpr auto kTransferBudget = std::chrono::minutes(10);
using TimePoint = std::chrono::steady_clock::time_point;

struct PackageOffer {
    std::string public_package_id;
    std::string content_digest;
    std::string archive_digest;
    std::uint64_t archive_byte_count = 0;
    [[nodiscard]] bool operator==(const PackageOffer&) const = default;
};
using PackageChunk = contest_room::PackageChunk;
using contest_room::encode_chunk;
using contest_room::decode_chunk;
struct PackageAck {
    security::Sha256Digest digest{};
    std::uint64_t next_offset = 0;
    [[nodiscard]] bool operator==(const PackageAck&) const = default;
};
struct Prepared {
    PackageOffer offer;
    [[nodiscard]] bool operator==(const Prepared&) const = default;
};

// IDs match [A-Za-z0-9_][A-Za-z0-9_.-]{0,95}; pins are nonzero lower-case hex[64].
[[nodiscard]] std::optional<security::Sha256Digest> digest_from_hex(std::string_view text) noexcept;
// Preserve 4a: offer=archive digest[32]+LE u64 archive bytes, ACK=archive
// digest[32]+LE u64 next offset, prepared=content digest[32]. ID/content pin
// come only from the authenticated issued context/teacher package pin.
[[nodiscard]] std::vector<std::byte> encode_offer(const PackageOffer& offer);
[[nodiscard]] std::optional<PackageOffer> decode_offer(std::span<const std::byte> wire,
    std::string_view trusted_public_package_id, std::string_view trusted_content_digest);
[[nodiscard]] std::vector<std::byte> encode_ack(const PackageAck& ack);
[[nodiscard]] std::optional<PackageAck> decode_ack(std::span<const std::byte> wire);
[[nodiscard]] std::vector<std::byte> encode_prepared(const Prepared& prepared);
[[nodiscard]] std::optional<Prepared> decode_prepared(std::span<const std::byte> wire,
    const PackageOffer& expected);

enum class TransferStatus { invalid, active, paused, complete, prepared, cancelled, expired };
enum class ReceiveResult { rejected, accepted, duplicate };

// One owner per member; consumers bind realm, authenticated request/transfer ID,
// member and five-field context. No socket/file I/O or authority is provided here.
class SenderWindow {
public:
    SenderWindow(PackageOffer offer, TimePoint started);
    [[nodiscard]] TransferStatus status(TimePoint now) noexcept;
    [[nodiscard]] std::optional<PackageChunk> next_chunk(std::span<const std::byte> data, TimePoint now);
    [[nodiscard]] bool acknowledge(const PackageAck& ack, TimePoint now) noexcept;
    [[nodiscard]] bool accept_prepared(const Prepared& prepared, TimePoint now) noexcept;
    [[nodiscard]] bool pause(TimePoint now) noexcept;
    [[nodiscard]] bool resume(const PackageOffer& offer, std::uint64_t receiver_offset, TimePoint now) noexcept;
    void cancel() noexcept;
    [[nodiscard]] std::size_t unacknowledged_chunks() const noexcept { return pending_.size(); }
    [[nodiscard]] std::uint64_t acknowledged_bytes() const noexcept { return acknowledged_; }
    [[nodiscard]] const PackageOffer& offer() const noexcept { return offer_; }
private:
    struct Pending { std::uint64_t end; TimePoint sent; };
    [[nodiscard]] bool advance(TimePoint now) noexcept;
    PackageOffer offer_;
    security::Sha256Digest archive_digest_{};
    TimePoint started_, last_;
    TransferStatus status_ = TransferStatus::invalid;
    std::deque<Pending> pending_;
    std::uint64_t sent_ = 0, high_water_ = 0, acknowledged_ = 0;
};

// Inspect before I/O: accepted means new contiguous bytes; duplicate/rejected
// must never be written. Commit with accept only after a successful new write.
// Counts/fingerprints do not prove flush, durability or complete archive validity.
class ReceiverTracker {
public:
    ReceiverTracker(PackageOffer offer, TimePoint started);
    [[nodiscard]] TransferStatus status(TimePoint now) noexcept;
    [[nodiscard]] ReceiveResult inspect(const PackageChunk& chunk, TimePoint now);
    [[nodiscard]] ReceiveResult accept(const PackageChunk& chunk, TimePoint now);
    [[nodiscard]] std::optional<PackageAck> acknowledgment(TimePoint now) noexcept;
    // Consumer must compute these pins from the complete archive and validated
    // manifest/content and finish native extraction/host validation before send.
    [[nodiscard]] std::optional<Prepared> prepared(std::string_view computed_content_digest,
        std::string_view computed_archive_digest, TimePoint now);
    [[nodiscard]] bool pause(TimePoint now) noexcept;
    [[nodiscard]] bool resume(const PackageOffer& offer, TimePoint now) noexcept;
    void cancel() noexcept;
    [[nodiscard]] std::uint64_t received_bytes() const noexcept { return received_; }
    [[nodiscard]] const PackageOffer& offer() const noexcept { return offer_; }
private:
    [[nodiscard]] bool advance(TimePoint now) noexcept;
    PackageOffer offer_;
    security::Sha256Digest archive_digest_{};
    TimePoint started_, last_;
    TransferStatus status_ = TransferStatus::invalid;
    std::uint64_t received_ = 0;
    std::vector<security::Sha256Digest> fingerprints_;
};

} // namespace nstu::contest_package
