#include "nstu/setup/uwf.hpp"

#include <windows.h>
#include <wbemidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace nstu::setup {
namespace {

using Microsoft::WRL::ComPtr;

constexpr std::uint32_t kProductEnterprise = 0x00000004;
constexpr std::uint32_t kProductEnterpriseN = 0x0000001b;
constexpr std::uint32_t kProductEducation = 0x00000079;
constexpr std::uint32_t kProductEducationN = 0x0000007a;
constexpr std::uint32_t kProductEnterpriseS = 0x0000007d;
constexpr std::uint32_t kProductEnterpriseSN = 0x0000007e;
constexpr std::uint32_t kProductIotEnterprise = 0x000000bc;
constexpr std::uint32_t kProductIotEnterpriseS = 0x000000bf;
constexpr wchar_t kUwfNamespace[] = L"ROOT\\standardcimv2\\embedded";
// Microsoft requires a full registry path, not the HKLM abbreviation.
// Source: https://learn.microsoft.com/en-us/windows/configuration/unified-write-filter/uwf-registryfilteraddexclusion
constexpr wchar_t kNstuRegistryExclusion[] =
    L"HKEY_LOCAL_MACHINE\\SOFTWARE\\NSTU";

class Bstr {
public:
    explicit Bstr(const wchar_t* value) : value_(SysAllocString(value)) {}
    explicit Bstr(std::wstring_view value)
        : value_(SysAllocStringLen(value.data(),
                                   static_cast<UINT>(value.size()))) {}
    ~Bstr() { SysFreeString(value_); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    [[nodiscard]] BSTR get() const noexcept { return value_; }

private:
    BSTR value_ = nullptr;
};

class ComApartment {
public:
    ComApartment() noexcept : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComApartment() {
        if (SUCCEEDED(result_)) {
            CoUninitialize();
        }
    }
    [[nodiscard]] bool ready() const noexcept {
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
    }

private:
    HRESULT result_;
};

void set_failure(UwfConfigureResult& result, UwfConfigureOutcome outcome,
                 std::string detail) {
    result.outcome = outcome;
    result.detail = std::move(detail);
}

bool variant_bool(IWbemClassObject* object, const wchar_t* name, bool& value) {
    VARIANT variant;
    VariantInit(&variant);
    Bstr property(name);
    const HRESULT status = object->Get(property.get(), 0, &variant, nullptr,
                                       nullptr);
    const bool valid = SUCCEEDED(status) && variant.vt == VT_BOOL;
    if (valid) {
        value = variant.boolVal != VARIANT_FALSE;
    }
    VariantClear(&variant);
    return valid;
}

bool variant_uint32(IWbemClassObject* object, const wchar_t* name,
                    std::uint32_t& value) {
    VARIANT variant;
    VariantInit(&variant);
    Bstr property(name);
    const HRESULT status = object->Get(property.get(), 0, &variant, nullptr,
                                       nullptr);
    bool valid = false;
    if (SUCCEEDED(status) && variant.vt == VT_UI4) {
        value = variant.ulVal;
        valid = true;
    } else if (SUCCEEDED(status) && variant.vt == VT_I4 && variant.lVal >= 0) {
        value = static_cast<std::uint32_t>(variant.lVal);
        valid = true;
    }
    VariantClear(&variant);
    return valid;
}

bool variant_string(IWbemClassObject* object, const wchar_t* name,
                    std::wstring& value) {
    VARIANT variant;
    VariantInit(&variant);
    Bstr property(name);
    const HRESULT status = object->Get(property.get(), 0, &variant, nullptr,
                                       nullptr);
    const bool valid = SUCCEEDED(status) && variant.vt == VT_BSTR &&
                       variant.bstrVal != nullptr;
    if (valid) {
        value.assign(variant.bstrVal, SysStringLen(variant.bstrVal));
    }
    VariantClear(&variant);
    return valid;
}

class WmiWriter {
public:
    bool connect() {
        ComPtr<IWbemLocator> locator;
        if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr,
                                    CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&locator)))) {
            return false;
        }
        Bstr ns(kUwfNamespace);
        if (ns.get() == nullptr ||
            FAILED(locator->ConnectServer(ns.get(), nullptr, nullptr, nullptr,
                                          WBEM_FLAG_CONNECT_USE_MAX_WAIT,
                                          nullptr, nullptr, &services_))) {
            return false;
        }
        return SUCCEEDED(CoSetProxyBlanket(
            services_.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
            RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr,
            EOAC_NONE));
    }

    bool query_one(const wchar_t* query, ComPtr<IWbemClassObject>& object) {
        object.Reset();
        Bstr language(L"WQL");
        Bstr text(query);
        ComPtr<IEnumWbemClassObject> enumerator;
        if (!services_ || language.get() == nullptr || text.get() == nullptr ||
            FAILED(services_->ExecQuery(
                language.get(), text.get(),
                WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                nullptr, &enumerator))) {
            return false;
        }
        ULONG returned = 0;
        return enumerator->Next(3000, 1, object.GetAddressOf(), &returned) ==
                   WBEM_S_NO_ERROR &&
               returned == 1u && object != nullptr;
    }

    bool call(IWbemClassObject* instance, const wchar_t* method,
              std::wstring_view argument_name = {},
              std::wstring_view argument_value = {},
              std::uint32_t* return_value = nullptr,
              ComPtr<IWbemClassObject>* output_parameters = nullptr) {
        if (instance == nullptr || services_ == nullptr) {
            return false;
        }
        std::wstring object_path;
        std::wstring class_name;
        if (!variant_string(instance, L"__PATH", object_path) ||
            !variant_string(instance, L"__CLASS", class_name)) {
            return false;
        }
        Bstr class_path(class_name);
        ComPtr<IWbemClassObject> class_object;
        if (class_path.get() == nullptr ||
            FAILED(services_->GetObject(class_path.get(), 0, nullptr,
                                        &class_object, nullptr))) {
            return false;
        }
        Bstr method_name(method);
        ComPtr<IWbemClassObject> input_definition;
        if (method_name.get() == nullptr ||
            FAILED(class_object->GetMethod(method_name.get(), 0,
                                           &input_definition, nullptr))) {
            return false;
        }
        ComPtr<IWbemClassObject> input;
        if (input_definition &&
            FAILED(input_definition->SpawnInstance(0, &input))) {
            return false;
        }
        if (!argument_name.empty()) {
            if (!input) {
                return false;
            }
            Bstr property(argument_name);
            Bstr value(argument_value);
            VARIANT variant;
            VariantInit(&variant);
            variant.vt = VT_BSTR;
            variant.bstrVal = value.get();
            const HRESULT put = property.get() == nullptr || value.get() == nullptr
                ? E_OUTOFMEMORY
                : input->Put(property.get(), 0, &variant, CIM_STRING);
            // `value` owns the BSTR; do not VariantClear this borrowed value.
            variant.vt = VT_EMPTY;
            variant.bstrVal = nullptr;
            if (FAILED(put)) {
                return false;
            }
        }
        Bstr path(object_path);
        ComPtr<IWbemClassObject> output;
        if (path.get() == nullptr ||
            FAILED(services_->ExecMethod(path.get(), method_name.get(), 0,
                                         nullptr, input.Get(), &output,
                                         nullptr)) ||
            !output) {
            return false;
        }
        std::uint32_t returned = 0;
        if (!variant_uint32(output.Get(), L"ReturnValue", returned)) {
            return false;
        }
        if (return_value != nullptr) {
            *return_value = returned;
        }
        if (output_parameters != nullptr) {
            *output_parameters = output;
        }
        return returned == 0u;
    }

    bool contains(IWbemClassObject* instance, const wchar_t* argument_name,
                  std::wstring_view argument_value, bool& found) {
        found = false;
        ComPtr<IWbemClassObject> output;
        if (!call(instance, L"FindExclusion", argument_name, argument_value,
                  nullptr, &output)) {
            return false;
        }
        return variant_bool(output.Get(), L"bFound", found);
    }

    bool call_uint32(IWbemClassObject* instance, const wchar_t* method,
                     std::wstring_view argument_name = {},
                     std::uint32_t argument_value = 0,
                     std::uint32_t* return_value = nullptr) {
        if (instance == nullptr || services_ == nullptr) {
            return false;
        }
        std::wstring object_path;
        std::wstring class_name;
        if (!variant_string(instance, L"__PATH", object_path) ||
            !variant_string(instance, L"__CLASS", class_name)) {
            return false;
        }
        Bstr class_path(class_name);
        ComPtr<IWbemClassObject> class_object;
        if (class_path.get() == nullptr ||
            FAILED(services_->GetObject(class_path.get(), 0, nullptr,
                                        &class_object, nullptr))) {
            return false;
        }
        Bstr method_name(method);
        ComPtr<IWbemClassObject> input_definition;
        if (method_name.get() == nullptr ||
            FAILED(class_object->GetMethod(method_name.get(), 0,
                                           &input_definition, nullptr))) {
            return false;
        }
        ComPtr<IWbemClassObject> input;
        if (input_definition &&
            FAILED(input_definition->SpawnInstance(0, &input))) {
            return false;
        }
        if (!argument_name.empty()) {
            if (!input) {
                return false;
            }
            // Microsoft WMI CIM uint32 maps to Automation VT_I4 (4-byte signed integer):
            // https://learn.microsoft.com/en-us/windows/win32/wmisdk/numbers
            // Explicitly range-check against INT32_MAX to prevent signed overflow wrapping.
            if (argument_value >
                static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
                return false;
            }
            Bstr property(argument_name);
            VARIANT variant;
            VariantInit(&variant);
            variant.vt = VT_I4;
            variant.lVal = static_cast<LONG>(argument_value);
            const HRESULT put = property.get() == nullptr
                ? E_OUTOFMEMORY
                : input->Put(property.get(), 0, &variant, 0);
            VariantClear(&variant);
            if (FAILED(put)) {
                return false;
            }
        }
        Bstr path(object_path);
        ComPtr<IWbemClassObject> output;
        if (path.get() == nullptr ||
            FAILED(services_->ExecMethod(path.get(), method_name.get(), 0,
                                         nullptr, input.Get(), &output,
                                         nullptr)) ||
            !output) {
            return false;
        }
        std::uint32_t returned = 0;
        if (!variant_uint32(output.Get(), L"ReturnValue", returned)) {
            return false;
        }
        if (return_value != nullptr) {
            *return_value = returned;
        }
        return returned == 0u;
    }

