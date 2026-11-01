#include "nstu/wfp_allowlist.hpp"

#include <algorithm>
#include <array>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <fwpmu.h>
#include <fwptypes.h>
#include <ws2tcpip.h>
#endif

namespace nstu::net {

WfpWebsiteAllowlist::~WfpWebsiteAllowlist() {
    if (active_) {
        (void)clear(nullptr);
    }
}

WfpWebsiteAllowlist::WfpWebsiteAllowlist(WfpWebsiteAllowlist&& other) noexcept
    : active_(other.active_) {
    other.active_ = false;
}

WfpWebsiteAllowlist& WfpWebsiteAllowlist::operator=(
    WfpWebsiteAllowlist&& other) noexcept {
    if (this != &other) {
        if (active_) {
            (void)clear(nullptr);
        }
        active_ = other.active_;
        other.active_ = false;
    }
    return *this;
}

namespace {

#ifdef _WIN32
constexpr GUID kProviderKey = {
    0x5f8b5c91, 0x4e2f, 0x4c9f,
    {0x9f, 0x3b, 0x1e, 0x2b, 0x4a, 0x9c, 0x77, 0x10}};
constexpr GUID kSubLayerKey = {
    0x1cb6fd1a, 0x8d1c, 0x45bf,
    {0x9d, 0x82, 0x6b, 0x88, 0x43, 0x7a, 0x5e, 0x21}};

// Fixed filter keys
constexpr GUID kBlockDefaultDenyV4Key = {
    0xa6bc8b10, 0x7612, 0x4d91,
    {0x9f, 0x08, 0x39, 0x11, 0x5d, 0x4c, 0x6a, 0x10}};
constexpr GUID kBlockDefaultDenyV6Key = {
    0xa6bc8b10, 0x7612, 0x4d91,
    {0x9f, 0x08, 0x39, 0x11, 0x5d, 0x4c, 0x6a, 0x11}};
constexpr GUID kPermitLoopbackV4Key = {
    0xa6bc8b10, 0x7612, 0x4d91,
    {0x9f, 0x08, 0x39, 0x11, 0x5d, 0x4c, 0x6a, 0x12}};
constexpr GUID kPermitLoopbackV6Key = {
    0xa6bc8b10, 0x7612, 0x4d91,
    {0x9f, 0x08, 0x39, 0x11, 0x5d, 0x4c, 0x6a, 0x13}};
constexpr GUID kPermitDnsV4Key = {
    0xa6bc8b10, 0x7612, 0x4d91,
    {0x9f, 0x08, 0x39, 0x11, 0x5d, 0x4c, 0x6a, 0x14}};
constexpr GUID kPermitDnsV6Key = {
    0xa6bc8b10, 0x7612, 0x4d91,
    {0x9f, 0x08, 0x39, 0x11, 0x5d, 0x4c, 0x6a, 0x15}};
constexpr GUID kPermitDhcpV4Key = {
    0xa6bc8b10, 0x7612, 0x4d91,
    {0x9f, 0x08, 0x39, 0x11, 0x5d, 0x4c, 0x6a, 0x16}};
constexpr GUID kPermitDhcpV6Key = {
    0xa6bc8b10, 0x7612, 0x4d91,
    {0x9f, 0x08, 0x39, 0x11, 0x5d, 0x4c, 0x6a, 0x17}};
constexpr GUID kPermitServerV4Key = {
    0xa6bc8b10, 0x7612, 0x4d91,
    {0x9f, 0x08, 0x39, 0x11, 0x5d, 0x4c, 0x6a, 0x18}};
constexpr GUID kPermitServerV6Key = {
    0xa6bc8b10, 0x7612, 0x4d91,
    {0x9f, 0x08, 0x39, 0x11, 0x5d, 0x4c, 0x6a, 0x19}};
constexpr GUID kBlockHttpKey = {
    0xa6bc8b10, 0x7612, 0x4d91,
    {0x9f, 0x08, 0x39, 0x11, 0x5d, 0x4c, 0x6a, 0x01}};
constexpr GUID kBlockHttpsKey = {
    0xa6bc8b11, 0x7612, 0x4d91,
    {0x9f, 0x08, 0x39, 0x11, 0x5d, 0x4c, 0x6a, 0x02}};

// Dynamic permit base keys
constexpr GUID kPermitV4BaseKey = {
    0xa6bc8b20, 0x7612, 0x4d91,
    {0x9f, 0x08, 0x39, 0x11, 0x5d, 0x4c, 0x6a, 0x20}};
constexpr GUID kPermitV6BaseKey = {
    0xa6bc8b30, 0x7612, 0x4d91,
    {0x9f, 0x08, 0x39, 0x11, 0x5d, 0x4c, 0x6a, 0x30}};

// WFP ALE layer GUIDs
constexpr GUID kAleAuthConnectV4 = {
    0xc38d57d1, 0x05a7, 0x4c33,
    {0x90, 0x4f, 0x7f, 0xbc, 0xee, 0xe6, 0x0e, 0x82}};
constexpr GUID kAleAuthConnectV6 = {
    0x4a72393b, 0x319f, 0x44bc,
    {0x84, 0xc3, 0xba, 0x54, 0xdc, 0xb3, 0xb6, 0xb4}};

// WFP Field Condition GUIDs
constexpr GUID kIpRemoteAddress = {
    0xb235ae9a, 0x1d64, 0x49b8,
    {0xa4, 0x4c, 0x5f, 0xf3, 0xd9, 0x09, 0x50, 0x45}};
constexpr GUID kIpRemotePort = {
    0xc35a604d, 0xd22b, 0x4e1a,
    {0x91, 0xb4, 0x68, 0xf6, 0x74, 0xee, 0x67, 0x4b}};

void set_error(std::string* error, const char* message, DWORD status = 0) {
    if (error == nullptr) {
        return;
    }
    *error = message;
    if (status != ERROR_SUCCESS) {
        *error += " (status " + std::to_string(status) + ")";
    }
}

GUID permit_v4_key(std::size_t index) {
    GUID key = kPermitV4BaseKey;
    const auto value = static_cast<unsigned long>(index);
    key.Data1 += value;
    key.Data4[7] = static_cast<unsigned char>((key.Data4[7] + (value & 0xffu)) & 0xffu);
    return key;
}

GUID permit_v6_key(std::size_t index) {
    GUID key = kPermitV6BaseKey;
    const auto value = static_cast<unsigned long>(index);
    key.Data1 += value;
    key.Data4[7] = static_cast<unsigned char>((key.Data4[7] + (value & 0xffu)) & 0xffu);
    return key;
}

bool add_filter(HANDLE engine, const GUID& layerKey, const GUID& key,
                const std::vector<FWPM_FILTER_CONDITION0>& conditions,
                FWP_ACTION_TYPE action, UINT8 weight, const wchar_t* name,
                std::string* error) {
    FWPM_FILTER0 filter{};
    filter.filterKey = key;
    filter.displayData.name = const_cast<wchar_t*>(name ? name : L"NSTU website allowlist");
    filter.displayData.description =
        const_cast<wchar_t*>(L"NSTU administrator-managed IP policy");
    filter.flags = 0; // Dynamic session filter: fail-safe auto-cleanup on service crash
    filter.providerKey = const_cast<GUID*>(&kProviderKey);
    filter.layerKey = layerKey;
    filter.subLayerKey = kSubLayerKey;
    filter.weight.type = FWP_UINT8;
    filter.weight.uint8 = weight;
    filter.numFilterConditions = static_cast<UINT32>(conditions.size());
    filter.filterCondition = conditions.empty()
        ? nullptr
        : const_cast<FWPM_FILTER_CONDITION0*>(conditions.data());
    filter.action.type = action;
    UINT64 id = 0;
    const DWORD status = FwpmFilterAdd0(engine, &filter, nullptr, &id);
    if (status != ERROR_SUCCESS) {
        set_error(error, "FwpmFilterAdd0 failed", status);
        return false;
    }
    return true;
}

FWPM_FILTER_CONDITION0 port_condition(UINT16 port) {
    FWPM_FILTER_CONDITION0 condition{};
    condition.fieldKey = kIpRemotePort;
    condition.matchType = FWP_MATCH_EQUAL;
    condition.conditionValue.type = FWP_UINT16;
    condition.conditionValue.uint16 = port;
    return condition;
}

FWPM_FILTER_CONDITION0 ipv4_condition(FWP_V4_ADDR_AND_MASK* mask_ptr) {
    FWPM_FILTER_CONDITION0 condition{};
    condition.fieldKey = kIpRemoteAddress;
    condition.matchType = FWP_MATCH_EQUAL;
    condition.conditionValue.type = FWP_V4_ADDR_MASK;
    condition.conditionValue.v4AddrMask = mask_ptr;
    return condition;
}

FWPM_FILTER_CONDITION0 ipv6_condition(FWP_V6_ADDR_AND_MASK* mask_ptr) {
    FWPM_FILTER_CONDITION0 condition{};
    condition.fieldKey = kIpRemoteAddress;
    condition.matchType = FWP_MATCH_EQUAL;
    condition.conditionValue.type = FWP_V6_ADDR_MASK;
    condition.conditionValue.v6AddrMask = mask_ptr;
    return condition;
}

bool is_zero_ipv6(const Ipv6Address& ip) noexcept {
    return std::all_of(ip.bytes.begin(), ip.bytes.end(), [](std::uint8_t b) { return b == 0; });
}

#endif

} // namespace

bool WfpWebsiteAllowlist::apply(const WfpAllowlistConfig& config,
                                std::string* error) {
    if (!config.default_deny && config.allowed_ipv4.empty() && config.allowed_ipv6.empty()) {
        if (error != nullptr) {
            *error = "website allowlist must contain at least one allowed address";
        }
        return false;
    }

#ifndef _WIN32
    if (error != nullptr) {
        *error = "WFP website allowlist is only available on Windows";
    }
    return false;
#else
    auto v4_addresses = config.allowed_ipv4;
    std::sort(v4_addresses.begin(), v4_addresses.end());
    v4_addresses.erase(std::unique(v4_addresses.begin(), v4_addresses.end()),
                       v4_addresses.end());

    auto v6_addresses = config.allowed_ipv6;
    std::sort(v6_addresses.begin(), v6_addresses.end());
    v6_addresses.erase(std::unique(v6_addresses.begin(), v6_addresses.end()),
                       v6_addresses.end());

    HANDLE engine = nullptr;
    DWORD status = FwpmEngineOpen0(nullptr, RPC_C_AUTHN_WINNT, nullptr,
                                   nullptr, &engine);
    if (status != ERROR_SUCCESS || engine == nullptr) {
        set_error(error, "FwpmEngineOpen0 failed", status);
        return false;
    }

    (void)clear(nullptr);
    status = FwpmProviderDeleteByKey0(engine, &kProviderKey);
    if (status != ERROR_SUCCESS &&
        status != static_cast<DWORD>(FWP_E_PROVIDER_NOT_FOUND)) {
        FwpmEngineClose0(engine);
        set_error(error, "FwpmProviderDeleteByKey0 failed", status);
        return false;
    }

    FWPM_PROVIDER0 provider{};
    provider.providerKey = kProviderKey;
    provider.displayData.name = const_cast<wchar_t*>(L"NSTU");
    provider.displayData.description =
        const_cast<wchar_t*>(L"NSTU website allowlist provider");
    provider.serviceName = const_cast<wchar_t*>(L"nstu-service");
    status = FwpmProviderAdd0(engine, &provider, nullptr);
    if (status != ERROR_SUCCESS &&
        status != static_cast<DWORD>(FWP_E_ALREADY_EXISTS)) {
        FwpmEngineClose0(engine);
        set_error(error, "FwpmProviderAdd0 failed", status);
        return false;
    }

    FWPM_SUBLAYER0 sublayer{};
    sublayer.subLayerKey = kSubLayerKey;
    sublayer.displayData.name = const_cast<wchar_t*>(L"NSTU website allowlist");
    sublayer.displayData.description =
        const_cast<wchar_t*>(L"NSTU explicit administrator policy");
    sublayer.providerKey = const_cast<GUID*>(&kProviderKey);
    sublayer.weight = 0x8000;
    status = FwpmSubLayerAdd0(engine, &sublayer, nullptr);
    if (status != ERROR_SUCCESS &&
        status != static_cast<DWORD>(FWP_E_ALREADY_EXISTS)) {
        FwpmEngineClose0(engine);
        set_error(error, "FwpmSubLayerAdd0 failed", status);
        return false;
    }

    if (config.default_deny) {
        // 1. Dual-stack Outbound Default-Deny Block Filter at Weight 1
        if (!add_filter(engine, kAleAuthConnectV4, kBlockDefaultDenyV4Key, {},
                        FWP_ACTION_BLOCK, 1, L"NSTU default deny IPv4", error)) {
            FwpmEngineClose0(engine);
            (void)clear(nullptr);
            return false;
        }
        if (!add_filter(engine, kAleAuthConnectV6, kBlockDefaultDenyV6Key, {},
                        FWP_ACTION_BLOCK, 1, L"NSTU default deny IPv6", error)) {
            FwpmEngineClose0(engine);
            (void)clear(nullptr);
            return false;
        }

        // 2. Loopback Permit Filters at Weight 20 (127.0.0.0/8 and ::1/128)
        if (config.permit_loopback) {
            FWP_V4_ADDR_AND_MASK loopback_v4{0x7f000000u, 0xff000000u};
            if (!add_filter(engine, kAleAuthConnectV4, kPermitLoopbackV4Key,
                            {ipv4_condition(&loopback_v4)},
                            FWP_ACTION_PERMIT, 20, L"NSTU permit loopback IPv4", error)) {
                FwpmEngineClose0(engine);
                (void)clear(nullptr);
                return false;
            }

            FWP_V6_ADDR_AND_MASK loopback_v6{};
            loopback_v6.addr[15] = 1;
            loopback_v6.prefixLength = 128;
            if (!add_filter(engine, kAleAuthConnectV6, kPermitLoopbackV6Key,
                            {ipv6_condition(&loopback_v6)},
                            FWP_ACTION_PERMIT, 20, L"NSTU permit loopback IPv6", error)) {
                FwpmEngineClose0(engine);
                (void)clear(nullptr);
                return false;
            }
        }

        // 3. DNS & DHCP Permit Filters at Weight 20
        if (config.permit_dns_dhcp) {
            // DNS (port 53 UDP/TCP) on V4 and V6
            if (!add_filter(engine, kAleAuthConnectV4, kPermitDnsV4Key,
                            {port_condition(53)},
                            FWP_ACTION_PERMIT, 20, L"NSTU permit DNS IPv4", error)) {
                FwpmEngineClose0(engine);
                (void)clear(nullptr);
                return false;
            }
            if (!add_filter(engine, kAleAuthConnectV6, kPermitDnsV6Key,
                            {port_condition(53)},
                            FWP_ACTION_PERMIT, 20, L"NSTU permit DNS IPv6", error)) {
                FwpmEngineClose0(engine);
                (void)clear(nullptr);
                return false;
            }

            // DHCPv4 (remote port 67 UDP) and DHCPv6 (remote port 547 UDP)
            if (!add_filter(engine, kAleAuthConnectV4, kPermitDhcpV4Key,
                            {port_condition(67)},
                            FWP_ACTION_PERMIT, 20, L"NSTU permit DHCPv4", error)) {
                FwpmEngineClose0(engine);
                (void)clear(nullptr);
                return false;
            }
            if (!add_filter(engine, kAleAuthConnectV6, kPermitDhcpV6Key,
                            {port_condition(547)},
                            FWP_ACTION_PERMIT, 20, L"NSTU permit DHCPv6", error)) {
                FwpmEngineClose0(engine);
                (void)clear(nullptr);
                return false;
            }
        }

        // 4. Teacher Server Endpoint Permit at Weight 20
        if (config.server_ipv4 != 0) {
            FWP_V4_ADDR_AND_MASK srv_v4{config.server_ipv4, 0xffffffffu};
            std::vector<FWPM_FILTER_CONDITION0> conds{ipv4_condition(&srv_v4)};
            if (config.server_port != 0) {
                conds.push_back(port_condition(config.server_port));
            }
            if (!add_filter(engine, kAleAuthConnectV4, kPermitServerV4Key, conds,
                            FWP_ACTION_PERMIT, 20, L"NSTU permit server IPv4", error)) {
                FwpmEngineClose0(engine);
                (void)clear(nullptr);
                return false;
            }
        }
        if (!is_zero_ipv6(config.server_ipv6)) {
            FWP_V6_ADDR_AND_MASK srv_v6{};
            std::copy(config.server_ipv6.bytes.begin(), config.server_ipv6.bytes.end(),
                      srv_v6.addr);
            srv_v6.prefixLength = 128;
            std::vector<FWPM_FILTER_CONDITION0> conds{ipv6_condition(&srv_v6)};
            if (config.server_port != 0) {
                conds.push_back(port_condition(config.server_port));
            }
            if (!add_filter(engine, kAleAuthConnectV6, kPermitServerV6Key, conds,
                            FWP_ACTION_PERMIT, 20, L"NSTU permit server IPv6", error)) {
                FwpmEngineClose0(engine);
                (void)clear(nullptr);
                return false;
            }
        }
    } else {
        // Legacy mode: block HTTP/HTTPS ports at weight 10
        const auto add_port_block = [&](const GUID& key, UINT16 port) {
            return add_filter(engine, kAleAuthConnectV4, key, {port_condition(port)},
                              FWP_ACTION_BLOCK, 10, L"NSTU port block", error);
        };
        if (config.block_http && !add_port_block(kBlockHttpKey, 80)) {
            FwpmEngineClose0(engine);
            (void)clear(nullptr);
            return false;
        }
        if (config.block_https && !add_port_block(kBlockHttpsKey, 443)) {
            FwpmEngineClose0(engine);
            (void)clear(nullptr);
            return false;
        }
    }

    // 5. Allowed exam document origins (Weight 20)
    std::size_t index = 0;
    for (const UINT32 address : v4_addresses) {
        FWP_V4_ADDR_AND_MASK mask{address, 0xffffffffu};
        FWPM_FILTER_CONDITION0 condition = ipv4_condition(&mask);
        if (!add_filter(engine, kAleAuthConnectV4, permit_v4_key(index++), {condition},
                        FWP_ACTION_PERMIT, 20, L"NSTU website allowlist IPv4", error)) {
            FwpmEngineClose0(engine);
            (void)clear(nullptr);
            return false;
        }
    }

    std::size_t v6_index = 0;
    for (const auto& addr6 : v6_addresses) {
        FWP_V6_ADDR_AND_MASK mask{};
        std::copy(addr6.bytes.begin(), addr6.bytes.end(), mask.addr);
        mask.prefixLength = 128;
        FWPM_FILTER_CONDITION0 condition = ipv6_condition(&mask);
        if (!add_filter(engine, kAleAuthConnectV6, permit_v6_key(v6_index++), {condition},
                        FWP_ACTION_PERMIT, 20, L"NSTU website allowlist IPv6", error)) {
            FwpmEngineClose0(engine);
            (void)clear(nullptr);
            return false;
        }
    }

    FwpmEngineClose0(engine);
    active_ = true;
    return true;
#endif
}

bool WfpWebsiteAllowlist::clear(std::string* error) {
#ifndef _WIN32
    if (error != nullptr) {
        *error = "WFP website allowlist is only available on Windows";
    }
    return false;
#else
    HANDLE engine = nullptr;
    DWORD status = FwpmEngineOpen0(nullptr, RPC_C_AUTHN_WINNT, nullptr,
                                   nullptr, &engine);
    if (status != ERROR_SUCCESS || engine == nullptr) {
        set_error(error, "FwpmEngineOpen0 failed", status);
        return false;
    }
    const std::array<GUID, 12> fixed_keys{
        kBlockDefaultDenyV4Key,
        kBlockDefaultDenyV6Key,
        kPermitLoopbackV4Key,
        kPermitLoopbackV6Key,
        kPermitDnsV4Key,
        kPermitDnsV6Key,
        kPermitDhcpV4Key,
        kPermitDhcpV6Key,
        kPermitServerV4Key,
        kPermitServerV6Key,
        kBlockHttpKey,
        kBlockHttpsKey,
    };
    for (const auto& key : fixed_keys) {
        (void)FwpmFilterDeleteByKey0(engine, &key);
    }

    // Permit filters have deterministic keys in bounded namespaces. Remove
    // sequentially; stop after 5 consecutive missing filters for responsiveness.
    std::size_t consecutive_missing = 0;
    for (std::size_t index = 0; index < 4096; ++index) {
        const GUID key = permit_v4_key(index);
        const DWORD result = FwpmFilterDeleteByKey0(engine, &key);
        if (result == static_cast<DWORD>(FWP_E_FILTER_NOT_FOUND)) {
            if (++consecutive_missing >= 5) {
                break;
            }
        } else {
            consecutive_missing = 0;
        }
    }

    consecutive_missing = 0;
    for (std::size_t index = 0; index < 4096; ++index) {
        const GUID key = permit_v6_key(index);
        const DWORD result = FwpmFilterDeleteByKey0(engine, &key);
        if (result == static_cast<DWORD>(FWP_E_FILTER_NOT_FOUND)) {
            if (++consecutive_missing >= 5) {
                break;
            }
        } else {
            consecutive_missing = 0;
        }
    }

    (void)FwpmSubLayerDeleteByKey0(engine, &kSubLayerKey);
    (void)FwpmProviderDeleteByKey0(engine, &kProviderKey);
    FwpmEngineClose0(engine);
    active_ = false;
    (void)error;
    return true;
#endif
}

} // namespace nstu::net
