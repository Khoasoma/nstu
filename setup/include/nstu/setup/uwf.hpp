#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace nstu::setup {

// Inputs kept separate from WMI so the safety gate is deterministic and can be
// tested without enabling a Windows feature on the test machine.
struct UwfEnableReadiness {
    std::uint32_t product_type = 0;
    bool feature_known = false;
    bool feature_enabled = false;
    bool provider_available = false;
    bool filter_state_known = false;
    bool current_enabled = false;
    bool next_enabled = false;
    bool data_root_valid = false;
    bool diagnostic_readiness_passed = false;
    bool checkpoint_acknowledged = false;
};

enum class UwfEnableGate : std::uint8_t {
    ready,
    already_enabled,
    unsupported_edition,
    feature_missing,
    probe_unavailable,
    provider_unavailable,
    reboot_pending,
    invalid_data_root,
    readiness_failed,
    checkpoint_required,
};

[[nodiscard]] bool is_uwf_supported_product(
    std::uint32_t product_type) noexcept;
[[nodiscard]] UwfEnableGate evaluate_uwf_enable(
    const UwfEnableReadiness& readiness) noexcept;

struct UwfConfigureRequest {
    std::filesystem::path data_root;
    // The caller must have shown the recovery-checkpoint reminder and received
    // an explicit acknowledgement. This library does not manufacture or
    // silently assume a restore point.
    bool diagnostic_readiness_passed = false;
    bool checkpoint_acknowledged = false;
};

enum class UwfConfigureOutcome : std::uint8_t {
    armed,
    already_enabled,
    unsupported_edition,
    feature_missing,
    probe_unavailable,
    provider_unavailable,
    reboot_pending,
    invalid_data_root,
    readiness_failed,
    checkpoint_required,
    access_denied,
    failed,
};

struct UwfConfigureResult {
    UwfConfigureOutcome outcome = UwfConfigureOutcome::failed;
    bool reboot_required = false;
    bool data_exclusion_added = false;
    bool registry_exclusion_added = false;
    std::string detail;
};

// Arms UWF for the next boot only after adding the whole NSTU data root and
// HKLM\Software\NSTU as exclusions. On any pre-enable failure it attempts to
// unprotect the volume again; it never schedules or forces a reboot itself.
[[nodiscard]] UwfConfigureResult configure_uwf(
    const UwfConfigureRequest& request);

// A strictly read-only observation of what UWF is doing in the session that is
// running right now.  Deliberately separate from configure_uwf: asking "is
// this machine protected?" must never be able to change whether it is, and the
// fleet workflow asks it on every reconnect.
struct UwfProtectionProbeResult {
    // False means the probe could not reach an answer at all.  Every other
    // field is then meaningless and must not be read as a negative
    // observation - "unknown" and "unprotected" are different states.
    bool probe_succeeded = false;
    bool supported_product = false;
    // Current-session state is proof of protection.  Next-session state is
    // only intent that a restart has yet to apply, which is exactly the
    // distinction the pre-reboot configure report cannot make.
    bool filter_current_enabled = false;
    bool filter_next_enabled = false;
    bool system_volume_current_protected = false;
    bool data_exclusion_present = false;
    bool registry_exclusion_present = false;
    // Short printable ASCII, safe to forward over the control channel.
    std::string detail;
};

// Reads live UWF state for the volume hosting `data_root`.  Issues WQL queries
// and the provider's Find* lookups only; it never protects, unprotects, or
// changes an exclusion.  Anything it can reach but not read is reported as
// unprotected rather than unknown, so the exam gate fails closed.
[[nodiscard]] UwfProtectionProbeResult probe_uwf_protection(
    const std::filesystem::path& data_root);

// UWF_OverlayConfig and UWF_Servicing each have distinct current/next rows.
// Only next-session rows may be changed; UWF_Overlay is a single live row.
enum class UwfOverlayType : std::uint32_t { ram = 0, disk = 1 };
struct UwfOverlayConfig {
    UwfOverlayType type = UwfOverlayType::ram;
    std::uint32_t maximum_size_mb = 0;
};
struct UwfOverlayUsage {
    std::uint32_t consumption_mb = 0;
    std::uint32_t available_mb = 0;
    std::uint32_t warning_mb = 0;
    std::uint32_t critical_mb = 0;
};
struct UwfFilterState {
    bool current_enabled = false;
    bool next_enabled = false;
};