private:
    ComPtr<IWbemServices> services_;
};

std::uint32_t product_type() {
    DWORD product = 0;
    return GetProductInfo(10, 0, 0, 0, &product) ? product : 0;
}

bool feature_state(bool& known, bool& enabled) {
    known = false;
    enabled = false;
    ComPtr<IWbemLocator> locator;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&locator)))) {
        return false;
    }
    Bstr ns(L"ROOT\\cimv2");
    ComPtr<IWbemServices> services;
    if (ns.get() == nullptr ||
        FAILED(locator->ConnectServer(ns.get(), nullptr, nullptr, nullptr,
                                      WBEM_FLAG_CONNECT_USE_MAX_WAIT, nullptr,
                                      nullptr, &services)) ||
        FAILED(CoSetProxyBlanket(
            services.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
            RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr,
            EOAC_NONE))) {
        return false;
    }
    Bstr language(L"WQL");
    Bstr query(L"SELECT InstallState FROM Win32_OptionalFeature WHERE "
               L"Name='Client-UnifiedWriteFilter'");
    ComPtr<IEnumWbemClassObject> enumerator;
    if (language.get() == nullptr || query.get() == nullptr ||
        FAILED(services->ExecQuery(
            language.get(), query.get(),
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr,
            &enumerator))) {
        return false;
    }
    ComPtr<IWbemClassObject> object;
    ULONG returned = 0;
    const HRESULT status =
        enumerator->Next(3000, 1, object.GetAddressOf(), &returned);
    if (status == WBEM_S_FALSE && returned == 0u) {
        known = true;
        return true;
    }
    std::uint32_t install_state = 0;
    if (status != WBEM_S_NO_ERROR || returned != 1u || !object ||
        !variant_uint32(object.Get(), L"InstallState", install_state)) {
        return false;
    }
    known = true;
    enabled = install_state == 1u;
    return true;
}

