#include "nstu/contest_package.hpp"

#include <algorithm>
#include <utility>

namespace nstu::contest_package {
namespace {
bool nonzero(const security::Sha256Digest& digest) noexcept {
    return std::any_of(digest.begin(), digest.end(), [](std::byte value) { return value != std::byte{0}; });
}
bool valid_id(std::string_view id) noexcept {
    const auto alnum = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    };
    return !id.empty() && id.size() <= 96 && alnum(id.front()) &&
        std::all_of(id.begin(), id.end(), [&](char c) { return alnum(c) || c == '.' || c == '-'; });
}
bool valid_offer(const PackageOffer& offer) noexcept {
    return valid_id(offer.public_package_id) && digest_from_hex(offer.content_digest) &&
        digest_from_hex(offer.archive_digest) && offer.archive_byte_count > 0 &&
        offer.archive_byte_count <= kMaximumArchiveBytes;
}
void append_count(std::vector<std::byte>& wire, std::uint64_t count) {
    for (std::size_t i = 0; i < 8; ++i) { wire.push_back(static_cast<std::byte>(count & 0xffu)); count >>= 8u; }
}
std::uint64_t read_count(std::span<const std::byte> wire) noexcept {
    std::uint64_t result = 0;
    for (std::size_t i = 0; i < 8; ++i) result |= static_cast<std::uint64_t>(std::to_integer<unsigned int>(wire[32 + i])) << (8u * i);
    return result;
}
std::string hex(const security::Sha256Digest& digest) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result; result.reserve(64);
    for (const auto byte : digest) {
        const auto value = std::to_integer<unsigned int>(byte);
        result.push_back(digits[value >> 4u]); result.push_back(digits[value & 15u]);
    }
    return result;
}
bool boundary(std::uint64_t offset, std::uint64_t total) noexcept {
    return offset <= total && (offset == total || offset % kChunkDataBytes == 0);
}
template<typename Duration>
bool elapsed(TimePoint now, TimePoint then, Duration budget) noexcept {
    return then <= TimePoint::max() - budget && now >= then + budget;
}
bool usable(TransferStatus status) noexcept {
    return status == TransferStatus::active || status == TransferStatus::complete || status == TransferStatus::prepared;
}
}

