#include "nstu/setup/policy_audit.hpp"

#include <windows.h>

namespace nstu::setup {
namespace {
constexpr wchar_t kExplorer[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer";
constexpr wchar_t kSystem[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System";
constexpr wchar_t kCommand[] = L"Software\\Policies\\Microsoft\\Windows\\System";
bool read_dword(const wchar_t* path, const wchar_t* name, DWORD expected) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    DWORD value = 0, type = 0, bytes = sizeof(value);
    const bool enabled = RegQueryValueExW(key, name, nullptr, &type,
        reinterpret_cast<BYTE*>(&value), &bytes) == ERROR_SUCCESS &&
        type == REG_DWORD && value == expected;
    RegCloseKey(key);
    return enabled;
}
bool write_dword(const wchar_t* path, const wchar_t* name, DWORD value,
                 std::string* error) {
    HKEY key = nullptr; DWORD disposition = 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, &disposition) != ERROR_SUCCESS) {
        if (error != nullptr) *error = "RegCreateKeyExW failed";
        return false;
    }
    const LONG result = RegSetValueExW(key, name, 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
    if (result != ERROR_SUCCESS && error != nullptr) *error = "RegSetValueExW failed";
    return result == ERROR_SUCCESS;
}
} // namespace

PolicyState read_policy(std::string* error) {
    (void)error;
    return {.task_manager_disabled = read_dword(kSystem, L"DisableTaskMgr", 1),
            .command_prompt_disabled = read_dword(kCommand, L"DisableCMD", 2),
            .control_panel_disabled = read_dword(kExplorer, L"NoControlPanel", 1),
            .drive_c_hidden = read_dword(kExplorer, L"NoViewOnDrive", 4)};
}

bool apply_lockdown(std::string* error) {
    return write_dword(kSystem, L"DisableTaskMgr", 1, error) &&
           write_dword(kCommand, L"DisableCMD", 2, error) &&
           write_dword(kExplorer, L"NoControlPanel", 1, error) &&
           write_dword(kExplorer, L"NoViewOnDrive", 4, error);
}
} // namespace nstu::setup