std::optional<std::wstring> volume_exclusion(
    const std::filesystem::path& data_root) {
    if (!data_root.is_absolute() || !data_root.has_root_name() ||
        data_root == data_root.root_path()) {
        return std::nullopt;
    }
    // UWF_Volume is addressed by a local drive letter. Refuse UNC, removable,
    // network, and synthetic roots rather than configuring a different volume
    // than the path will actually use after restart.
    const auto root = data_root.root_path();
    const auto drive = root.root_name().wstring();
    if (drive.size() != 2u || drive[1] != L':' ||
        !((drive[0] >= L'A' && drive[0] <= L'Z') ||
          (drive[0] >= L'a' && drive[0] <= L'z')) ||
        GetDriveTypeW(root.c_str()) != DRIVE_FIXED) {
        return std::nullopt;
    }
    const auto normalized = data_root.lexically_normal();
    const auto relative = normalized.lexically_relative(root);
    if (relative.empty()) {
        return std::nullopt;
    }
    for (const auto& component : relative) {
        if (component == L"..") {
            return std::nullopt;
        }
    }
    std::error_code error;
    if (!std::filesystem::is_directory(normalized, error) || error) {
        return std::nullopt;
    }
    return L"\\" + relative.wstring();
}

bool access_denied(std::uint32_t status) noexcept {
    return status == ERROR_ACCESS_DENIED ||
           status == static_cast<std::uint32_t>(
                         HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)) ||
           status == static_cast<std::uint32_t>(WBEM_E_ACCESS_DENIED);
}

UwfConfigureOutcome outcome_for(UwfEnableGate gate) noexcept {
    switch (gate) {
    case UwfEnableGate::ready: return UwfConfigureOutcome::failed;
    case UwfEnableGate::already_enabled: return UwfConfigureOutcome::already_enabled;
    case UwfEnableGate::unsupported_edition: return UwfConfigureOutcome::unsupported_edition;
    case UwfEnableGate::feature_missing: return UwfConfigureOutcome::feature_missing;
    case UwfEnableGate::probe_unavailable: return UwfConfigureOutcome::probe_unavailable;
    case UwfEnableGate::provider_unavailable: return UwfConfigureOutcome::provider_unavailable;
    case UwfEnableGate::reboot_pending: return UwfConfigureOutcome::reboot_pending;
    case UwfEnableGate::invalid_data_root: return UwfConfigureOutcome::invalid_data_root;
    case UwfEnableGate::readiness_failed: return UwfConfigureOutcome::readiness_failed;
    case UwfEnableGate::checkpoint_required: return UwfConfigureOutcome::checkpoint_required;
    }
    return UwfConfigureOutcome::failed;
}

const char* detail_for(UwfEnableGate gate) noexcept {
    switch (gate) {
    case UwfEnableGate::ready: return "UWF is ready to configure";
    case UwfEnableGate::already_enabled: return "UWF is already enabled";
    case UwfEnableGate::unsupported_edition: return "this Windows edition does not support UWF";
    case UwfEnableGate::feature_missing: return "the Unified Write Filter optional feature is not enabled";
    case UwfEnableGate::probe_unavailable: return "Windows edition or feature state could not be verified";
    case UwfEnableGate::provider_unavailable: return "the UWF WMI provider is unavailable";
    case UwfEnableGate::reboot_pending: return "a UWF state change is already waiting for a reboot";
    case UwfEnableGate::invalid_data_root: return "the NSTU data root is not a safe local path";
    case UwfEnableGate::readiness_failed: return "client diagnostics must pass before UWF is enabled";
    case UwfEnableGate::checkpoint_required: return "acknowledge the recovery-checkpoint reminder first";
    }
    return "UWF readiness failed";
}

} // namespace

bool is_uwf_supported_product(std::uint32_t product) noexcept {
    switch (product) {
    case kProductEnterprise:
    case kProductEnterpriseN:
    case kProductEnterpriseS:
    case kProductEnterpriseSN:
    case kProductEducation:
    case kProductEducationN:
    case kProductIotEnterprise:
    case kProductIotEnterpriseS:
        return true;
    default:
        return false;
    }
}

UwfEnableGate evaluate_uwf_enable(
    const UwfEnableReadiness& readiness) noexcept {
    if (readiness.product_type == 0u || !readiness.feature_known) {
        return UwfEnableGate::probe_unavailable;
    }
    if (!is_uwf_supported_product(readiness.product_type)) {
        return UwfEnableGate::unsupported_edition;
    }
    if (!readiness.feature_enabled) {
        return UwfEnableGate::feature_missing;
    }
    if (!readiness.provider_available || !readiness.filter_state_known) {
        return UwfEnableGate::provider_unavailable;
    }
    if (readiness.current_enabled != readiness.next_enabled) {
        return UwfEnableGate::reboot_pending;
    }
    // Even an already-enabled filter may be missing NSTU's mandatory
    // persistence exclusions. Require every safety acknowledgement before the
    // writer is allowed to inspect and repair next-session configuration.
    if (!readiness.data_root_valid) {
        return UwfEnableGate::invalid_data_root;
    }
    if (!readiness.diagnostic_readiness_passed) {
        return UwfEnableGate::readiness_failed;
    }
    if (!readiness.checkpoint_acknowledged) {
        return UwfEnableGate::checkpoint_required;
    }
    return readiness.current_enabled ? UwfEnableGate::already_enabled
                                     : UwfEnableGate::ready;
}

