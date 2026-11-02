#include "nstu/uwf_overlay_report.hpp"

#include <cassert>

int main() {
    using namespace nstu::uwf;
    const OverlayReport report{true, OverlayType::disk, 4096u, 128u, 3968u,
                               3072u, 3584u};
    const auto wire = encode_overlay_report(report);
    assert(wire.size() == kOverlayReportBytes);
    assert(decode_overlay_report(wire) == report);
    assert(decode_overlay_report(encode_overlay_report({})) == OverlayReport{});
    for (std::size_t length = 0u; length < wire.size(); ++length) {
        assert(!decode_overlay_report(std::span{wire}.first(length)));
    }
    auto modified = wire;
    modified[0] = std::byte{2};
    assert(!decode_overlay_report(modified));
    modified = wire;
    modified[1] = std::byte{2};
    assert(!decode_overlay_report(modified));
    modified = wire;
    modified[2] = std::byte{2};
    assert(!decode_overlay_report(modified));
    modified = wire;
    modified.push_back(std::byte{0});
    assert(!decode_overlay_report(modified));
    modified = wire;
    modified[3] = modified[4] = modified[5] = modified[6] = std::byte{0};
    assert(!decode_overlay_report(modified));
    modified = wire;
    modified[6] = std::byte{0x80}; // MaximumSize exceeds signed WMI range.
    assert(!decode_overlay_report(modified));
    modified = wire;
    modified[15] = modified[16] = modified[17] = modified[18] = std::byte{0xff};
    assert(!decode_overlay_report(modified));
    modified = wire;
    modified[15] = modified[19];
    modified[16] = modified[20];
    modified[17] = modified[21];
    modified[18] = modified[22];
    assert(!decode_overlay_report(modified));
    // Critical threshold exceeds signed WMI range.
    modified = wire;
    modified[22] = std::byte{0x80}; // Bit 31 set: > INT32_MAX.
    assert(!decode_overlay_report(modified));
    // Warning threshold exceeds Critical threshold.
    modified = wire;
    modified[15] = std::byte{0x00};
    modified[16] = std::byte{0x0f};
    assert(!decode_overlay_report(modified));
    // Known=false with non-zero fields.
    modified = wire;
    modified[1] = std::byte{0};
    assert(!decode_overlay_report(modified));
    // Consumption exceeds MaximumSize.
    modified = wire;
    modified[7] = std::byte{0x00};
    modified[8] = std::byte{0x20}; // consumption = 0x2000 = 8192 > 4096
    assert(!decode_overlay_report(modified));
    // Available space exceeds MaximumSize.
    modified = wire;
    modified[11] = std::byte{0x00};
    modified[12] = std::byte{0x20}; // available = 0x2000 = 8192 > 4096
    assert(!decode_overlay_report(modified));
    // Consumption + available exceeds MaximumSize.
    modified = wire;
    modified[7] = std::byte{0x00};
    modified[8] = std::byte{0x08}; // consumption = 2048
    modified[11] = std::byte{0x00};
    modified[12] = std::byte{0x09}; // available = 2304; sum = 4352 > 4096
    assert(!decode_overlay_report(modified));
    // Consumption + available sum overflow.
    modified = wire;
    modified[7] = modified[8] = modified[9] = modified[10] = std::byte{0xff};
    assert(!decode_overlay_report(modified));
    // Strict encoding rejection.
    assert(encode_overlay_report(OverlayReport{true, OverlayType::ram, 0u, 0u, 0u, 0u, 0u})
               .empty());
    assert(encode_overlay_report(OverlayReport{false, OverlayType::disk, 0u, 0u, 0u, 0u, 0u})
               .empty());
    assert(encode_overlay_report(OverlayReport{false, OverlayType::ram, 1024u, 0u, 0u, 0u, 0u})
               .empty());
    assert(encode_overlay_report(OverlayReport{true, OverlayType::disk, 4096u, 5000u, 0u, 3000u, 3500u})
               .empty());
    assert(encode_overlay_report(OverlayReport{true, OverlayType::disk, 4096u, 0u, 5000u, 3000u, 3500u})
               .empty());
    assert(encode_overlay_report(OverlayReport{true, OverlayType::disk, 4096u, 3000u, 2000u, 3000u, 3500u})
               .empty());
    assert(encode_overlay_report(OverlayReport{true, OverlayType::disk, 4096u, 0u, 0u, 3600u, 3000u})
               .empty());
    assert(encode_overlay_report(OverlayReport{true, OverlayType::disk, 4096u, 0u, 0u, 0x80000000u, 3000u})
               .empty());
    assert(encode_overlay_report(OverlayReport{true, OverlayType::disk, 4096u, 0u, 0u, 3000u, 0x80000000u})
               .empty());

    // Pending-reboot state: current-session RAM overlay (1024 MiB), but thresholds
    // were raised to 3072 / 3686 MiB for next-session disk overlay (4096 MiB).
    const OverlayReport pending_report{true, OverlayType::ram, 1024u, 128u, 896u,
                                       3072u, 3686u};
    const auto pending_wire = encode_overlay_report(pending_report);
    assert(pending_wire.size() == kOverlayReportBytes);
    assert(decode_overlay_report(pending_wire) == pending_report);
    // Even in pending state, warning >= critical is still rejected.
    assert(encode_overlay_report(OverlayReport{true, OverlayType::ram, 1024u, 128u, 896u,
                                               3686u, 3072u})
               .empty());
    // And consumption + available exceeding maximum_size_mb is still rejected.
    assert(encode_overlay_report(OverlayReport{true, OverlayType::ram, 1024u, 600u, 600u,
                                               3072u, 3686u})
               .empty());
    return 0;
}
