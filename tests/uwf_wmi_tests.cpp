#include "nstu/setup/uwf.hpp"

#include <algorithm>
#include <cassert>
#include <limits>
#include <string>
#include <vector>

namespace {
using namespace nstu::setup;

std::string to_ascii_string(std::wstring_view wide) {
    std::string result;
    result.reserve(wide.size());
    for (const wchar_t ch : wide) {
        assert(static_cast<std::make_unsigned_t<wchar_t>>(ch) < 128u);
        result.push_back(static_cast<char>(ch));
    }
    return result;
}

struct FakeWmi final : UwfWmi {
    UwfFilterState filter{false, false};
    UwfOverlayConfig current{UwfOverlayType::ram, 1024u};
    UwfOverlayConfig next = current;
    UwfOverlayUsage usage{30u, 994u, 512u, 896u};
    bool current_servicing = false;
    bool next_servicing = false;
    std::vector<std::wstring> exclusions;
    bool readable = true;
    bool fail_read_overlay_config = false;
    bool fail_read_overlay = false;
    int fail_write = -1;
    std::vector<int> fail_write_steps;
    int write_count = 0;
    std::vector<std::string> writes;

    bool write(std::string what) {
        writes.push_back(std::move(what));
        ++write_count;
        if (write_count == fail_write) return false;
        if (std::find(fail_write_steps.begin(), fail_write_steps.end(), write_count) !=
            fail_write_steps.end()) {
            return false;
        }
        return true;
    }
    bool read_filter(UwfFilterState& output) override {
        if (!readable) return false;
        output = filter;
        return true;
    }
    bool read_overlay_config(bool session, UwfOverlayConfig& output) override {
        if (!readable || fail_read_overlay_config) return false;
        const auto& src = session ? current : next;
        if (src.type != UwfOverlayType::ram && src.type != UwfOverlayType::disk) {
            return false;
        }
        output = src;
        return true;
    }
    bool read_overlay(UwfOverlayUsage& output) override {
        if (!readable || fail_read_overlay) return false;
        output = usage;
        return true;
    }
    bool read_servicing(bool session, bool& output) override {
        if (!readable) return false;
        output = session ? current_servicing : next_servicing;
        return true;
    }
    bool set_overlay_type(UwfOverlayType value) override {
        // Microsoft WMI maps CIM uint32 to Automation VT_I4:
        // https://learn.microsoft.com/en-us/windows/win32/wmisdk/numbers
        assert(static_cast<std::uint32_t>(value) <=
               static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()));
        if (value == UwfOverlayType::disk && next.maximum_size_mb < 1024u) {
            return false;
        }
        if (!write("type")) return false;
        next.type = value;
        return true;
    }
    bool set_overlay_maximum_size(std::uint32_t value) override {
        // Microsoft WMI maps CIM uint32 to Automation VT_I4:
        // https://learn.microsoft.com/en-us/windows/win32/wmisdk/numbers
        assert(value <=
               static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()));
        if (next.type == UwfOverlayType::disk && value < 1024u) {
            return false;
        }
        if (!write("maximum")) return false;
        next.maximum_size_mb = value;
        return true;
    }
    bool set_warning_threshold(std::uint32_t value) override {
        // Microsoft WMI maps CIM uint32 to Automation VT_I4:
        // https://learn.microsoft.com/en-us/windows/win32/wmisdk/numbers
        assert(value <=
               static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()));
        // Microsoft UWF docs: size 0 disables warning events ("unset" state).
        // https://learn.microsoft.com/en-us/windows/configuration/unified-write-filter/uwf-overlaysetwarningthreshold
        if (usage.critical_mb != 0u && value >= usage.critical_mb) {
            return false;
        }
        if (usage.critical_mb == 0u && value != 0u) {
            return false;
        }
        if (!write("warning")) return false;
        usage.warning_mb = value;
        return true;
    }
    bool set_critical_threshold(std::uint32_t value) override {
        // Microsoft WMI maps CIM uint32 to Automation VT_I4:
        // https://learn.microsoft.com/en-us/windows/win32/wmisdk/numbers
        assert(value <=
               static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()));
        // Microsoft UWF docs: size 0 disables critical events ("unset" state).
        // https://learn.microsoft.com/en-us/windows/configuration/unified-write-filter/uwf-overlaysetcriticalthreshold
        // Value must be strictly higher than warning threshold unless 0 (disabled).
        if (value != 0u && value <= usage.warning_mb) {
            return false;
        }
        if (!write("critical")) return false;
        usage.critical_mb = value;
        return true;
    }
    bool set_servicing(bool value) override {
        if (!write("servicing")) return false;
        next_servicing = value;
        return true;
    }
    bool set_filter_enabled(bool value) override {
        if (!write("filter")) return false;
        filter.next_enabled = value;
        return true;
    }
    bool find_file_exclusion(std::wstring_view path, bool& found) override {
        if (!readable) return false;
        found = std::find(exclusions.begin(), exclusions.end(), path) != exclusions.end();
        return true;
    }
    bool add_file_exclusion(std::wstring_view path) override {
        writes.push_back(to_ascii_string(path));
        if (!write("file-add")) return false;
        exclusions.emplace_back(path);
        return true;
    }
    bool remove_file_exclusion(std::wstring_view path) override {
        writes.push_back(to_ascii_string(path));
        if (!write("file-remove")) return false;
        const auto it = std::find(exclusions.begin(), exclusions.end(), path);
        if (it != exclusions.end()) exclusions.erase(it);
        return true;
    }
};