UwfConfigureResult configure_uwf(const UwfConfigureRequest& request) {
    UwfConfigureResult result;
    const auto exclusion = volume_exclusion(request.data_root);
    UwfEnableReadiness readiness;
    readiness.product_type = product_type();
    readiness.diagnostic_readiness_passed =
        request.diagnostic_readiness_passed;
    readiness.checkpoint_acknowledged = request.checkpoint_acknowledged;
    readiness.data_root_valid = exclusion.has_value();

    ComApartment apartment;
    if (!apartment.ready()) {
        set_failure(result, UwfConfigureOutcome::probe_unavailable,
                    "COM could not be initialized for the UWF readiness probe");
        return result;
    }
    (void)feature_state(readiness.feature_known, readiness.feature_enabled);

    WmiWriter writer;
    ComPtr<IWbemClassObject> filter;
    readiness.provider_available = writer.connect();
    if (readiness.provider_available &&
        writer.query_one(L"SELECT CurrentEnabled, NextEnabled FROM UWF_Filter",
                         filter)) {
        readiness.filter_state_known =
            variant_bool(filter.Get(), L"CurrentEnabled",
                         readiness.current_enabled) &&
            variant_bool(filter.Get(), L"NextEnabled", readiness.next_enabled);
    }
    const auto gate = evaluate_uwf_enable(readiness);
    const bool filter_already_enabled =
        gate == UwfEnableGate::already_enabled;
    if (gate != UwfEnableGate::ready && !filter_already_enabled) {
        result.outcome = outcome_for(gate);
        result.reboot_required = gate == UwfEnableGate::reboot_pending;
        result.detail = detail_for(gate);
        return result;
    }

    const std::wstring drive = request.data_root.root_name().wstring();
    const std::wstring volume_query =
        L"SELECT * FROM UWF_Volume WHERE CurrentSession=FALSE AND "
        L"DriveLetter='" + drive + L"'";
    ComPtr<IWbemClassObject> volume;
    ComPtr<IWbemClassObject> registry_filter;
    if (!writer.query_one(volume_query.c_str(), volume) ||
        !writer.query_one(L"SELECT * FROM UWF_RegistryFilter WHERE "
                          L"CurrentSession=FALSE",
                          registry_filter)) {
        set_failure(result, UwfConfigureOutcome::provider_unavailable,
                    "the next-session UWF volume or registry filter was unavailable");
        return result;
    }

    bool volume_protected = false;
    if (!variant_bool(volume.Get(), L"Protected", volume_protected)) {
        set_failure(result, UwfConfigureOutcome::provider_unavailable,
                    "the next-session volume protection state was unreadable");
        return result;
    }
    bool file_exclusion_present = false;
    bool registry_exclusion_present = false;
    if (!writer.contains(volume.Get(), L"FileName", *exclusion,
                         file_exclusion_present) ||
        !writer.contains(registry_filter.Get(), L"RegistryKey",
                         kNstuRegistryExclusion,
                         registry_exclusion_present)) {
        set_failure(result, UwfConfigureOutcome::provider_unavailable,
                    "the next-session UWF exclusions could not be verified");
        return result;
    }

    std::uint32_t returned = 0;
    bool volume_protection_added = false;
    bool file_exclusion_added = false;
    bool registry_exclusion_added = false;
    const auto rollback = [&] {
        // Roll back only state this call created. Existing protection and
        // exclusions belong to the operator and are never removed here.
        if (registry_exclusion_added) {
            (void)writer.call(registry_filter.Get(), L"RemoveExclusion",
                              L"RegistryKey", kNstuRegistryExclusion);
        }
        if (file_exclusion_added) {
            (void)writer.call(volume.Get(), L"RemoveExclusion", L"FileName",
                              *exclusion);
        }
        if (volume_protection_added) {
            (void)writer.call(volume.Get(), L"Unprotect");
        }
    };

    // Protect() and all exclusion changes target the next session and require
    // a restart according to Microsoft's UWF provider reference.
    // Sources:
    // https://learn.microsoft.com/en-us/windows/configuration/unified-write-filter/uwf-volumeprotect
    // https://learn.microsoft.com/en-us/windows/configuration/unified-write-filter/uwf-volumeaddexclusion
    if (!volume_protected) {
        if (!writer.call(volume.Get(), L"Protect", {}, {}, &returned)) {
            set_failure(result,
                        access_denied(returned)
                            ? UwfConfigureOutcome::access_denied
                            : UwfConfigureOutcome::failed,
                        "the NSTU system volume could not be protected");
            return result;
        }
        volume_protection_added = true;
    }
    if (!file_exclusion_present) {
        if (!writer.call(volume.Get(), L"AddExclusion", L"FileName",
                         *exclusion, &returned)) {
            rollback();
            set_failure(result, UwfConfigureOutcome::failed,
                        "the NSTU data exclusion could not be added");
            return result;
        }
        file_exclusion_added = true;
    }
    if (!registry_exclusion_present) {
        if (!writer.call(registry_filter.Get(), L"AddExclusion",
                         L"RegistryKey", kNstuRegistryExclusion, &returned)) {
            rollback();
            set_failure(result, UwfConfigureOutcome::failed,
                        "the NSTU registry exclusion could not be added");
            return result;
        }
        registry_exclusion_added = true;
    }
    // Enable() arms UWF for next restart and requires elevation.
    // Source: https://learn.microsoft.com/en-us/windows/configuration/unified-write-filter/uwf-filterenable
    if (!filter_already_enabled &&
        !writer.call(filter.Get(), L"Enable", {}, {}, &returned)) {
        rollback();
        set_failure(result,
                    access_denied(returned)
                        ? UwfConfigureOutcome::access_denied
                        : UwfConfigureOutcome::failed,
                    "UWF could not be armed for the next boot");
        return result;
    }

    const bool changed = volume_protection_added || file_exclusion_added ||
                         registry_exclusion_added || !filter_already_enabled;
    result.outcome = filter_already_enabled
        ? UwfConfigureOutcome::already_enabled
        : UwfConfigureOutcome::armed;
    result.reboot_required = changed;
    result.data_exclusion_added =
        file_exclusion_present || file_exclusion_added;
    result.registry_exclusion_added =
        registry_exclusion_present || registry_exclusion_added;
    result.detail = changed
        ? "UWF configuration is ready; save work and reboot to apply it"
        : "UWF and the mandatory NSTU exclusions are already active";
    return result;
}

