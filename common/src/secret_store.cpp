#include "nstu/secret_store.hpp"

#include "nstu/auth.hpp"

#include <windows.h>
#include <dpapi.h>
#include <sddl.h>

#include <algorithm>
#include <array>
#include <limits>

namespace nstu::security {
namespace {

inline constexpr std::size_t kMaximumSecretFileBytes = 1024u * 1024u;

void set_error(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
}

DATA_BLOB blob_from(std::span<const std::byte> bytes) {
    return {
        static_cast<DWORD>(bytes.size()),
        const_cast<BYTE*>(reinterpret_cast<const BYTE*>(bytes.data())),
    };
}

bool write_all(HANDLE file, std::span<const std::byte> bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(
            bytes.size() - offset, std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (!WriteFile(file, bytes.data() + offset, chunk, &written, nullptr) ||
            written == 0) {
            return false;
        }
        offset += written;
    }
    return true;
}

std::wstring temporary_secret_path(const std::wstring& final_path) {
    std::array<std::byte, sizeof(std::uint64_t)> random{};
    if (!generate_random(random)) {
        return {};
    }
    std::uint64_t suffix = 0;
    for (std::size_t index = 0; index < random.size(); ++index) {
        suffix |= static_cast<std::uint64_t>(
                      std::to_integer<unsigned int>(random[index]))
                  << (index * 8u);
    }
    return final_path + L".tmp-" + std::to_wstring(GetCurrentProcessId()) +
           L"-" + std::to_wstring(suffix);
}

} // namespace

std::vector<std::byte> protect_machine_secret(
    std::span<const std::byte> plaintext,
    std::span<const std::byte> optional_entropy, std::string* error) {
    if (plaintext.empty() || plaintext.size() > std::numeric_limits<DWORD>::max() ||
        optional_entropy.size() > std::numeric_limits<DWORD>::max()) {
        set_error(error, "invalid secret size");
        return {};
    }
    auto input = blob_from(plaintext);
    auto entropy = blob_from(optional_entropy);
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"NSTU machine secret",
                          optional_entropy.empty() ? nullptr : &entropy, nullptr,
                          nullptr, CRYPTPROTECT_LOCAL_MACHINE |
                                       CRYPTPROTECT_UI_FORBIDDEN,
                          &output)) {
        set_error(error, "CryptProtectData failed");
        return {};
    }
    std::vector<std::byte> protected_blob(output.cbData);
    std::copy_n(reinterpret_cast<const std::byte*>(output.pbData), output.cbData,
                protected_blob.begin());
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return protected_blob;
}

std::vector<std::byte> unprotect_machine_secret(
    std::span<const std::byte> protected_blob,
    std::span<const std::byte> optional_entropy, std::string* error) {
    if (protected_blob.empty() ||
        protected_blob.size() > std::numeric_limits<DWORD>::max() ||
        optional_entropy.size() > std::numeric_limits<DWORD>::max()) {
        set_error(error, "invalid protected secret size");
        return {};
    }
    auto input = blob_from(protected_blob);
    auto entropy = blob_from(optional_entropy);
    DATA_BLOB output{};
    LPWSTR description = nullptr;
    if (!CryptUnprotectData(&input, &description,
                            optional_entropy.empty() ? nullptr : &entropy,
                            nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                            &output)) {
        set_error(error, "CryptUnprotectData failed");
        return {};
    }
    if (description != nullptr) {
        LocalFree(description);
    }
    std::vector<std::byte> plaintext(output.cbData);
    std::copy_n(reinterpret_cast<const std::byte*>(output.pbData), output.cbData,
                plaintext.begin());
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return plaintext;
}

bool save_machine_secret(std::wstring_view path,
                         std::span<const std::byte> plaintext,
                         std::span<const std::byte> optional_entropy,
                         std::string* error) {
    auto protected_blob =
        protect_machine_secret(plaintext, optional_entropy, error);
    if (protected_blob.empty()) {
        return false;
    }
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    constexpr wchar_t sddl[] =
        L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;OW)";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl, SDDL_REVISION_1, &descriptor, nullptr)) {
        secure_zero(protected_blob);
        set_error(error, "secret file SDDL conversion failed");
        return false;
    }
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    const std::wstring file_path(path);
    const auto temporary_path = temporary_secret_path(file_path);
    if (temporary_path.empty()) {
        LocalFree(descriptor);
        secure_zero(protected_blob);
        set_error(error, "secret temporary path generation failed");
        return false;
    }
    const HANDLE file = CreateFileW(
        temporary_path.c_str(), GENERIC_WRITE, 0, &attributes, CREATE_NEW,
        FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED, nullptr);
    LocalFree(descriptor);
    if (file == INVALID_HANDLE_VALUE) {
        secure_zero(protected_blob);
        set_error(error, "secret file creation failed");
        return false;
    }
    const bool written = write_all(file, protected_blob) &&
                         FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    secure_zero(protected_blob);
    const bool replaced = written &&
        MoveFileExW(temporary_path.c_str(), file_path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!replaced) {
        DeleteFileW(temporary_path.c_str());
        set_error(error, "secret file write failed");
    }
    return replaced;
}

std::vector<std::byte> load_machine_secret(
    std::wstring_view path, std::span<const std::byte> optional_entropy,
    std::string* error) {
    const std::wstring file_path(path);
    const HANDLE file = CreateFileW(file_path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        set_error(error, "secret file open failed");
        return {};
    }
    LARGE_INTEGER file_size{};
    if (!GetFileSizeEx(file, &file_size) || file_size.QuadPart <= 0 ||
        file_size.QuadPart > static_cast<LONGLONG>(kMaximumSecretFileBytes)) {
        CloseHandle(file);
        set_error(error, "secret file size is invalid");
        return {};
    }
    std::vector<std::byte> protected_blob(
        static_cast<std::size_t>(file_size.QuadPart));
    std::size_t offset = 0;
    while (offset < protected_blob.size()) {
        DWORD read_bytes = 0;
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(
            protected_blob.size() - offset,
            std::numeric_limits<DWORD>::max()));
        if (!ReadFile(file, protected_blob.data() + offset, chunk, &read_bytes,
                      nullptr) ||
            read_bytes == 0) {
            CloseHandle(file);
            secure_zero(protected_blob);
            set_error(error, "secret file read failed");
            return {};
        }
        offset += read_bytes;
    }
    CloseHandle(file);
    auto plaintext =
        unprotect_machine_secret(protected_blob, optional_entropy, error);
    secure_zero(protected_blob);
    return plaintext;
}

} // namespace nstu::security