void test_formula() {
    assert(!uwf_maximum_size_mb(UwfOverlayType::ram, 0u, 16000u));
    assert(!uwf_maximum_size_mb(UwfOverlayType::ram, 4095u, 16000u));
    assert(uwf_maximum_size_mb(UwfOverlayType::ram, 4096u, 0u) == 1024u);
    assert(uwf_maximum_size_mb(UwfOverlayType::ram, 8192u, 0u) == 2048u);
    assert(uwf_maximum_size_mb(UwfOverlayType::ram, UINT64_MAX, 0u) == 4096u);
    assert(!uwf_maximum_size_mb(UwfOverlayType::disk, 8000u, 2047u));
    assert(uwf_maximum_size_mb(UwfOverlayType::disk, 0u, 2048u) == 1024u);
    assert(uwf_maximum_size_mb(UwfOverlayType::disk, 0u, 16384u) == 4096u);
    assert(uwf_maximum_size_mb(UwfOverlayType::disk, 0u, UINT64_MAX) == 8192u);
    assert(!uwf_maximum_size_mb(static_cast<UwfOverlayType>(2), 8192u, 8192u));
}

void test_overlay() {
    FakeWmi wmi;
    const auto outcome = configure_uwf_overlay(wmi, UwfOverlayType::disk,
                                                8192u, 16384u);
    assert(outcome.succeeded && outcome.changed && outcome.reboot_required);
    assert(wmi.next.type == UwfOverlayType::disk);
    assert(wmi.next.maximum_size_mb == 4096u);
    assert(wmi.usage.warning_mb == 3072u && wmi.usage.critical_mb == 3686u);
    assert(wmi.writes == (std::vector<std::string>{"maximum", "type",
                                                    "critical", "warning"}));
    assert(configure_uwf_overlay(wmi, UwfOverlayType::disk, 8192u, 16384u)
               .succeeded);
    assert(wmi.writes.size() == 4u); // no writes on already-configured state
    for (int step = 1; step <= 4; ++step) {
        FakeWmi failed;
        failed.fail_write = step;
        auto result = configure_uwf_overlay(failed, UwfOverlayType::disk,
                                             8192u, 16384u);
        assert(!result.succeeded && !result.changed);
        assert(result.rollback_succeeded);
        assert(failed.next.type == UwfOverlayType::ram);
        assert(failed.next.maximum_size_mb == 1024u);
        assert(failed.usage.warning_mb == 512u);
        assert(failed.usage.critical_mb == 896u);
        assert(failed.writes.size() == static_cast<std::size_t>(2 * step - 1));
    }
    FakeWmi filter_enabled;
    filter_enabled.filter.current_enabled = true;
    assert(!configure_uwf_overlay(filter_enabled, UwfOverlayType::disk,
                                  8192, 16384).succeeded);
    assert(filter_enabled.writes.empty());
    FakeWmi read_cfg_failed;
    read_cfg_failed.fail_read_overlay_config = true;
    assert(!configure_uwf_overlay(read_cfg_failed, UwfOverlayType::disk,
                                  8192, 16384).succeeded);
    assert(read_cfg_failed.writes.empty());
    FakeWmi read_overlay_failed;
    read_overlay_failed.fail_read_overlay = true;
    assert(!configure_uwf_overlay(read_overlay_failed, UwfOverlayType::disk,
                                  8192, 16384).succeeded);
    assert(read_overlay_failed.writes.empty());
    FakeWmi invalid_type;
    invalid_type.next.type = static_cast<UwfOverlayType>(99);
    assert(!configure_uwf_overlay(invalid_type, UwfOverlayType::disk,
                                  8192, 16384).succeeded);
    assert(invalid_type.writes.empty());
    FakeWmi unreadable;
    unreadable.readable = false;
    assert(!configure_uwf_overlay(unreadable, UwfOverlayType::disk,
                                  8192, 16384).succeeded);
    assert(unreadable.writes.empty());
}