namespace {

// The probe validates the drive-letter shape only.  Unlike volume_exclusion it
// does not require the directory to exist: a machine can be genuinely
// protected while its data root is momentarily absent, and reporting that as
// "unknown" would stall the whole fleet for no security benefit.
std::optional<std::wstring> probe_drive_letter(
    const std::filesystem::path& data_root) {
    if (!data_root.is_absolute() || !data_root.has_root_name()) {
        return std::nullopt;
    }
    const auto drive = data_root.root_name().wstring();
    if (drive.size() != 2 || drive[1] != L':' ||
        !((drive[0] >= L'A' && drive[0] <= L'Z') ||
          (drive[0] >= L'a' && drive[0] <= L'z'))) {
        return std::nullopt;
    }
    return drive;
}

} // namespace

UwfProtectionProbeResult probe_uwf_protection(
    const std::filesystem::path& data_root) {
    UwfProtectionProbeResult result;

    ComApartment apartment;
    if (!apartment.ready()) {
        result.detail = "COM could not be initialized for the UWF probe";
        return result;
    }

    const auto product = product_type();
    if (product == 0u) {
        result.detail = "the Windows edition could not be determined";
        return result;
    }
    result.supported_product = is_uwf_supported_product(product);
    if (!result.supported_product) {
        // A definite answer rather than a failure: this edition cannot run UWF
        // at all, so it is certainly not protected.
        result.probe_succeeded = true;
        result.detail = "this Windows edition does not support UWF";
        return result;
    }

    bool feature_known = false;
    bool feature_enabled = false;
    if (!feature_state(feature_known, feature_enabled) || !feature_known) {
        result.detail = "the UWF optional feature state was unreadable";
        return result;
    }
    if (!feature_enabled) {
        result.probe_succeeded = true;
        result.detail = "the UWF optional feature is not installed";
        return result;
    }

    // WmiWriter is reused for its connection and query plumbing.  Nothing
    // below mutates: only ExecQuery and the provider's FindExclusion lookups.
    WmiWriter provider;
    ComPtr<IWbemClassObject> filter;
    if (!provider.connect() ||
        !provider.query_one(
            L"SELECT CurrentEnabled, NextEnabled FROM UWF_Filter", filter) ||
        !variant_bool(filter.Get(), L"CurrentEnabled",
                      result.filter_current_enabled) ||
        !variant_bool(filter.Get(), L"NextEnabled",
                      result.filter_next_enabled)) {
        result.detail = "the UWF filter state was unreadable";
        return result;
    }

    // Past this point the probe has reached the provider and has an answer.
    // Anything it then fails to read about the volume or the exclusions is
    // reported as unprotected rather than unknown, because an unreadable
    // volume must not be able to authorize an exam.
    result.probe_succeeded = true;

    const auto drive = probe_drive_letter(data_root);
    if (!drive) {
        result.detail = "the NSTU data root is not on a local drive letter";
        return result;
    }

    // CurrentSession=TRUE is the entire point of this query.  The FALSE rows
    // describe what the next boot is configured to do and prove nothing about
    // the session running now.
    const std::wstring volume_query =
        L"SELECT * FROM UWF_Volume WHERE CurrentSession=TRUE AND "
        L"DriveLetter='" + *drive + L"'";
    ComPtr<IWbemClassObject> volume;
    if (!provider.query_one(volume_query.c_str(), volume) ||
        !variant_bool(volume.Get(), L"Protected",
                      result.system_volume_current_protected)) {
        result.system_volume_current_protected = false;
        result.detail =
            "the current-session volume protection state was unreadable";
        return result;
    }

    // Exclusions are reported for the operator's benefit.  A missing exclusion
    // is a configuration warning, not a protection failure, so a failed lookup
    // here leaves the flag false and does not disturb the verdict above.
    if (const auto exclusion = volume_exclusion(data_root); exclusion) {
        (void)provider.contains(volume.Get(), L"FileName", *exclusion,
                                result.data_exclusion_present);
    }
    ComPtr<IWbemClassObject> registry_filter;
    if (provider.query_one(L"SELECT * FROM UWF_RegistryFilter WHERE "
                           L"CurrentSession=TRUE",
                           registry_filter)) {
        (void)provider.contains(registry_filter.Get(), L"RegistryKey",
                                kNstuRegistryExclusion,
                                result.registry_exclusion_present);
    }

    if (!result.system_volume_current_protected) {
        result.detail = "the NSTU volume is not protected in this session";
    } else if (!result.filter_current_enabled) {
        result.detail = "the volume is marked protected but UWF is disabled";
    } else {
        result.detail = "UWF is protecting the NSTU volume in this session";
    }
    return result;
}

namespace {

class ProductionUwfWmi final : public UwfWmi {
public:
    bool connect() { return writer_.connect(); }

    bool read_filter(UwfFilterState& state) override {
        ComPtr<IWbemClassObject> filter;
        if (!writer_.query_one(
                L"SELECT CurrentEnabled, NextEnabled FROM UWF_Filter", filter)) {
            return false;
        }
        return variant_bool(filter.Get(), L"CurrentEnabled", state.current_enabled) &&
               variant_bool(filter.Get(), L"NextEnabled", state.next_enabled);
    }

