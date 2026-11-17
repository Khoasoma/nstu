#include "nstu/contest_package.hpp"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <limits>

namespace cp = nstu::contest_package;
using namespace std::chrono_literals;

namespace {
const cp::TimePoint start{};
cp::PackageOffer offer(std::uint64_t size = 4 * 32768 + 7) {
    return {"public-package_1", std::string(64, '1'), std::string(64, '2'), size};
}
std::vector<std::byte> data(std::size_t size = 32768, unsigned char value = 3) {
    return std::vector<std::byte>(size, static_cast<std::byte>(value));
}
cp::PackageChunk chunk(const cp::PackageOffer& pin, std::uint64_t offset) {
    return {*cp::digest_from_hex(pin.archive_digest), offset, pin.archive_byte_count,
            data(static_cast<std::size_t>(std::min<std::uint64_t>(32768, pin.archive_byte_count - offset)))};
}
cp::PackageAck ack(const cp::PackageOffer& pin, std::uint64_t offset) {
    return {*cp::digest_from_hex(pin.archive_digest), offset};
}

void codec_tests() {
    auto pin = offer();
    auto wire = cp::encode_offer(pin);
    assert(wire.size() == 40);
    assert(wire[0] == std::byte{0x22});
    assert(wire[32] == std::byte{7} && wire[34] == std::byte{2});
    assert(cp::decode_offer(wire, pin.public_package_id, pin.content_digest) == pin);
    for (std::size_t i = 0; i < wire.size(); ++i) assert(!cp::decode_offer(std::span(wire).first(i), pin.public_package_id, pin.content_digest));
    wire.push_back(std::byte{0}); assert(!cp::decode_offer(wire, pin.public_package_id, pin.content_digest));
    for (const auto& id : std::vector<std::string>{"", ".x", "-x", "a/b", "a\\b", "a b", std::string(97, 'a')}) {
        auto invalid = pin; invalid.public_package_id = id; assert(cp::encode_offer(invalid).empty());
        assert(!cp::decode_offer(cp::encode_offer(pin), id, pin.content_digest));
    }
    pin.public_package_id = std::string(96, 'a'); assert(cp::decode_offer(cp::encode_offer(pin), pin.public_package_id, pin.content_digest) == pin);
    for (const auto& digest : std::vector<std::string>{"", std::string(63, '1'), std::string(65, '1'), std::string(64, '0'), std::string(64, 'g'), std::string(64, 'A')}) {
        auto invalid = pin; invalid.archive_digest = digest; assert(cp::encode_offer(invalid).empty());
        invalid = pin; invalid.content_digest = digest; assert(cp::encode_offer(invalid).empty());
        assert(!cp::decode_offer(cp::encode_offer(pin), pin.public_package_id, digest));
    }
    for (const auto count : {std::uint64_t{0}, cp::kMaximumArchiveBytes + 1, std::numeric_limits<std::uint64_t>::max()}) {
        auto invalid = pin; invalid.archive_byte_count = count; assert(cp::encode_offer(invalid).empty());
    }
    assert(!cp::encode_offer(offer(cp::kMaximumArchiveBytes)).empty());
    const auto piece = chunk(pin, 0);
    assert(cp::encode_chunk(piece) == nstu::contest_room::encode_chunk(piece));
    assert(cp::decode_chunk(cp::encode_chunk(piece))->data == piece.data);
    auto bad = piece; bad.data.clear(); assert(cp::encode_chunk(bad).empty());
    bad = piece; bad.data = data(32769); assert(cp::encode_chunk(bad).empty());
    bad = piece; bad.offset = std::numeric_limits<std::uint64_t>::max(); assert(cp::encode_chunk(bad).empty());
    bad = piece; bad.total_bytes = cp::kMaximumArchiveBytes + 1; assert(cp::encode_chunk(bad).empty());
    auto receipt = ack(pin, 32768); const auto receipt_wire = cp::encode_ack(receipt);
    assert(receipt_wire.size() == 40 && receipt_wire[32] == std::byte{0} && receipt_wire[33] == std::byte{0x80});
    assert(cp::decode_ack(receipt_wire) == receipt);
    for (std::size_t i = 0; i < receipt_wire.size(); ++i) assert(!cp::decode_ack(std::span(receipt_wire).first(i)));
    auto trailing = receipt_wire; trailing.push_back(std::byte{0}); assert(!cp::decode_ack(trailing));
    receipt.digest.fill(std::byte{0}); assert(cp::encode_ack(receipt).empty());
    receipt = ack(pin, cp::kMaximumArchiveBytes + 1); assert(cp::encode_ack(receipt).empty());
    const auto prepared_wire = cp::encode_prepared({pin});
    assert(prepared_wire.size() == 32 && prepared_wire.front() == std::byte{0x11});
    assert(cp::decode_prepared(prepared_wire, pin) == cp::Prepared{pin});
    auto wrong = pin; wrong.content_digest[0] = '3'; assert(!cp::decode_prepared(prepared_wire, wrong));
    for (std::size_t i = 0; i < prepared_wire.size(); ++i) assert(!cp::decode_prepared(std::span(prepared_wire).first(i), pin));
    trailing = prepared_wire; trailing.push_back(std::byte{0}); assert(!cp::decode_prepared(trailing, pin));
    const nstu::contest_room::Realm realm{1, "teacher", std::string(32, '1'), std::string(32, '2')};
    assert(!nstu::contest_room::encode_message(realm, nstu::contest_room::Side::teacher,
        {nstu::contest_room::Kind::package_offer, 1, cp::encode_offer(pin)}).empty());
    assert(!nstu::contest_room::encode_message(realm, nstu::contest_room::Side::candidate,
        {nstu::contest_room::Kind::prepared, 1, prepared_wire}).empty());
    std::cout << "codecs: exact wire, strict IDs/pins, truncation and archive/chunk bounds passed\n";
}

void sender_tests() {
    const auto pin = offer(); cp::SenderWindow sender(pin, start);
    assert(sender.status(start) == cp::TransferStatus::active);
    assert(!sender.accept_prepared({pin}, start));
    assert(!sender.next_chunk(data(32767), start));
    for (std::uint64_t i = 0; i < 4; ++i) {
        const auto piece = sender.next_chunk(data(), start + 1s);
        assert(piece && piece->offset == i * 32768);
    }
    assert(sender.unacknowledged_chunks() == 4);
    assert(!sender.next_chunk(data(7), start + 1s));
    assert(!sender.acknowledge(ack(pin, 1), start + 2s));
    assert(!sender.acknowledge(ack(pin, pin.archive_byte_count), start + 2s));
    auto foreign = ack(pin, 32768); foreign.digest[0] = std::byte{1};
    assert(!sender.acknowledge(foreign, start + 2s));
    assert(sender.acknowledge(ack(pin, 65536), start + 2s));
    assert(sender.acknowledge(ack(pin, 32768), start + 3s));
    assert(sender.unacknowledged_chunks() == 2);
    assert(sender.next_chunk(data(7), start + 3s));
    assert(!sender.accept_prepared({pin}, start + 3s));
    assert(sender.acknowledge(ack(pin, pin.archive_byte_count), start + 4s));
    assert(sender.status(start + 4s) == cp::TransferStatus::complete);
    auto stale = pin; stale.content_digest[0] = '3';
    assert(!sender.accept_prepared({stale}, start + 4s));
    auto stale_id = pin; stale_id.public_package_id = "other";
    auto stale_archive = pin; stale_archive.archive_digest[0] = '3';
    auto stale_size = pin; ++stale_size.archive_byte_count;
    for (const auto& changed : {stale_id, stale_archive, stale_size}) {
        assert(!sender.accept_prepared({changed}, start + 4s));
        assert(!sender.resume(changed, pin.archive_byte_count, start + 4s));
    }
    assert(sender.accept_prepared({pin}, start + 4s));
    assert(sender.accept_prepared({pin}, start + 5s));
    assert(sender.status(start + 5s) == cp::TransferStatus::prepared);
    assert(!sender.next_chunk(data(), start + 5s));

    cp::SenderWindow slow(pin, start), independent(pin, start);
    assert(slow.next_chunk(data(), start));
    assert(slow.next_chunk(data(), start + 1s));
    assert(slow.status(start + 9999ms) == cp::TransferStatus::active);
    assert(slow.status(start + 10s) == cp::TransferStatus::paused);
    assert(!slow.acknowledge(ack(pin, 32768), start + 10s));
    assert(independent.next_chunk(data(), start + 10s));
    assert(independent.acknowledge(ack(pin, 32768), start + 11s));
    assert(!slow.resume(stale, 32768, start + 11s));
    assert(!slow.resume(pin, 1, start + 11s));
    assert(!slow.resume(pin, 98304, start + 11s));
    assert(slow.resume(pin, 32768, start + 11s));
    assert(slow.next_chunk(data(), start + 11s)->offset == 32768);
    assert(slow.pause(start + 12s));
    assert(slow.resume(pin, 0, start + 599s));
    assert(slow.status(start + 600s) == cp::TransferStatus::expired);
    assert(!slow.resume(pin, 0, start + 601s));
    cp::SenderWindow cancelled(pin, start); cancelled.cancel();
    assert(!cancelled.next_chunk(data(), start)); assert(!cancelled.resume(pin, 0, start));
    assert(cancelled.status(start) == cp::TransferStatus::cancelled);
    cp::SenderWindow invalid(offer(0), start); assert(invalid.status(start) == cp::TransferStatus::invalid);
    cp::SenderWindow backwards(pin, start); assert(!backwards.next_chunk(data(), start - 1ms));
    assert(backwards.next_chunk(data(), start + 2s)); assert(!backwards.acknowledge(ack(pin, 32768), start + 1s));
    cp::SenderWindow tiny(offer(1), start); assert(tiny.next_chunk(data(1), start));
    assert(tiny.acknowledge(ack(offer(1), 1), start)); assert(tiny.accept_prepared({offer(1)}, start));
    cp::SenderWindow max(offer(cp::kMaximumArchiveBytes), start);
    for (std::uint64_t offset = 0; offset < cp::kMaximumArchiveBytes; offset += 32768) {
        assert(max.next_chunk(data(), start)); assert(max.acknowledge(ack(offer(cp::kMaximumArchiveBytes), offset + 32768), start));
    }
    assert(max.accept_prepared({offer(cp::kMaximumArchiveBytes)}, start));
    std::cout << "sender: four-chunk window, cumulative/replayed ACK, timeout, fairness, resume/cancel and total budget passed\n";
}

void receiver_tests() {
    const auto pin = offer(32768 + 7); cp::ReceiverTracker receiver(pin, start);
    assert(!receiver.prepared(pin.content_digest, pin.archive_digest, start));
    assert(receiver.accept(chunk(pin, 32768), start) == cp::ReceiveResult::rejected);
    auto short_piece = chunk(pin, 0); short_piece.data.resize(7);
    assert(receiver.accept(short_piece, start) == cp::ReceiveResult::rejected);
    const auto first = chunk(pin, 0);
    assert(receiver.inspect(first, start) == cp::ReceiveResult::accepted);
    assert(receiver.received_bytes() == 0 && receiver.acknowledgment(start)->next_offset == 0);
    assert(receiver.inspect(short_piece, start) == cp::ReceiveResult::rejected);
    assert(receiver.accept(first, start + 1s) == cp::ReceiveResult::accepted);
    assert(receiver.acknowledgment(start + 1s)->next_offset == 32768);
    assert(receiver.accept(first, start + 2s) == cp::ReceiveResult::duplicate);
    auto conflict = first; conflict.data[0] = std::byte{4};
    assert(receiver.inspect(conflict, start + 3s) == cp::ReceiveResult::rejected);
    assert(receiver.received_bytes() == 32768);
    assert(receiver.accept(conflict, start + 3s) == cp::ReceiveResult::rejected);
    auto foreign = first; foreign.digest[0] = std::byte{4};
    assert(receiver.accept(foreign, start + 3s) == cp::ReceiveResult::rejected);
    foreign = first; ++foreign.total_bytes; assert(receiver.accept(foreign, start + 3s) == cp::ReceiveResult::rejected);
    assert(receiver.pause(start + 4s)); assert(receiver.accept(chunk(pin, 32768), start + 4s) == cp::ReceiveResult::rejected);
    auto stale = pin; stale.public_package_id = "another-package";
    assert(!receiver.resume(stale, start + 5s)); assert(receiver.resume(pin, start + 5s));
    assert(receiver.accept(chunk(pin, 32768), start + 6s) == cp::ReceiveResult::accepted);
    assert(receiver.status(start + 6s) == cp::TransferStatus::complete);
    assert(!receiver.prepared(std::string(64, '3'), pin.archive_digest, start + 6s));
    assert(!receiver.prepared(pin.content_digest, std::string(64, '3'), start + 6s));
    assert(receiver.prepared(pin.content_digest, pin.archive_digest, start + 6s) == cp::Prepared{pin});
    assert(receiver.status(start + 6s) == cp::TransferStatus::prepared);
    assert(receiver.inspect(first, start + 7s) == cp::ReceiveResult::duplicate);
    assert(receiver.accept(first, start + 7s) == cp::ReceiveResult::duplicate);
    cp::ReceiverTracker cancelled(pin, start); cancelled.cancel();
    assert(!cancelled.resume(pin, start)); assert(!cancelled.acknowledgment(start));
    assert(cancelled.inspect(first, start) == cp::ReceiveResult::rejected);
    assert(!cancelled.prepared(pin.content_digest, pin.archive_digest, start));
    cp::ReceiverTracker expired(pin, start); assert(expired.accept(first, start + 599s) == cp::ReceiveResult::accepted);
    assert(!expired.resume(pin, start + 600s)); assert(expired.status(start + 600s) == cp::TransferStatus::expired);
    cp::ReceiverTracker backwards(pin, start); assert(backwards.accept(first, start - 1ms) == cp::ReceiveResult::rejected);
    assert(backwards.accept(first, start + 2s) == cp::ReceiveResult::accepted);
    assert(backwards.accept(chunk(pin, 32768), start + 1s) == cp::ReceiveResult::rejected);
    assert(!cp::ReceiverTracker(offer(0), start).acknowledgment(start));
    cp::ReceiverTracker unpolled(pin, start);
    assert(unpolled.accept(first, start) == cp::ReceiveResult::accepted);
    assert(!unpolled.acknowledgment(start + 600s));
    cp::SenderWindow advancing(offer(3 * 32768), start);
    assert(advancing.next_chunk(data(), start)); assert(advancing.next_chunk(data(), start + 1s));
    assert(advancing.acknowledge(ack(offer(3 * 32768), 32768), start + 9s));
    assert(advancing.status(start + 10999ms) == cp::TransferStatus::active);
    assert(advancing.status(start + 11s) == cp::TransferStatus::paused);
    cp::SenderWindow near_max(pin, cp::TimePoint::max() - 1s);
    assert(near_max.next_chunk(data(), cp::TimePoint::max()));
    cp::SenderWindow extreme(pin, cp::TimePoint::min());
    assert(extreme.status(cp::TimePoint::max()) == cp::TransferStatus::expired);
    cp::ReceiverTracker maximum(offer(cp::kMaximumArchiveBytes), start);
    const auto maximum_pin = maximum.offer();
    for (std::uint64_t offset = 0; offset < cp::kMaximumArchiveBytes; offset += 32768) {
        assert(maximum.accept(chunk(maximum_pin, offset), start) == cp::ReceiveResult::accepted);
    }
    assert(maximum.received_bytes() == cp::kMaximumArchiveBytes);
    assert(maximum.accept(chunk(maximum_pin, 0), start) == cp::ReceiveResult::duplicate);
    assert(maximum.prepared(maximum_pin.content_digest, maximum_pin.archive_digest, start));
    std::cout << "receiver: ordering, real duplicate fingerprints, conflict/stale pins, partial/final preparation and cancellation passed\n";
}
}

int main() { codec_tests(); sender_tests(); receiver_tests(); }
