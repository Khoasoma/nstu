#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nstu::setup {

struct DisplayScan { std::uint32_t width = 0; std::uint32_t height = 0; std::uint32_t refresh_hz = 0; };
struct NetworkScan { std::wstring adapter_name; std::uint64_t link_speed_mbps = 0; bool operational = false; };
struct HardwareScan { DisplayScan display; std::vector<NetworkScan> networks; };

[[nodiscard]] HardwareScan scan_hardware();

} // namespace nstu::setup