    bool read_overlay_config(bool current, UwfOverlayConfig& config) override {
        const std::wstring query = current
            ? L"SELECT * FROM UWF_OverlayConfig WHERE CurrentSession=TRUE"
            : L"SELECT * FROM UWF_OverlayConfig WHERE CurrentSession=FALSE";
        ComPtr<IWbemClassObject> obj;
        if (!writer_.query_one(query.c_str(), obj)) {
            return false;
        }
        std::uint32_t type_val = 0;
        if (!variant_uint32(obj.Get(), L"Type", type_val) ||
            !variant_uint32(obj.Get(), L"MaximumSize", config.maximum_size_mb)) {
            return false;
        }
        if (type_val != static_cast<std::uint32_t>(UwfOverlayType::ram) &&
            type_val != static_cast<std::uint32_t>(UwfOverlayType::disk)) {
            return false;
        }
        config.type = static_cast<UwfOverlayType>(type_val);
        return true;
    }

    bool read_overlay(UwfOverlayUsage& usage) override {
        ComPtr<IWbemClassObject> obj;
        if (!writer_.query_one(L"SELECT * FROM UWF_Overlay", obj)) {
            return false;
        }
        return variant_uint32(obj.Get(), L"OverlayConsumption", usage.consumption_mb) &&
               variant_uint32(obj.Get(), L"AvailableSpace", usage.available_mb) &&
               variant_uint32(obj.Get(), L"WarningOverlayThreshold", usage.warning_mb) &&
               variant_uint32(obj.Get(), L"CriticalOverlayThreshold", usage.critical_mb);
    }

    bool read_servicing(bool current, bool& enabled) override {
        const std::wstring query = current
            ? L"SELECT * FROM UWF_Servicing WHERE CurrentSession=TRUE"
            : L"SELECT * FROM UWF_Servicing WHERE CurrentSession=FALSE";
        ComPtr<IWbemClassObject> obj;
        if (!writer_.query_one(query.c_str(), obj)) {
            return false;
        }
        return variant_bool(obj.Get(), L"ServicingEnabled", enabled);
    }

    bool set_overlay_type(UwfOverlayType type) override {
        ComPtr<IWbemClassObject> obj;
        if (!writer_.query_one(
                L"SELECT * FROM UWF_OverlayConfig WHERE CurrentSession=FALSE", obj)) {
            return false;
        }
        return writer_.call_uint32(obj.Get(), L"SetType", L"Type",
                                   static_cast<std::uint32_t>(type));
    }

    bool set_overlay_maximum_size(std::uint32_t size_mb) override {
        ComPtr<IWbemClassObject> obj;
        if (!writer_.query_one(
                L"SELECT * FROM UWF_OverlayConfig WHERE CurrentSession=FALSE", obj)) {
            return false;
        }
        return writer_.call_uint32(obj.Get(), L"SetMaximumSize", L"size", size_mb);
    }

    bool set_warning_threshold(std::uint32_t size_mb) override {
        ComPtr<IWbemClassObject> obj;
        if (!writer_.query_one(L"SELECT * FROM UWF_Overlay", obj)) {
            return false;
        }
        return writer_.call_uint32(obj.Get(), L"SetWarningThreshold", L"size", size_mb);
    }

    bool set_critical_threshold(std::uint32_t size_mb) override {
        ComPtr<IWbemClassObject> obj;
        if (!writer_.query_one(L"SELECT * FROM UWF_Overlay", obj)) {
            return false;
        }
        return writer_.call_uint32(obj.Get(), L"SetCriticalThreshold", L"size", size_mb);
    }

    bool set_servicing(bool enabled) override {
        ComPtr<IWbemClassObject> obj;
        if (!writer_.query_one(
                L"SELECT * FROM UWF_Servicing WHERE CurrentSession=FALSE", obj)) {
            return false;
        }
        return writer_.call(obj.Get(), enabled ? L"Enable" : L"Disable");
    }

    bool set_filter_enabled(bool enabled) override {
        ComPtr<IWbemClassObject> obj;
        if (!writer_.query_one(L"SELECT * FROM UWF_Filter", obj)) {
            return false;
        }
        return writer_.call(obj.Get(), enabled ? L"Enable" : L"Disable");
    }

    bool find_file_exclusion(std::wstring_view path, bool& found) override {
        found = false;
        ComPtr<IWbemClassObject> volume;
        if (!system_volume(volume)) {
            return false;
        }
        return writer_.contains(volume.Get(), L"FileName", path, found);
    }

    bool add_file_exclusion(std::wstring_view path) override {
        ComPtr<IWbemClassObject> volume;
        if (!system_volume(volume)) {
            return false;
        }
        return writer_.call(volume.Get(), L"AddExclusion", L"FileName", path);
    }

    bool remove_file_exclusion(std::wstring_view path) override {
        ComPtr<IWbemClassObject> volume;
        if (!system_volume(volume)) {
            return false;
        }
        return writer_.call(volume.Get(), L"RemoveExclusion", L"FileName", path);
    }

private:
    bool system_volume(ComPtr<IWbemClassObject>& volume) {
        wchar_t win_dir[MAX_PATH] = {};
        if (GetSystemWindowsDirectoryW(win_dir, MAX_PATH) == 0 || win_dir[0] == L'\0') {
            return false;
        }
        const std::wstring drive{win_dir[0], L':'};
        const std::wstring query =
            L"SELECT * FROM UWF_Volume WHERE CurrentSession=FALSE AND DriveLetter='" +
            drive + L"'";
        return writer_.query_one(query.c_str(), volume);
    }

