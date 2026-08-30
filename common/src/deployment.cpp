#include "nstu/deployment.hpp"

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include <array>

namespace nstu::deployment {
namespace {

void set_error(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
}

bool acceptable_root(const std::filesystem::path& path) {
    return path.is_absolute() && path.has_root_name() &&
           path != path.root_path() && !path.empty();
}

} // namespace

std::filesystem::path data_root(std::string* error) {
    std::array<wchar_t, 1024> configured{};
    DWORD bytes = static_cast<DWORD>(configured.size() * sizeof(wchar_t));
    const LSTATUS registry_result = RegGetValueW(
        HKEY_LOCAL_MACHINE, L"SOFTWARE\\NSTU", L"DataRoot",
        RRF_RT_REG_SZ, nullptr, configured.data(), &bytes);
    if (registry_result == ERROR_SUCCESS) {
        std::filesystem::path path(configured.data());
        if (acceptable_root(path)) {
            return path;
        }
        set_error(error, "configured NSTU data root is invalid");
        return {};
    }
    wchar_t program_data[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(
        L"ProgramData", program_data,
        static_cast<DWORD>(std::size(program_data)));
    if (length == 0 || length >= std::size(program_data)) {
        set_error(error, "ProgramData path is unavailable");
        return {};
    }
    return std::filesystem::path(program_data) / L"NSTU";
}

bool ensure_data_root(const std::filesystem::path& path, std::string* error) {
    if (!acceptable_root(path)) {
        set_error(error, "NSTU data root must be an absolute subdirectory");
        return false;
    }
    std::error_code existence_error;
    const bool existed = std::filesystem::exists(path, existence_error);
    std::error_code directory_error;
    std::filesystem::create_directories(path, directory_error);
    if (directory_error) {
        set_error(error, "NSTU data root creation failed");
        return false;
    }
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL dacl = nullptr;
    constexpr wchar_t sddl[] =
        L"D:P(A;OICI;GA;;;SY)(A;OICI;GA;;;BA)(A;OICI;GA;;;OW)";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl, SDDL_REVISION_1, &descriptor, nullptr)) {
        set_error(error, "NSTU data root ACL creation failed");
        return false;
    }
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    const bool got_dacl = GetSecurityDescriptorDacl(
        descriptor, &present, &dacl, &defaulted) != FALSE && present;
    const DWORD result = got_dacl
        ? SetNamedSecurityInfoW(
              const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
              DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
              nullptr, nullptr, dacl, nullptr)
        : ERROR_INVALID_SECURITY_DESCR;
    LocalFree(descriptor);
    if (result == ERROR_ACCESS_DENIED && existed) {
        return true;
    }
    if (result != ERROR_SUCCESS) {
        set_error(error, "NSTU data root ACL update failed");
        return false;
    }
    return true;
}

} // namespace nstu::deployment
