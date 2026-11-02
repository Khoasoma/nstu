#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace nstu::uwf {

// Protocol ID assigned at integration.
inline constexpr std::uint8_t kOverlayReportVersion = 1;
inline constexpr std::size_t kOverlayReportBytes = 23;

enum class OverlayType : std::uint8_t { ram = 0, disk = 1 };

// All fields describe the current running session:
// - type: current-session overlay type (RAM or disk, from UWF_OverlayConfig CurrentSession=TRUE).
// - maximum_size_mb: current-session overlay maximum size in MiB.
// - consumption_mb: current-session overlay space consumed in MiB (from UWF_Overlay).
// - available_mb: current-session overlay space available in MiB (from UWF_Overlay).
// - warning_mb: current live warning threshold in MiB (from UWF_Overlay).
// - critical_mb: current live critical threshold in MiB (from UWF_Overlay).
//
// Pending-reboot state:
// When configure_uwf_overlay configures a larger overlay for the next boot, it sets the live
// warning and critical thresholds targeting the next-session size immediately. During this
// pending-reboot window, warning_mb and critical_mb can legitimately exceed current-session
// maximum_size_mb until the next restart. Both thresholds are bounded by INT32_MAX
// (WMI VT_I4 automation range).
struct OverlayReport {
    bool known = false;
    OverlayType type = OverlayType::ram;
    std::uint32_t maximum_size_mb = 0;
    std::uint32_t consumption_mb = 0;
    std::uint32_t available_mb = 0;
    std::uint32_t warning_mb = 0;
    std::uint32_t critical_mb = 0;
    bool operator==(const OverlayReport&) const = default;
};

[[nodiscard]] std::vector<std::byte> encode_overlay_report(
    const OverlayReport& report);
[[nodiscard]] std::optional<OverlayReport> decode_overlay_report(
    std::span<const std::byte> wire);

} // namespace nstu::uwf