    WmiWriter writer_;
};

// Microsoft UWF Antimalware Support documentation:
// https://learn.microsoft.com/en-us/windows-hardware/customize/enterprise/uwf-antimalware-support
// Excluded paths persist across reboot-to-restore.
constexpr std::wstring_view kDefenderExclusionPaths[] = {
    L"\\Program Files\\Windows Defender",
    L"\\ProgramData\\Microsoft\\Windows Defender",
    L"\\Windows\\WindowsUpdate.log",
    L"\\Windows\\Temp\\MpCmdRun.log",
};

} // namespace

// MaximumSize sizing formula and clamps:
// For RAM overlay (type == UwfOverlayType::ram):
//   Requires at least 4096 MiB physical RAM.
//   Formula: clamp(physical_ram_mb / 4, 1024, 4096) MiB.
//   Warning threshold: 75% of maximum size ((max * 75) / 100).
//   Critical threshold: 90% of maximum size ((max * 90) / 100).
// For Disk overlay (type == UwfOverlayType::disk):
//   Requires at least 2048 MiB free disk space on the protected volume.
//   Formula: clamp(free_disk_mb / 4, 1024, 8192) MiB.
//   Warning threshold: 75% of maximum size ((max * 75) / 100).
//   Critical threshold: 90% of maximum size ((max * 90) / 100).
std::optional<std::uint32_t> uwf_maximum_size_mb(
    UwfOverlayType type, std::uint64_t physical_ram_mb,
    std::uint64_t free_disk_mb) noexcept {
    if (type == UwfOverlayType::ram) {
        if (physical_ram_mb < 4096u) {
            return std::nullopt;
        }
        const std::uint64_t size = std::clamp<std::uint64_t>(
            physical_ram_mb / 4u, 1024u, 4096u);
        return static_cast<std::uint32_t>(size);
    }
    if (type == UwfOverlayType::disk) {
        if (free_disk_mb < 2048u) {
            return std::nullopt;
        }
        const std::uint64_t size = std::clamp<std::uint64_t>(
            free_disk_mb / 4u, 1024u, 8192u);
        return static_cast<std::uint32_t>(size);
    }
    return std::nullopt;
}

UwfOperationResult configure_uwf_overlay(
    UwfWmi& wmi, UwfOverlayType type, std::uint64_t physical_ram_mb,
    std::uint64_t free_disk_mb) {
    UwfFilterState filter_state;
    if (!wmi.read_filter(filter_state) || filter_state.current_enabled) {
        return {false, false, false, true};
    }
    const auto target_max_mb =
        uwf_maximum_size_mb(type, physical_ram_mb, free_disk_mb);
    if (!target_max_mb) {
        return {false, false, false, true};
    }

    const std::uint32_t target_warning = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(*target_max_mb) * 75ULL) / 100ULL);
    const std::uint32_t target_critical = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(*target_max_mb) * 90ULL) / 100ULL);

    UwfOverlayConfig original_config;
    if (!wmi.read_overlay_config(false, original_config)) {
        return {false, false, false, true};
    }
    UwfOverlayUsage original_usage;
    if (!wmi.read_overlay(original_usage)) {
        return {false, false, false, true};
    }

    const bool need_type = original_config.type != type;
    const bool need_size = original_config.maximum_size_mb != *target_max_mb;
    const bool need_warning = original_usage.warning_mb != target_warning;
    const bool need_critical = original_usage.critical_mb != target_critical;
    if (!need_type && !need_size && !need_warning && !need_critical) {
        return {true, false, false, true};
    }

    bool type_changed = false;
    bool size_changed = false;
    bool warning_changed = false;
    bool critical_changed = false;
    bool rollback_succeeded = true;

    const bool switching_to_disk = need_type && type == UwfOverlayType::disk;
    const bool raising_thresholds = need_critical
        ? (target_critical >= original_usage.critical_mb)
        : (target_warning >= original_usage.warning_mb);

    auto rollback = [&]() {
        // Rollback thresholds first (undoing threshold writes)
        // Microsoft UWF threshold documentation:
        // https://learn.microsoft.com/en-us/windows/configuration/unified-write-filter/uwf-overlaysetwarningthreshold
        // https://learn.microsoft.com/en-us/windows/configuration/unified-write-filter/uwf-overlaysetcriticalthreshold
        // Size 0 disables threshold notifications ("unset" state).
        // When restoring original unset (0/0) thresholds, lowering-rollback restores warning
        // to 0 first (strictly below the live critical threshold), then critical to 0 (disabling it).
        if (raising_thresholds) {
            // Forward was raising, so rollback is LOWERING: write warning first, then critical.
            if (warning_changed) {
                if (!wmi.set_warning_threshold(original_usage.warning_mb)) {
                    rollback_succeeded = false;
                }
            }
            if (critical_changed) {
                if (!wmi.set_critical_threshold(original_usage.critical_mb)) {
                    rollback_succeeded = false;
                }
            }
        } else {
            // Forward was lowering, so rollback is RAISING: write critical first, then warning.
            if (critical_changed) {
                if (!wmi.set_critical_threshold(original_usage.critical_mb)) {
                    rollback_succeeded = false;
                }
            }
            if (warning_changed) {
                if (!wmi.set_warning_threshold(original_usage.warning_mb)) {
                    rollback_succeeded = false;
                }
            }
        }

        // Rollback type and size
        if (switching_to_disk) {
            // Forward wrote size then type, so rollback restores type then size.
            if (type_changed) {
                if (!wmi.set_overlay_type(original_config.type)) {
                    rollback_succeeded = false;
                }
            }
            if (size_changed) {
                if (!wmi.set_overlay_maximum_size(original_config.maximum_size_mb)) {
                    rollback_succeeded = false;
                }
            }
        } else {
            // Forward wrote type then size, so rollback restores size then type.
            if (size_changed) {
                if (!wmi.set_overlay_maximum_size(original_config.maximum_size_mb)) {
                    rollback_succeeded = false;
                }
            }
            if (type_changed) {
                if (!wmi.set_overlay_type(original_config.type)) {
                    rollback_succeeded = false;
                }
            }
        }
    };

    auto make_failure_result = [&]() -> UwfOperationResult {
        rollback();
        const bool state_changed = !rollback_succeeded;
        return {false, state_changed, state_changed, rollback_succeeded};
    };

    if (switching_to_disk) {
        if (need_size) {
            if (!wmi.set_overlay_maximum_size(*target_max_mb)) {
                return make_failure_result();
            }
            size_changed = true;
        }
        if (need_type) {
            if (!wmi.set_overlay_type(type)) {
                return make_failure_result();
            }
            type_changed = true;
        }
    } else {
        if (need_type) {
            if (!wmi.set_overlay_type(type)) {
                return make_failure_result();
            }
            type_changed = true;
        }
        if (need_size) {
            if (!wmi.set_overlay_maximum_size(*target_max_mb)) {
                return make_failure_result();
            }
            size_changed = true;
        }
    }

    if (raising_thresholds) {
        if (need_critical) {
            if (!wmi.set_critical_threshold(target_critical)) {
                return make_failure_result();
            }
            critical_changed = true;
        }
        if (need_warning) {
            if (!wmi.set_warning_threshold(target_warning)) {
                return make_failure_result();
            }
            warning_changed = true;
        }
    } else {
        if (need_warning) {
            if (!wmi.set_warning_threshold(target_warning)) {
                return make_failure_result();
            }
            warning_changed = true;
        }
        if (need_critical) {
            if (!wmi.set_critical_threshold(target_critical)) {
                return make_failure_result();
            }
            critical_changed = true;
        }
    }

    return {true, true, true, true};
}

