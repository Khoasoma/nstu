#include "nstu/frame_reassembler.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>

namespace {

nstu::protocol::VideoPacketHeader header(std::uint64_t frame,
                                         std::uint16_t fragment,
                                         std::uint16_t count,
                                         std::size_t payload_bytes) {
    const bool last = fragment + 1 == count;
    return {
        .version = nstu::protocol::kVideoVersion,
        .flags = last ? nstu::protocol::VideoFlags::end_of_frame
                      : nstu::protocol::VideoFlags::none,
        .stream_id = 7,
        .packet_sequence = frame * 10 + fragment,
        .frame_id = frame,
        .fragment_index = fragment,
        .fragment_count = count,
        .payload_bytes = static_cast<std::uint32_t>(payload_bytes),
        .capture_time_100ns = frame * 100,
    };
}

} // namespace

int main() {
    using namespace std::chrono_literals;
    using nstu::video::FragmentDisposition;
    const auto start = std::chrono::steady_clock::now();

    nstu::video::FrameReassembler reassembler;
    const std::array<std::byte, 2> first{std::byte{0x61}, std::byte{0x62}};
    const std::array<std::byte, 1> middle{std::byte{0x63}};
    const std::array<std::byte, 2> last{std::byte{0x64}, std::byte{0x65}};
    auto result = reassembler.push(header(1, 2, 3, last.size()), last, start);
    assert(result.disposition == FragmentDisposition::uninitialized);
    reassembler.reset(7);
    result = reassembler.push(header(1, 2, 3, last.size()), last, start);
    assert(result.disposition == FragmentDisposition::accepted);
    result = reassembler.push(header(1, 0, 3, first.size()), first, start + 1ms);
    assert(result.disposition == FragmentDisposition::accepted);
    const auto missing = reassembler.missing_fragments(1);
    assert(missing.has_value());
    assert(missing->size() == 1 && (*missing)[0] == 1);
    result = reassembler.push(header(1, 0, 3, first.size()), first, start + 2ms);
    assert(result.disposition == FragmentDisposition::duplicate);
    result = reassembler.push(header(1, 1, 3, middle.size()), middle, start + 3ms);
    assert(result.disposition == FragmentDisposition::frame_complete);
    assert(result.frame.has_value());
    const std::array<std::byte, 5> expected{
        std::byte{0x61}, std::byte{0x62}, std::byte{0x63}, std::byte{0x64},
        std::byte{0x65}};
    assert(result.frame->payload.size() == expected.size());
    assert(std::equal(result.frame->payload.begin(), result.frame->payload.end(),
                      expected.begin()));
    result = reassembler.push(header(1, 1, 3, middle.size()), middle, start + 4ms);
    assert(result.disposition == FragmentDisposition::late_completed_frame);

    auto conflicting = header(2, 0, 2, first.size());
    assert(reassembler.push(conflicting, first, start + 5ms).disposition ==
           FragmentDisposition::accepted);
    auto different = first;
    different[0] ^= std::byte{1};
    assert(reassembler.push(conflicting, different, start + 6ms).disposition ==
           FragmentDisposition::invalid);

    nstu::video::ReassemblyLimits deadline_limits;
    deadline_limits.frame_deadline = 50ms;
    nstu::video::FrameReassembler expiring(deadline_limits);
    expiring.reset(7);
    assert(expiring.push(header(3, 0, 3, first.size()), first, start).disposition ==
           FragmentDisposition::accepted);
    assert(expiring.push(header(3, 1, 3, middle.size()), middle, start + 40ms)
               .disposition == FragmentDisposition::accepted);
    assert(expiring.expire(start + 51ms) == 1);
    assert(expiring.stats().expired_incomplete_frames == 1);
    assert(expiring.stats().buffered_bytes == 0);

    nstu::video::ReassemblyLimits resource_limits;
    resource_limits.max_frames_in_flight = 1;
    nstu::video::FrameReassembler limited(resource_limits);
    limited.reset(7);
    assert(limited.push(header(4, 0, 2, first.size()), first, start).disposition ==
           FragmentDisposition::accepted);
    assert(limited.push(header(5, 0, 2, first.size()), first, start).disposition ==
           FragmentDisposition::resource_limit);

    auto invalid_end = header(6, 0, 2, first.size());
    invalid_end.flags = nstu::protocol::VideoFlags::end_of_frame;
    assert(reassembler.push(invalid_end, first, start).disposition ==
           FragmentDisposition::invalid);
    auto wrong_stream = header(7, 0, 2, first.size());
    wrong_stream.stream_id = 99;
    assert(reassembler.push(wrong_stream, first, start).disposition ==
           FragmentDisposition::wrong_stream);
    return 0;
}
