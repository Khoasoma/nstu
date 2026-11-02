#include "nstu/uwf_overlay_report.hpp"

#include <limits>

namespace nstu::uwf {
namespace {

bool valid(const OverlayReport& report) {
    if (!report.known) {
        return report.type == OverlayType::ram && report.maximum_size_mb == 0u &&
               report.consumption_mb == 0u && report.available_mb == 0u &&
               report.warning_mb == 0u && report.critical_mb == 0u;
    }
    const auto max_mb = static_cast<std::uint64_t>(report.maximum_size_mb);
    const auto sum_mb = static_cast<std::uint64_t>(report.consumption_mb) +
                        static_cast<std::uint64_t>(report.available_mb);
    constexpr auto kMaxSignedI32 =
        static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
    // Live thresholds describe the live UWF_Overlay row. When configure_uwf_overlay
    // configures a larger overlay for the next boot, live thresholds immediately reflect
    // the target next-boot size and may legitimately exceed current-session maximum_size_mb
    // while a reboot is pending. Both thresholds are bounded by INT32_MAX (VT_I4).
    return (report.type == OverlayType::ram ||
            report.type == OverlayType::disk) &&
           report.maximum_size_mb > 0u &&
           report.maximum_size_mb <= kMaxSignedI32 &&
           report.consumption_mb <= report.maximum_size_mb &&
           report.available_mb <= report.maximum_size_mb &&
           sum_mb <= max_mb &&
           report.warning_mb <= kMaxSignedI32 &&
           report.critical_mb <= kMaxSignedI32 &&
           (report.warning_mb == 0u || report.critical_mb == 0u ||
            report.warning_mb < report.critical_mb);
}

void append_u32(std::vector<std::byte>& bytes, std::uint32_t value) {
    for (unsigned shift = 0u; shift < 32u; shift += 8u) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
    }
}

std::uint32_t read_u32(std::span<const std::byte> wire, std::size_t offset) {
    std::uint32_t value = 0u;
    for (unsigned shift = 0u; shift < 32u; shift += 8u) {
        value |= std::to_integer<std::uint32_t>(wire[offset++]) << shift;
    }
    return value;
}

} // namespace

std::vector<std::byte> encode_overlay_report(const OverlayReport& report) {
    if (!valid(report)) return {};
    std::vector<std::byte> bytes;
    bytes.reserve(kOverlayReportBytes);
    bytes.push_back(static_cast<std::byte>(kOverlayReportVersion));
    bytes.push_back(report.known ? std::byte{1} : std::byte{0});
    bytes.push_back(static_cast<std::byte>(report.type));
    append_u32(bytes, report.maximum_size_mb);
    append_u32(bytes, report.consumption_mb);
    append_u32(bytes, report.available_mb);
    append_u32(bytes, report.warning_mb);
    append_u32(bytes, report.critical_mb);
    return bytes;
}

std::optional<OverlayReport> decode_overlay_report(
    std::span<const std::byte> wire) {
    if (wire.size() != kOverlayReportBytes ||
        wire[0] != static_cast<std::byte>(kOverlayReportVersion) ||
        wire[1] > std::byte{1}) {
        return std::nullopt;
    }
    OverlayReport report;
    report.known = wire[1] == std::byte{1};
    report.type = static_cast<OverlayType>(std::to_integer<std::uint8_t>(wire[2]));
    report.maximum_size_mb = read_u32(wire, 3);
    report.consumption_mb = read_u32(wire, 7);
    report.available_mb = read_u32(wire, 11);
    report.warning_mb = read_u32(wire, 15);
    report.critical_mb = read_u32(wire, 19);
    if (!valid(report)) return std::nullopt;
    return report;
}

} // namespace nstu::uwf