void test_probe() {
    FakeWmi wmi;
    const auto initial = wmi.usage;
    auto result = probe_uwf_overlay(wmi);
    assert(result && result->consumption_mb == 30u && result->available_mb == 994u);
    assert(result->warning_mb == 512u && result->critical_mb == 896u);
    assert(wmi.writes.empty());
    assert(wmi.usage.consumption_mb == initial.consumption_mb);
    wmi.readable = false;
    assert(!probe_uwf_overlay(wmi));
    assert(wmi.writes.empty());
}

void test_servicing() {
    for (bool enter : {true, false}) {
        FakeWmi wmi;
        wmi.next_servicing = !enter;
        auto result = set_uwf_servicing(wmi, enter);
        assert(result.succeeded && result.changed && result.reboot_required);
        assert(wmi.next_servicing == enter);
        assert(set_uwf_servicing(wmi, enter).succeeded);
        assert(wmi.writes.size() == 1u);
        FakeWmi failed;
        failed.next_servicing = !enter;
        failed.fail_write = 1;
        assert(!set_uwf_servicing(failed, enter).succeeded);
        assert(failed.next_servicing == !enter && failed.writes.size() == 1u);
    }
    FakeWmi pending;
    pending.current_servicing = true;
    assert(!set_uwf_servicing(pending, true).succeeded);
    assert(pending.writes.empty());
    pending.next_servicing = true;
    const auto exit_result = set_uwf_servicing(pending, false);
    assert(exit_result.succeeded && exit_result.changed && exit_result.reboot_required);
    assert(!pending.next_servicing && pending.writes.size() == 1u);
}

void test_defender_exclusions() {
    const std::vector<std::wstring> expected{
        L"\\Program Files\\Windows Defender",
        L"\\ProgramData\\Microsoft\\Windows Defender",
        L"\\Windows\\WindowsUpdate.log",
        L"\\Windows\\Temp\\MpCmdRun.log",
    };
    FakeWmi wmi;
    assert(configure_uwf_overlay(wmi, UwfOverlayType::ram, 8192u, 0u).succeeded);
    assert(wmi.exclusions.empty()); // no implicit Defender opt-in
    wmi.writes.clear();
    const auto added = add_uwf_defender_exclusions(wmi);
    assert(added.succeeded && added.changed && added.reboot_required);
    assert(wmi.exclusions == expected);
    assert(wmi.writes.size() == 8u);
    for (std::size_t i = 0u; i < expected.size(); ++i) {
        assert(wmi.writes[i * 2u] == to_ascii_string(expected[i]));
    }
    assert(add_uwf_defender_exclusions(wmi).succeeded);
    assert(wmi.writes.size() == 8u); // exact existing exclusions not touched
    for (int step = 1; step <= 4; ++step) {
        FakeWmi failed;
        failed.exclusions.push_back(expected[0]);
        failed.fail_write = step;
        const auto result = add_uwf_defender_exclusions(failed);
        if (step <= 3) {
            assert(!result.succeeded && !result.changed && result.rollback_succeeded);
            assert(failed.exclusions == (std::vector<std::wstring>{expected[0]}));
            assert(failed.writes.size() == static_cast<std::size_t>(4 * step - 2));
        } else {
            assert(result.succeeded);
            assert(failed.exclusions == expected);
        }
    }
    FakeWmi unreadable;
    unreadable.readable = false;
    assert(!add_uwf_defender_exclusions(unreadable).succeeded);
    assert(unreadable.writes.empty());
}