std::optional<UwfOverlayUsage> probe_uwf_overlay(UwfWmi& wmi) {
    UwfOverlayUsage usage;
    if (!wmi.read_overlay(usage)) {
        return std::nullopt;
    }
    return usage;
}

UwfOperationResult set_uwf_servicing(UwfWmi& wmi, bool enabled) {
    bool current_servicing = false;
    bool next_servicing = false;
    if (!wmi.read_servicing(true, current_servicing) ||
        !wmi.read_servicing(false, next_servicing)) {
        return {false, false, false, true};
    }
    if (current_servicing && enabled) {
        return {false, false, false, true};
    }
    if (next_servicing == enabled) {
        return {true, false, false, true};
    }
    if (!wmi.set_servicing(enabled)) {
        return {false, false, false, true};
    }
    return {true, true, true, true};
}

UwfOperationResult disable_uwf(UwfWmi& wmi) {
    UwfFilterState state;
    if (!wmi.read_filter(state)) {
        return {false, false, false, true};
    }
    if (!state.current_enabled && state.next_enabled) {
        return {false, false, false, true};
    }
    if (!state.next_enabled) {
        return {true, false, false, true};
    }
    if (!wmi.set_filter_enabled(false)) {
        return {false, false, false, true};
    }
    return {true, true, true, true};
}

UwfOperationResult add_uwf_defender_exclusions(UwfWmi& wmi) {
    std::vector<std::wstring_view> to_add;
    to_add.reserve(std::size(kDefenderExclusionPaths));

    for (const auto path : kDefenderExclusionPaths) {
        bool found = false;
        if (!wmi.find_file_exclusion(path, found)) {
            return {false, false, false, true};
        }
        if (!found) {
            to_add.push_back(path);
        }
    }

    if (to_add.empty()) {
        return {true, false, false, true};
    }

    std::vector<std::wstring_view> added;
    added.reserve(to_add.size());
    bool rollback_succeeded = true;

    for (const auto path : to_add) {
        if (!wmi.add_file_exclusion(path)) {
            for (auto it = added.rbegin(); it != added.rend(); ++it) {
                if (!wmi.remove_file_exclusion(*it)) {
                    rollback_succeeded = false;
                }
            }
            const bool state_changed = !rollback_succeeded;
            return {false, state_changed, state_changed, rollback_succeeded};
        }
        added.push_back(path);
    }

    return {true, true, true, true};
}

UwfOperationResult configure_uwf_overlay(
    UwfOverlayType type, std::uint64_t physical_ram_mb,
    std::uint64_t free_disk_mb) {
    ComApartment apartment;
    if (!apartment.ready()) {
        return {false, false, false, true};
    }
    ProductionUwfWmi wmi;
    if (!wmi.connect()) {
        return {false, false, false, true};
    }
    return configure_uwf_overlay(wmi, type, physical_ram_mb, free_disk_mb);
}

std::optional<UwfOverlayUsage> probe_uwf_overlay() {
    ComApartment apartment;
    if (!apartment.ready()) {
        return std::nullopt;
    }
    ProductionUwfWmi wmi;
    if (!wmi.connect()) {
        return std::nullopt;
    }
    return probe_uwf_overlay(wmi);
}

UwfOperationResult set_uwf_servicing(bool enabled) {
    ComApartment apartment;
    if (!apartment.ready()) {
        return {false, false, false, true};
    }
    ProductionUwfWmi wmi;
    if (!wmi.connect()) {
        return {false, false, false, true};
    }
    return set_uwf_servicing(wmi, enabled);
}

UwfOperationResult disable_uwf() {
    ComApartment apartment;
    if (!apartment.ready()) {
        return {false, false, false, true};
    }
    ProductionUwfWmi wmi;
    if (!wmi.connect()) {
        return {false, false, false, true};
    }
    return disable_uwf(wmi);
}

UwfOperationResult add_uwf_defender_exclusions() {
    ComApartment apartment;
    if (!apartment.ready()) {
        return {false, false, false, true};
    }
    ProductionUwfWmi wmi;
    if (!wmi.connect()) {
        return {false, false, false, true};
    }
    return add_uwf_defender_exclusions(wmi);
}

} // namespace nstu::setup
