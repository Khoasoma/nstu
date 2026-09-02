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
namespace {

#ifdef _WIN32
constexpr GUID kProviderKey = {
    0x5f8b5c91, 0x4e2f, 0x4c9f,
    {0x9f, 0x3b, 0x1e, 0x2b, 0x4a, 0x9c, 0x77, 0x10}};
constexpr GUID kSubLayerKey = {
    0x1cb6fd1a, 0x8d1c, 0x45bf,
    {0x9d, 0x82, 0x6b, 0x88, 0x43, 0x7a, 0x5e, 0x21}};
constexpr GUID kBlockHttpKey = {
    0xa6bc8b10, 0x7612, 0x4d91,
    {0x9f, 0x08, 0x39, 0x11, 0x5d, 0x4c, 0x6a, 0x01}};
constexpr GUID kBlockHttpsKey = {
    0xa6bc8b11, 0x7612, 0x4d91,
    {0x9f, 0x08, 0x39, 0x11, 0x5d, 0x4c, 0x6a, 0x02}};
constexpr GUID kPermitBaseKey = {
    0xa6bc8b20, 0x7612, 0x4d91,
    {0x9f, 0x08, 0x39, 0x11, 0x5d, 0x4c, 0x6a, 0x20}};
constexpr GUID kAleAuthConnectV4 = {
    0xc38d57d1, 0x05a7, 0x4c33,
    {0x90, 0x4f, 0x7f, 0xbc, 0xee, 0xe6, 0x0e, 0x82}};
constexpr GUID kIpRemoteAddress = {
    0xb235ae9a, 0x1d64, 0x49b8,
    {0xa4, 0x4c, 0x5f, 0xf3, 0xd9, 0x09, 0x50, 0x45}};
constexpr GUID kIpRemotePort = {
    0xc35a604d, 0xd22b, 0x4e1a,
    {0x91, 0xb4, 0x68, 0xf6, 0x74, 0xee, 0x67, 0x4b}};

constexpr UINT32 kPersistentFilterFlag = 0x00000001u;

void set_error(std::string* error, const char* message, DWORD status = 0) {
    if (error == nullptr) {
        return;
    }
    *error = message;
    if (status != ERROR_SUCCESS) {
        *error += " (status " + std::to_string(status) + ")";
    }
}

GUID permit_key(std::size_t index) {
    GUID key = kPermitBaseKey;
    const auto value = static_cast<unsigned long>(index);
    key.Data1 += value;
    key.Data4[7] = static_cast<unsigned char>(value & 0xffu);
    return key;
}

bool add_filter(HANDLE engine, const GUID& key,
                const std::vector<FWPM_FILTER_CONDITION0>& conditions,
                FWP_ACTION_TYPE action, UINT8 weight, std::string* error) {
    FWPM_FILTER0 filter{};
    filter.filterKey = key;
    filter.displayData.name = const_cast<wchar_t*>(L"NSTU website allowlist");
    filter.displayData.description =
        const_cast<wchar_t*>(L"NSTU administrator-managed IP policy");
    filter.flags = kPersistentFilterFlag;
    filter.providerKey = const_cast<GUID*>(&kProviderKey);
    filter.layerKey = kAleAuthConnectV4;
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

#endif

} // namespace

bool WfpWebsiteAllowlist::apply(const WfpAllowlistConfig& config,
                                std::string* error) {
    if (config.allowed_ipv4.empty()) {
        if (error != nullptr) {
            *error = "website allowlist must contain at least one IPv4 address";
        }
        return false;
    }

#ifndef _WIN32
    if (error != nullptr) {
        *error = "WFP website allowlist is only available on Windows";
    }
    return false;
#else
    auto addresses = config.allowed_ipv4;
    std::sort(addresses.begin(), addresses.end());
    addresses.erase(std::unique(addresses.begin(), addresses.end()),
                    addresses.end());
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

    const auto add_port_block = [&](const GUID& key, UINT16 port) {
        return add_filter(engine, key, {port_condition(port)}, FWP_ACTION_BLOCK,
                          10, error);
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

    std::size_t index = 0;
    for (const UINT32 address : addresses) {
        FWP_V4_ADDR_AND_MASK mask{address, 0xffffffffu};
        FWPM_FILTER_CONDITION0 condition{};
        condition.fieldKey = kIpRemoteAddress;
        condition.matchType = FWP_MATCH_EQUAL;
        condition.conditionValue.type = FWP_V4_ADDR_MASK;
        condition.conditionValue.v4AddrMask = &mask;
        if (!add_filter(engine, permit_key(index++), {condition},
                        FWP_ACTION_PERMIT, 20, error)) {
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
    const std::array<GUID, 2> fixed_keys{kBlockHttpKey, kBlockHttpsKey};
    for (const auto& key : fixed_keys) {
        (void)FwpmFilterDeleteByKey0(engine, &key);
    }
    // Permit filters have deterministic keys in a bounded namespace. Remove
    // the namespace until the API reports a missing key.
    for (std::size_t index = 0; index < 4096; ++index) {
        const GUID key = permit_key(index);
        const DWORD result = FwpmFilterDeleteByKey0(engine, &key);
        (void)result;
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