void test_disable() {
    FakeWmi wmi;
    wmi.filter = {true, true};
    auto result = disable_uwf(wmi);
    assert(result.succeeded && result.reboot_required && result.changed);
    assert(wmi.filter.current_enabled && !wmi.filter.next_enabled);
    assert(disable_uwf(wmi).succeeded && wmi.writes.size() == 1u);
    FakeWmi failed;
    failed.filter = {true, true};
    failed.fail_write = 1;
    assert(!disable_uwf(failed).succeeded);
    assert(failed.filter.next_enabled && failed.writes.size() == 1u);
    FakeWmi unreadable;
    unreadable.readable = false;
    assert(!disable_uwf(unreadable).succeeded && unreadable.writes.empty());
    FakeWmi pending;
    pending.filter = {false, true};
    assert(!disable_uwf(pending).succeeded && pending.writes.empty());
}

void test_ram_to_disk_small_size() {
    FakeWmi wmi;
    // Small RAM size below 1024. If type were set before maximum, SetType(disk) would fail!
    wmi.current = {UwfOverlayType::ram, 512u};
    wmi.next = wmi.current;
    wmi.usage = {100u, 412u, 256u, 400u};
    const auto outcome = configure_uwf_overlay(wmi, UwfOverlayType::disk, 8192u, 16384u);
    assert(outcome.succeeded && outcome.changed && outcome.reboot_required);
    assert(wmi.next.type == UwfOverlayType::disk);
    assert(wmi.next.maximum_size_mb == 4096u);
    assert(wmi.usage.warning_mb == 3072u && wmi.usage.critical_mb == 3686u);
    assert(wmi.writes == (std::vector<std::string>{"maximum", "type", "critical", "warning"}));

    for (int step = 1; step <= 4; ++step) {
        FakeWmi failed;
        failed.current = {UwfOverlayType::ram, 512u};
        failed.next = failed.current;
        failed.usage = {100u, 412u, 256u, 400u};
        failed.fail_write = step;
        auto result = configure_uwf_overlay(failed, UwfOverlayType::disk, 8192u, 16384u);
        assert(!result.succeeded && !result.changed && result.rollback_succeeded);
        assert(failed.next.type == UwfOverlayType::ram);
        assert(failed.next.maximum_size_mb == 512u);
        assert(failed.usage.warning_mb == 256u);
        assert(failed.usage.critical_mb == 400u);
        assert(failed.writes.size() == static_cast<std::size_t>(2 * step - 1));
    }
}

