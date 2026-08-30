#include "nstu/packet_loss.hpp"

#include <cassert>
#include <algorithm>
#include <vector>

namespace {

nstu::protocol::VideoPacketHeader packet(std::uint32_t stream,
                                         std::uint64_t sequence) {
    return {
        .version = nstu::protocol::kVideoVersion,
        .stream_id = stream,
        .packet_sequence = sequence,
        .fragment_count = 1,
    };
}

} // namespace

int main() {
    using nstu::net::PacketDisposition;
    using nstu::net::PacketLossTracker;

    PacketLossTracker reordered(8, 1000);
    assert(reordered.observe(packet(7, 100)) == PacketDisposition::in_order);
    assert(reordered.observe(packet(7, 102)) == PacketDisposition::in_order);
    assert(reordered.stats().confirmed_lost == 0);
    assert(reordered.observe(packet(7, 101)) == PacketDisposition::reordered);
    assert(reordered.observe(packet(7, 101)) == PacketDisposition::duplicate);
    assert(reordered.stats().confirmed_lost == 0);
    assert(reordered.stats().reordered == 1);
    assert(reordered.stats().duplicates == 1);

    PacketLossTracker startup_reorder(8, 1000);
    startup_reorder.reset(8, 100);
    assert(startup_reorder.observe(packet(8, 102)) ==
           PacketDisposition::in_order);
    assert(startup_reorder.observe(packet(8, 100)) ==
           PacketDisposition::reordered);
    assert(startup_reorder.observe(packet(8, 101)) ==
           PacketDisposition::reordered);
    startup_reorder.flush();
    assert(startup_reorder.stats().confirmed_lost == 0);
    assert(startup_reorder.stats().finalized_received == 3);

    PacketLossTracker missing(8, 1000);
    assert(missing.observe(packet(9, 10)) == PacketDisposition::in_order);
    assert(missing.observe(packet(9, 12)) == PacketDisposition::in_order);
    for (std::uint64_t sequence = 13; sequence <= 19; ++sequence) {
        assert(missing.observe(packet(9, sequence)) ==
               PacketDisposition::in_order);
    }
    assert(missing.stats().confirmed_lost == 1);
    assert(missing.observe(packet(9, 11)) == PacketDisposition::too_late);
    assert(missing.stats().confirmed_lost == 1);
    assert(missing.stats().too_late == 1);

    PacketLossTracker large_gap(8, 1000);
    assert(large_gap.observe(packet(11, 0)) == PacketDisposition::in_order);
    assert(large_gap.observe(packet(11, 100)) == PacketDisposition::in_order);
    assert(large_gap.stats().finalized_received == 1);
    assert(large_gap.stats().confirmed_lost == 92);

    PacketLossTracker invalid_jump(8, 100);
    assert(invalid_jump.observe(packet(12, 1000)) == PacketDisposition::in_order);
    assert(invalid_jump.observe(packet(12, 5000)) ==
           PacketDisposition::invalid_forward_jump);
    assert(invalid_jump.stats().confirmed_lost == 0);
    assert(invalid_jump.stats().invalid_forward_jumps == 1);
    assert(invalid_jump.observe(packet(13, 1001)) ==
           PacketDisposition::wrong_stream);

    PacketLossTracker flushed(8, 1000);
    assert(flushed.observe(packet(14, 20)) == PacketDisposition::in_order);
    assert(flushed.observe(packet(14, 22)) == PacketDisposition::in_order);
    flushed.flush();
    assert(flushed.stats().finalized_received == 2);
    assert(flushed.stats().confirmed_lost == 1);
    assert(flushed.stats().finalized_sample_size() == 3);
    assert(flushed.stats().loss_per_mille() == 333);

    const auto before = flushed.stats();
    assert(flushed.observe(packet(14, 23)) == PacketDisposition::in_order);
    flushed.flush();
    const auto delta = nstu::net::packet_loss_delta(flushed.stats(), before);
    assert(delta.has_value());
    assert(delta->finalized_received == 1);
    assert(delta->confirmed_lost == 0);
    const auto pre_reset = flushed.stats();
    flushed.reset(14, 1000);
    assert(!nstu::net::packet_loss_delta(flushed.stats(), pre_reset).has_value());

    PacketLossTracker patterned(32, 1000);
    patterned.reset(21, 0);
    std::vector<std::uint64_t> sequences;
    for (std::uint64_t sequence = 0; sequence < 1000; ++sequence) {
        if (sequence == 0 || sequence % 10 != 0) {
            sequences.push_back(sequence);
        }
    }
    for (std::size_t begin = 0; begin < sequences.size(); begin += 16) {
        const auto end = std::min(begin + 16, sequences.size());
        std::reverse(sequences.begin() + static_cast<std::ptrdiff_t>(begin),
                     sequences.begin() + static_cast<std::ptrdiff_t>(end));
        for (std::size_t index = begin; index < end; ++index) {
            const auto disposition = patterned.observe(packet(21, sequences[index]));
            assert(disposition == PacketDisposition::in_order ||
                   disposition == PacketDisposition::reordered);
        }
    }
    patterned.flush();
    assert(patterned.stats().unique_received == 901);
    assert(patterned.stats().finalized_received == 901);
    assert(patterned.stats().confirmed_lost == 99);
    assert(patterned.stats().loss_per_mille() == 99);
    return 0;
}
