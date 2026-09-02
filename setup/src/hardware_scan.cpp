#include "nstu/setup/hardware_scan.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>

#include <utility>
#include <vector>

namespace nstu::setup {

HardwareScan scan_hardware() {
    HardwareScan result;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) != FALSE) {
        result.display = {mode.dmPelsWidth, mode.dmPelsHeight,
                          mode.dmDisplayFrequency};
    }

    ULONG bytes = 0;
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr,
                             nullptr, &bytes) != ERROR_BUFFER_OVERFLOW) {
        return result;
    }
    std::vector<std::byte> buffer(bytes);
    auto* addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr,
                             addresses, &bytes) != NO_ERROR) {
        return result;
    }
    for (auto* adapter = addresses; adapter != nullptr;
         adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp ||
            adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }
        NetworkScan network;
        if (adapter->FriendlyName != nullptr) {
            network.adapter_name = adapter->FriendlyName;
        }
        network.link_speed_mbps = adapter->TransmitLinkSpeed / 1'000'000u;
        network.operational = true;
        result.networks.push_back(std::move(network));
    }
    return result;
}

} // namespace nstu::setup