void test_disk_to_ram_lowering() {
    FakeWmi wmi;
    wmi.current = {UwfOverlayType::disk, 4096u};
    wmi.next = wmi.current;
    wmi.usage = {500u, 3596u, 3072u, 3686u};
    // Target RAM with 4096 MB physical RAM -> target size 1024, warning 768, critical 921
    const auto outcome = configure_uwf_overlay(wmi, UwfOverlayType::ram, 4096u, 0u);
    assert(outcome.succeeded && outcome.changed && outcome.reboot_required);
    assert(wmi.next.type == UwfOverlayType::ram);
    assert(wmi.next.maximum_size_mb == 1024u);
    assert(wmi.usage.warning_mb == 768u && wmi.usage.critical_mb == 921u);
    // Writes: type first (switching to RAM), then maximum, then warning (lowering), then critical
    assert(wmi.writes == (std::vector<std::string>{"type", "maximum", "warning", "critical"}));

    for (int step = 1; step <= 4; ++step) {
        FakeWmi failed;
        failed.current = {UwfOverlayType::disk, 4096u};
        failed.next = failed.current;
        failed.usage = {500u, 3596u, 3072u, 3686u};
        failed.fail_write = step;
        auto result = configure_uwf_overlay(failed, UwfOverlayType::ram, 4096u, 0u);
        assert(!result.succeeded && !result.changed && result.rollback_succeeded);
        assert(failed.next.type == UwfOverlayType::disk);
        assert(failed.next.maximum_size_mb == 4096u);
        assert(failed.usage.warning_mb == 3072u);
        assert(failed.usage.critical_mb == 3686u);
        assert(failed.writes.size() == static_cast<std::size_t>(2 * step - 1));
    }
}

void test_rollback_failure() {
    {
        FakeWmi wmi;
        wmi.fail_write_steps = {2, 3};
        auto result = configure_uwf_overlay(wmi, UwfOverlayType::disk, 8192u, 16384u);
        assert(!result.succeeded);
        assert(result.changed && result.reboot_required);
        assert(!result.rollback_succeeded);
    }
    {
        FakeWmi wmi;
        wmi.fail_write_steps = {4, 6};
        auto result = add_uwf_defender_exclusions(wmi);
        assert(!result.succeeded);
        assert(result.changed && result.reboot_required);
        assert(!result.rollback_succeeded);
    }
    {
        FakeWmi wmi;
        wmi.current = {UwfOverlayType::disk, 4096u};
        wmi.next = wmi.current;
        wmi.usage = {500u, 3596u, 3072u, 3686u};
        wmi.fail_write_steps = {2, 3};
        auto result = configure_uwf_overlay(wmi, UwfOverlayType::ram, 4096u, 0u);
        assert(!result.succeeded);
        assert(result.changed && result.reboot_required);
        assert(!result.rollback_succeeded);
    }
}

void test_rollback_unset_thresholds() {
    // Test that rolling back from forward changes to original unset (0/0) thresholds succeeds.
    // Microsoft UWF docs specify that size 0 disables threshold notifications ("unset" state):
    // https://learn.microsoft.com/en-us/windows/configuration/unified-write-filter/uwf-overlaysetwarningthreshold
    // https://learn.microsoft.com/en-us/windows/configuration/unified-write-filter/uwf-overlaysetcriticalthreshold
    {
        FakeWmi wmi;
        wmi.usage = {0u, 1024u, 0u, 0u};
        const auto outcome = configure_uwf_overlay(wmi, UwfOverlayType::disk, 8192u, 16384u);
        assert(outcome.succeeded && outcome.changed && outcome.reboot_required);
        assert(wmi.usage.warning_mb == 3072u && wmi.usage.critical_mb == 3686u);
        assert(wmi.writes == (std::vector<std::string>{"maximum", "type", "critical", "warning"}));
    }
    for (int step = 1; step <= 4; ++step) {
        FakeWmi failed;
        failed.usage = {0u, 1024u, 0u, 0u}; // thresholds unset (0/0)
        failed.fail_write = step;
        auto result = configure_uwf_overlay(failed, UwfOverlayType::disk, 8192u, 16384u);
        assert(!result.succeeded && !result.changed);
        assert(result.rollback_succeeded);
        assert(failed.usage.warning_mb == 0u);
        assert(failed.usage.critical_mb == 0u);
        assert(failed.writes.size() == static_cast<std::size_t>(2 * step - 1));
    }
}
} // namespace

int main() {
    test_formula();
    test_overlay();
    test_ram_to_disk_small_size();
    test_disk_to_ram_lowering();
    test_probe();
    test_servicing();
    test_defender_exclusions();
    test_disable();
    test_rollback_failure();
    test_rollback_unset_thresholds();
}