std::optional<security::Sha256Digest> digest_from_hex(std::string_view text) noexcept {
    if (text.size() != 64) return std::nullopt;
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    security::Sha256Digest result{};
    for (std::size_t i = 0; i < result.size(); ++i) {
        const auto high = nibble(text[2 * i]), low = nibble(text[2 * i + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        result[i] = static_cast<std::byte>((high << 4) | low);
    }
    return nonzero(result) ? std::optional(result) : std::nullopt;
}
std::vector<std::byte> encode_offer(const PackageOffer& offer) {
    if (!valid_offer(offer)) return {};
    return encode_ack({*digest_from_hex(offer.archive_digest), offer.archive_byte_count});
}
std::optional<PackageOffer> decode_offer(std::span<const std::byte> wire,
    std::string_view trusted_public_package_id, std::string_view trusted_content_digest) {
    const auto ack = decode_ack(wire);
    if (!ack || ack->next_offset == 0 || !valid_id(trusted_public_package_id) || !digest_from_hex(trusted_content_digest)) return std::nullopt;
    return PackageOffer{std::string(trusted_public_package_id), std::string(trusted_content_digest), hex(ack->digest), ack->next_offset};
}
std::vector<std::byte> encode_ack(const PackageAck& ack) {
    if (!nonzero(ack.digest) || ack.next_offset > kMaximumArchiveBytes) return {};
    std::vector<std::byte> wire(ack.digest.begin(), ack.digest.end());
    append_count(wire, ack.next_offset); return wire;
}
std::optional<PackageAck> decode_ack(std::span<const std::byte> wire) {
    if (wire.size() != 40) return std::nullopt;
    PackageAck result{}; std::copy_n(wire.begin(), result.digest.size(), result.digest.begin());
    result.next_offset = read_count(wire);
    if (!nonzero(result.digest) || result.next_offset > kMaximumArchiveBytes) return std::nullopt;
    return result;
}
std::vector<std::byte> encode_prepared(const Prepared& prepared) {
    if (!valid_offer(prepared.offer)) return {};
    const auto digest = digest_from_hex(prepared.offer.content_digest);
    return {digest->begin(), digest->end()};
}
std::optional<Prepared> decode_prepared(std::span<const std::byte> wire, const PackageOffer& expected) {
    if (!valid_offer(expected) || wire.size() != security::kSha256Bytes) return std::nullopt;
    const auto digest = digest_from_hex(expected.content_digest);
    if (!security::constant_time_equal(wire, *digest)) return std::nullopt;
    return Prepared{expected};
}

SenderWindow::SenderWindow(PackageOffer offer, TimePoint started)
    : offer_(std::move(offer)), started_(started), last_(started) {
    if (valid_offer(offer_)) { archive_digest_ = *digest_from_hex(offer_.archive_digest); status_ = TransferStatus::active; }
}
bool SenderWindow::advance(TimePoint now) noexcept {
    if (now < last_) return false;
    last_ = now;
    if (status_ != TransferStatus::invalid && status_ != TransferStatus::cancelled && status_ != TransferStatus::prepared) {
        if (elapsed(now, started_, kTransferBudget)) { status_ = TransferStatus::expired; pending_.clear(); }
        else if (status_ == TransferStatus::active && !pending_.empty() && elapsed(now, pending_.front().sent, kAckTimeout)) status_ = TransferStatus::paused;
    }
    return true;
}
TransferStatus SenderWindow::status(TimePoint now) noexcept {
    return advance(now) ? status_ : TransferStatus::invalid;
}
std::optional<PackageChunk> SenderWindow::next_chunk(std::span<const std::byte> data, TimePoint now) {
    if (!advance(now) || status_ != TransferStatus::active || pending_.size() >= kMaximumUnacknowledgedChunks || sent_ >= offer_.archive_byte_count) return std::nullopt;
    const auto count = std::min<std::uint64_t>(kChunkDataBytes, offer_.archive_byte_count - sent_);
    if (data.size() != count) return std::nullopt;
    PackageChunk result{archive_digest_, sent_, offer_.archive_byte_count, {data.begin(), data.end()}};
    pending_.push_back({sent_ + count, now});
    sent_ += count; high_water_ = std::max(high_water_, sent_);
    return result;
}
bool SenderWindow::acknowledge(const PackageAck& ack, TimePoint now) noexcept {
    if (!advance(now) || !usable(status_) || ack.digest != archive_digest_ ||
        !boundary(ack.next_offset, offer_.archive_byte_count) || ack.next_offset > sent_) return false;
    if (ack.next_offset <= acknowledged_) return true;
    if (status_ != TransferStatus::active || pending_.empty()) return false;
    while (!pending_.empty() && pending_.front().end <= ack.next_offset) pending_.pop_front();
    acknowledged_ = ack.next_offset;
    if (acknowledged_ == offer_.archive_byte_count) status_ = TransferStatus::complete;
    return true;
}
bool SenderWindow::accept_prepared(const Prepared& prepared, TimePoint now) noexcept {
    if (!advance(now) || (status_ != TransferStatus::complete && status_ != TransferStatus::prepared) ||
        prepared.offer != offer_ || acknowledged_ != offer_.archive_byte_count || !pending_.empty()) return false;
    status_ = TransferStatus::prepared; return true;
}
bool SenderWindow::pause(TimePoint now) noexcept {
    if (!advance(now) || (status_ != TransferStatus::active && status_ != TransferStatus::complete && status_ != TransferStatus::paused)) return false;
    status_ = TransferStatus::paused; return true;
}
bool SenderWindow::resume(const PackageOffer& offer, std::uint64_t receiver_offset, TimePoint now) noexcept {
    if (!advance(now) || offer != offer_ || (status_ != TransferStatus::active && status_ != TransferStatus::paused && status_ != TransferStatus::complete) ||
        !boundary(receiver_offset, offer_.archive_byte_count) || receiver_offset > high_water_) return false;
    pending_.clear(); sent_ = acknowledged_ = receiver_offset;
    status_ = receiver_offset == offer_.archive_byte_count ? TransferStatus::complete : TransferStatus::active;
    return true;
}
void SenderWindow::cancel() noexcept { status_ = TransferStatus::cancelled; pending_.clear(); }

ReceiverTracker::ReceiverTracker(PackageOffer offer, TimePoint started)
    : offer_(std::move(offer)), started_(started), last_(started) {
    if (valid_offer(offer_)) { archive_digest_ = *digest_from_hex(offer_.archive_digest); status_ = TransferStatus::active; }
}
bool ReceiverTracker::advance(TimePoint now) noexcept {
    if (now < last_) return false;
    last_ = now;
    if (status_ != TransferStatus::invalid && status_ != TransferStatus::cancelled && status_ != TransferStatus::prepared && elapsed(now, started_, kTransferBudget)) status_ = TransferStatus::expired;
    return true;
}
TransferStatus ReceiverTracker::status(TimePoint now) noexcept {
    return advance(now) ? status_ : TransferStatus::invalid;
}
ReceiveResult ReceiverTracker::inspect(const PackageChunk& chunk, TimePoint now) {
    if (!advance(now) || !usable(status_) || chunk.digest != archive_digest_ || chunk.total_bytes != offer_.archive_byte_count ||
        chunk.offset >= offer_.archive_byte_count || chunk.offset % kChunkDataBytes != 0 ||
        chunk.data.size() != std::min<std::uint64_t>(kChunkDataBytes, offer_.archive_byte_count - chunk.offset) || chunk.offset > received_) return ReceiveResult::rejected;
    if (chunk.offset < received_) {
        const auto fingerprint = security::sha256(chunk.data);
        const auto index = static_cast<std::size_t>(chunk.offset / kChunkDataBytes);
        return fingerprint && index < fingerprints_.size() && *fingerprint == fingerprints_[index] ? ReceiveResult::duplicate : ReceiveResult::rejected;
    }
    return status_ == TransferStatus::active ? ReceiveResult::accepted : ReceiveResult::rejected;
}
ReceiveResult ReceiverTracker::accept(const PackageChunk& chunk, TimePoint now) {
    const auto checked = inspect(chunk, now);
    if (checked != ReceiveResult::accepted) return checked;
    const auto fingerprint = security::sha256(chunk.data); if (!fingerprint) return ReceiveResult::rejected;
    fingerprints_.push_back(*fingerprint); received_ += chunk.data.size();
    if (received_ == offer_.archive_byte_count) status_ = TransferStatus::complete;
    return ReceiveResult::accepted;
}
std::optional<PackageAck> ReceiverTracker::acknowledgment(TimePoint now) noexcept {
    if (!advance(now) || !usable(status_)) return std::nullopt;
    return PackageAck{archive_digest_, received_};
}
std::optional<Prepared> ReceiverTracker::prepared(std::string_view computed_content_digest,
    std::string_view computed_archive_digest, TimePoint now) {
    if (!advance(now) || (status_ != TransferStatus::complete && status_ != TransferStatus::prepared) || received_ != offer_.archive_byte_count ||
        computed_content_digest != offer_.content_digest || computed_archive_digest != offer_.archive_digest) return std::nullopt;
    status_ = TransferStatus::prepared; return Prepared{offer_};
}
bool ReceiverTracker::pause(TimePoint now) noexcept {
    if (!advance(now) || (status_ != TransferStatus::active && status_ != TransferStatus::complete && status_ != TransferStatus::paused)) return false;
    status_ = TransferStatus::paused; return true;
}
bool ReceiverTracker::resume(const PackageOffer& offer, TimePoint now) noexcept {
    if (!advance(now) || offer != offer_ || (status_ != TransferStatus::active && status_ != TransferStatus::paused && status_ != TransferStatus::complete)) return false;
    status_ = received_ == offer_.archive_byte_count ? TransferStatus::complete : TransferStatus::active; return true;
}
void ReceiverTracker::cancel() noexcept { status_ = TransferStatus::cancelled; fingerprints_.clear(); }

} // namespace nstu::contest_package
