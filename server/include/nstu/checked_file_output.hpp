#pragma once

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <filesystem>
#include <limits>
#include <span>
#include <string_view>
#include <utility>

namespace nstu::server::files {

enum class FileFailureStage { none, path, open, write, flush, close, exception };
enum class ResourceOwnership { not_acquired, held, released, unknown };

struct FileOutputResult {
    FileFailureStage failure = FileFailureStage::path;
    DWORD native_error = 0;
    bool cleanup_failed = false;
    bool release_unconfirmed = false;
    [[nodiscard]] bool ok() const noexcept { return failure == FileFailureStage::none; }
};

struct FileCallResult { bool succeeded = false; DWORD error = 0; };
struct FileWriteResult { bool succeeded = false; DWORD error = 0; std::size_t bytes_written = 0; };

// This boundary owns exactly one resource. Ownership must be published before
// throwing, so the same checked algorithm can distinguish safe cleanup from an
// ambiguous/released handle. Callers serialize each instance.
class FileOutputApi {
public:
    FileOutputApi() = default;
    virtual ~FileOutputApi() = default;
    FileOutputApi(const FileOutputApi&) = delete;
    FileOutputApi& operator=(const FileOutputApi&) = delete;
    [[nodiscard]] virtual FileCallResult open(const std::filesystem::path& path) = 0;
    [[nodiscard]] virtual FileWriteResult write_some(std::span<const char> bytes) = 0;
    [[nodiscard]] virtual FileCallResult flush() = 0;
    [[nodiscard]] virtual FileCallResult close() = 0;
    [[nodiscard]] virtual ResourceOwnership ownership_state() const noexcept = 0;
};

namespace detail {
class NativeFileOutputApi final : public FileOutputApi {
public:
    ~NativeFileOutputApi() override {
        if (ownership_ == ResourceOwnership::held) (void)close();
    }
    FileCallResult open(const std::filesystem::path& path) noexcept override {
        if (ownership_ == ResourceOwnership::held || ownership_ == ResourceOwnership::unknown)
            return {false, ERROR_INVALID_HANDLE};
        handle_ = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            ownership_ = ResourceOwnership::not_acquired;
            return {false, error};
        }
        ownership_ = ResourceOwnership::held;
        return {true, 0};
    }
    FileWriteResult write_some(std::span<const char> bytes) noexcept override {
        if (ownership_ != ResourceOwnership::held || bytes.size() > std::numeric_limits<DWORD>::max())
            return {false, ERROR_INVALID_PARAMETER, 0};
        DWORD written = 0;
        if (!WriteFile(handle_, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)) {
            const DWORD error = GetLastError();
            return {false, error, written};
        }
        return {true, 0, written};
    }
    FileCallResult flush() noexcept override {
        if (ownership_ != ResourceOwnership::held) return {false, ERROR_INVALID_HANDLE};
        if (!FlushFileBuffers(handle_)) {
            const DWORD error = GetLastError();
            return {false, error};
        }
        return {true, 0};
    }
    FileCallResult close() noexcept override {
        if (ownership_ != ResourceOwnership::held) return {false, ERROR_INVALID_HANDLE};
        const bool closed = CloseHandle(handle_) != FALSE;
        const DWORD error = closed ? 0 : GetLastError();
        // A failed native close is ambiguous. Disarm rather than retry a
        // potentially released/reused handle from this owner or its destructor.
        handle_ = INVALID_HANDLE_VALUE;
        ownership_ = closed ? ResourceOwnership::released : ResourceOwnership::unknown;
        return {closed, error};
    }
    ResourceOwnership ownership_state() const noexcept override { return ownership_; }
private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    ResourceOwnership ownership_ = ResourceOwnership::not_acquired;
};
} // namespace detail