// This seam is deliberately typed: tests supply a fake; the production adapter
// issues only the named UWF provider methods. Every write targets a next-boot
// setting except the provider's global warning/critical thresholds.
class UwfWmi {
public:
    virtual ~UwfWmi() = default;
    virtual bool read_filter(UwfFilterState& state) = 0;
    virtual bool read_overlay_config(bool current, UwfOverlayConfig& config) = 0;
    virtual bool read_overlay(UwfOverlayUsage& usage) = 0;
    virtual bool read_servicing(bool current, bool& enabled) = 0;
    virtual bool set_overlay_type(UwfOverlayType type) = 0;
    virtual bool set_overlay_maximum_size(std::uint32_t size_mb) = 0;
    virtual bool set_warning_threshold(std::uint32_t size_mb) = 0;
    virtual bool set_critical_threshold(std::uint32_t size_mb) = 0;
    virtual bool set_servicing(bool enabled) = 0;
    virtual bool set_filter_enabled(bool enabled) = 0;
    virtual bool find_file_exclusion(std::wstring_view path, bool& found) = 0;
    virtual bool add_file_exclusion(std::wstring_view path) = 0;
    virtual bool remove_file_exclusion(std::wstring_view path) = 0;
};

struct UwfOperationResult {
    bool succeeded = false;
    bool changed = false;
    bool reboot_required = false;
    // False means a prior successful write could not be restored. No automatic
    // restart is safe until an operator has reconciled next-session state.
    bool rollback_succeeded = true;
};

// Inputs are MiB, measured before the call; zero/insufficient capacity rejects
// the request. Disk sizing uses available bytes on the protected system volume.
[[nodiscard]] std::optional<std::uint32_t> uwf_maximum_size_mb(
    UwfOverlayType type, std::uint64_t physical_ram_mb,
    std::uint64_t free_disk_mb) noexcept;
[[nodiscard]] UwfOperationResult configure_uwf_overlay(
    UwfWmi& wmi, UwfOverlayType type, std::uint64_t physical_ram_mb,
    std::uint64_t free_disk_mb);
[[nodiscard]] std::optional<UwfOverlayUsage> probe_uwf_overlay(UwfWmi& wmi);
[[nodiscard]] UwfOperationResult set_uwf_servicing(UwfWmi& wmi, bool enabled);
// Controlled decommission: schedule filter disable only. Never remove operator
// exclusions/protection or uninstall the optional Windows feature.
[[nodiscard]] UwfOperationResult disable_uwf(UwfWmi& wmi);
// Human sign-off required before any default-on use (installer page or service).
// Explicit opt-in only; UWF exclusions are NOT Defender Antivirus scan
// exclusions. The four Microsoft-listed paths alone are persisted by UWF.
// Caveat: "If a Windows IoT Enterprise computer stops responding during Windows startup, see https://learn.microsoft.com/en-us/troubleshoot/windows-client/performance/windows-hangs-on-startup-after-excluding-uwf-from-microsoft-defender for a workaround." (https://learn.microsoft.com/en-us/windows-hardware/customize/enterprise/uwf-antimalware-support)
[[nodiscard]] UwfOperationResult add_uwf_defender_exclusions(UwfWmi& wmi);

// Production entry points establish COM/WMI without exposing COM to callers.
[[nodiscard]] UwfOperationResult configure_uwf_overlay(
    UwfOverlayType type, std::uint64_t physical_ram_mb,
    std::uint64_t free_disk_mb);
[[nodiscard]] std::optional<UwfOverlayUsage> probe_uwf_overlay();
[[nodiscard]] UwfOperationResult set_uwf_servicing(bool enabled);
[[nodiscard]] UwfOperationResult disable_uwf();
[[nodiscard]] UwfOperationResult add_uwf_defender_exclusions();

} // namespace nstu::setup