[[nodiscard]] inline FileOutputResult write_checked_file(const std::filesystem::path& path,
    std::string_view bytes, FileOutputApi& api) noexcept {
    bool acquisition_started = false;
    const auto fail = [&](FileOutputResult result) noexcept {
        if (acquisition_started && api.ownership_state() == ResourceOwnership::held) {
            // Exactly one cleanup attempt, and only for definitely held
            // ownership. Released/unknown never permits a second close.
            try {
                const auto cleanup = api.close();
                result.cleanup_failed = !cleanup.succeeded ||
                    api.ownership_state() != ResourceOwnership::released;
            } catch (...) {
                result.cleanup_failed = true;
            }
        }
        const auto ownership = api.ownership_state();
        result.release_unconfirmed = ownership == ResourceOwnership::held ||
                                     ownership == ResourceOwnership::unknown;
        return result;
    };
    try {
        if (path.empty() || !path.is_absolute() ||
            path.native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos)
            return {FileFailureStage::path, ERROR_INVALID_NAME};
        if (api.ownership_state() == ResourceOwnership::held || api.ownership_state() == ResourceOwnership::unknown)
            return {FileFailureStage::open, ERROR_INVALID_HANDLE, false, true};
        acquisition_started = true;
        const auto opened = api.open(path);
        if (!opened.succeeded)
            return fail({FileFailureStage::open, opened.error ? opened.error : ERROR_OPEN_FAILED});
        if (api.ownership_state() != ResourceOwnership::held)
            return fail({FileFailureStage::open, ERROR_INVALID_HANDLE});
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto count = std::min(bytes.size() - offset,
                static_cast<std::size_t>(std::numeric_limits<DWORD>::max()));
            const auto written = api.write_some(std::span<const char>{bytes.data() + offset, count});
            if (!written.succeeded || written.bytes_written == 0 || written.bytes_written > count)
                return fail({FileFailureStage::write, written.error ? written.error : ERROR_WRITE_FAULT});
            offset += written.bytes_written;
        }
        const auto flushed = api.flush();
        if (!flushed.succeeded)
            return fail({FileFailureStage::flush, flushed.error ? flushed.error : ERROR_WRITE_FAULT});
        const auto closed = api.close();
        if (!closed.succeeded)
            return fail({FileFailureStage::close, closed.error ? closed.error : ERROR_INVALID_HANDLE});
        if (api.ownership_state() != ResourceOwnership::released)
            return fail({FileFailureStage::close, ERROR_INVALID_HANDLE});
        return {FileFailureStage::none};
    } catch (...) {
        return fail({FileFailureStage::exception, ERROR_UNHANDLED_EXCEPTION});
    }
}

[[nodiscard]] inline FileOutputResult write_checked_file(const std::filesystem::path& path,
    std::string_view bytes) noexcept {
    detail::NativeFileOutputApi api;
    return write_checked_file(path, bytes, api);
}

struct DocumentsPathResult {
    std::filesystem::path path;
    HRESULT error = E_FAIL;
    [[nodiscard]] bool ok() const noexcept { return SUCCEEDED(error) && !path.empty() && path.is_absolute(); }
};

using DocumentsQuery = HRESULT (WINAPI*)(REFKNOWNFOLDERID, DWORD, HANDLE, PWSTR*);

[[nodiscard]] inline DocumentsPathResult current_user_documents_path(DocumentsQuery query = &SHGetKnownFolderPath) noexcept {
    struct FolderMemory {
        PWSTR value = nullptr;
        FolderMemory() = default;
        FolderMemory(const FolderMemory&) = delete;
        FolderMemory& operator=(const FolderMemory&) = delete;
        ~FolderMemory() { CoTaskMemFree(value); }
    } memory;
    if (!query) return {{}, E_INVALIDARG};
    try {
        const HRESULT error = query(FOLDERID_Documents, 0, nullptr, &memory.value);
        if (FAILED(error)) return {{}, error};
        if (!memory.value || !*memory.value) return {{}, E_FAIL};
        std::filesystem::path path(memory.value);
        if (!path.is_absolute()) return {{}, E_INVALIDARG};
        return {std::move(path), error};
    } catch (...) {
        return {{}, E_FAIL};
    }
}

} // namespace nstu::server::files
