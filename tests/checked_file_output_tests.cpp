#include "nstu/checked_file_output.hpp"

#include <cstring>
#include <functional>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace nstu::server::files;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

class ScriptedFile final : public FileOutputApi {
public:
    ResourceOwnership ownership = ResourceOwnership::not_acquired;
    std::string output;
    unsigned int opens = 0, writes = 0, flushes = 0, closes = 0;
    bool fail_open = false, throw_open_before = false, throw_open_after_acquire = false;
    unsigned int fail_write = 0, throw_write = 0;
    bool zero_progress = false, oversized_count = false, fail_flush = false, throw_flush = false;
    std::size_t short_limit = std::numeric_limits<std::size_t>::max();
    unsigned int fail_close = 0, throw_close = 0;
    ResourceOwnership failed_close_state = ResourceOwnership::unknown;
    ResourceOwnership thrown_close_state = ResourceOwnership::held;
    DWORD write_error = ERROR_DISK_FULL;
    FileCallResult open(const std::filesystem::path&) override {
        ++opens;
        if (throw_open_before) throw std::runtime_error("injected open exception before acquire");
        if (fail_open) return {false, ERROR_ACCESS_DENIED};
        ownership = ResourceOwnership::held;
        if (throw_open_after_acquire) throw std::runtime_error("injected open exception after acquire");
        return {true, 0};
    }
    FileWriteResult write_some(std::span<const char> bytes) override {
        ++writes;
        require(ownership == ResourceOwnership::held, "write must hold the resource");
        if (writes == throw_write) throw std::runtime_error("injected write exception");
        if (writes == fail_write) return {false, write_error, 0};
        if (zero_progress) return {true, 0, 0};
        if (oversized_count) return {true, 0, bytes.size() + 1};
        const auto count = std::min(bytes.size(), short_limit);
        if (count) output.append(bytes.data(), count);
        return {true, 0, count};
    }
    FileCallResult flush() override {
        ++flushes;
        require(ownership == ResourceOwnership::held, "flush must hold the resource");
        if (throw_flush) throw std::runtime_error("injected flush exception");
        return fail_flush ? FileCallResult{false, ERROR_WRITE_FAULT} : FileCallResult{true, 0};
    }
    FileCallResult close() override {
        ++closes;
        require(ownership == ResourceOwnership::held, "close must hold a definitely owned resource");
        if (closes == throw_close) {
            ownership = thrown_close_state;
            throw std::runtime_error("injected close exception");
        }
        if (closes == fail_close) {
            ownership = failed_close_state;
            return {false, ERROR_INVALID_HANDLE};
        }
        ownership = ResourceOwnership::released;
        return {true, 0};
    }
    ResourceOwnership ownership_state() const noexcept override { return ownership; }
};

const std::filesystem::path injected_path{L"C:\\owned-fixture\\output.bin"};

void complete_and_short_writes() {
    for (const auto limit : {std::size_t{2}, std::size_t{100}}) {
        ScriptedFile file;
        file.short_limit = limit;
        const auto result = write_checked_file(injected_path, "abcdefg", file);
        require(result.ok(), "complete write/flush/close must succeed");
        require(file.output == "abcdefg", "every byte must be written, including short-write tails");
        require(file.writes == (limit == 2 ? 4u : 1u), "short writes must continue exactly to completion");
        require(file.flushes == 1 && file.closes == 1, "completion requires one checked flush and close");
        require(file.ownership == ResourceOwnership::released, "success requires released ownership");
    }
}

void failed_write_is_not_saved() {
    ScriptedFile file;
    file.short_limit = 2;
    file.fail_write = 2;
    const auto result = write_checked_file(injected_path, "abcdefg", file);
    require(!result.ok() && result.failure == FileFailureStage::write, "late disk-full write must fail");
    require(result.native_error == ERROR_DISK_FULL && file.output == "ab", "first write error and actual partial bytes are retained");
    require(file.flushes == 0 && file.closes == 1, "failed write skips success flush and safely closes once");
    require(!result.cleanup_failed && !result.release_unconfirmed, "successful cleanup confirms release but cannot confirm save");
}

void open_and_progress_failures() {
    ScriptedFile open;
    open.fail_open = true;
    const auto result = write_checked_file(injected_path, "data", open);
    require(!result.ok() && result.failure == FileFailureStage::open && result.native_error == ERROR_ACCESS_DENIED, "open error is preserved");
    require(open.writes == 0 && open.flushes == 0 && open.closes == 0, "unacquired resource is never used or closed");
    for (bool oversized : {false, true}) {
        ScriptedFile file;
        file.oversized_count = oversized;
        file.zero_progress = !oversized;
        const auto failed = write_checked_file(injected_path, "data", file);
        require(!failed.ok() && failed.failure == FileFailureStage::write, "zero/oversized progress must fail");
        require(file.writes == 1 && file.flushes == 0 && file.closes == 1, "invalid progress cannot loop, flush or double-close");
    }
}

void flush_close_and_sticky_failures() {
    ScriptedFile flush;
    flush.fail_flush = true;
    const auto failed = write_checked_file(injected_path, "data", flush);
    require(!failed.ok() && failed.failure == FileFailureStage::flush && failed.native_error == ERROR_WRITE_FAULT, "flush failure is not saved");
    require(flush.closes == 1 && flush.ownership == ResourceOwnership::released, "failed flush cleans up once");
    for (auto state : {ResourceOwnership::held, ResourceOwnership::released, ResourceOwnership::unknown}) {
        ScriptedFile file;
        file.fail_close = 1;
        file.failed_close_state = state;
        const auto result = write_checked_file(injected_path, "data", file);
        require(!result.ok() && result.failure == FileFailureStage::close && result.native_error == ERROR_INVALID_HANDLE, "close failure remains first error");
        require(file.closes == (state == ResourceOwnership::held ? 2u : 1u), "only definitely held close permits one cleanup");
        require(result.release_unconfirmed == (state == ResourceOwnership::unknown), "unknown close is disarmed and release unconfirmed");
    }
    ScriptedFile sticky;
    sticky.fail_write = 1;
    sticky.fail_close = 1;
    sticky.failed_close_state = ResourceOwnership::unknown;
    const auto result = write_checked_file(injected_path, "data", sticky);
    require(!result.ok() && result.failure == FileFailureStage::write && result.native_error == ERROR_DISK_FULL, "cleanup failure cannot overwrite first error");
    require(result.cleanup_failed && result.release_unconfirmed && sticky.closes == 1, "ambiguous cleanup never retries");
}

void exceptional_ownership_cleanup() {
    for (int stage = 0; stage != 4; ++stage) {
        ScriptedFile file;
        file.throw_open_before = stage == 0;
        file.throw_open_after_acquire = stage == 1;
        file.throw_write = stage == 2 ? 1 : 0;
        file.throw_flush = stage == 3;
        const auto result = write_checked_file(injected_path, "data", file);
        require(!result.ok() && result.failure == FileFailureStage::exception, "stage exception must return an error instead of escaping");
        require(file.closes == (stage == 0 ? 0u : 1u), "acquired exception must close once; unacquired must not close");
    }
    for (auto state : {ResourceOwnership::held, ResourceOwnership::released, ResourceOwnership::unknown}) {
        ScriptedFile file;
        file.throw_close = 1;
        file.thrown_close_state = state;
        const auto result = write_checked_file(injected_path, "data", file);
        require(!result.ok() && result.failure == FileFailureStage::exception, "close exception is a failed save");
        require(file.closes == (state == ResourceOwnership::held ? 2u : 1u), "release-before-throw and unknown never close twice");
        require(result.release_unconfirmed == (state == ResourceOwnership::unknown), "throw ownership state governs release confirmation");
    }
    ScriptedFile cleanup;
    cleanup.fail_write = 1;
    cleanup.throw_close = 1;
    cleanup.thrown_close_state = ResourceOwnership::held;
    const auto failed = write_checked_file(injected_path, "data", cleanup);
    require(failed.failure == FileFailureStage::write && failed.native_error == ERROR_DISK_FULL, "cleanup exception keeps original write failure");
    require(failed.cleanup_failed && failed.release_unconfirmed && cleanup.closes == 1, "failed cleanup is bounded without destructor retry");
    ScriptedFile released_cleanup;
    released_cleanup.fail_write = 1;
    released_cleanup.throw_close = 1;
    released_cleanup.thrown_close_state = ResourceOwnership::released;
    const auto released = write_checked_file(injected_path, "data", released_cleanup);
    require(released.failure == FileFailureStage::write && released.native_error == ERROR_DISK_FULL,
        "release-then-throw cleanup preserves the first write failure");
    require(released.cleanup_failed && !released.release_unconfirmed && released_cleanup.closes == 1,
        "released cleanup exception must not retry a reused handle");
    ScriptedFile second_failure;
    second_failure.throw_close = 1;
    second_failure.thrown_close_state = ResourceOwnership::held;
    second_failure.fail_close = 2;
    second_failure.failed_close_state = ResourceOwnership::unknown;
    const auto ambiguous = write_checked_file(injected_path, "data", second_failure);
    require(ambiguous.failure == FileFailureStage::exception && ambiguous.cleanup_failed &&
        ambiguous.release_unconfirmed && second_failure.closes == 2,
        "one permitted held cleanup that becomes unknown never permits a third close");
}

void paths_and_empty_payload() {
    for (const auto& path : {std::filesystem::path{}, std::filesystem::path{L"relative.bin"},
            std::filesystem::path{std::wstring{L'C', L':', L'\\', L'a', L'\0', L'b'}}}) {
        ScriptedFile file;
        const auto result = write_checked_file(path, "data", file);
        require(!result.ok() && result.failure == FileFailureStage::path && file.opens == 0, "invalid path fails before acquisition");
    }
    ScriptedFile empty;
    const auto result = write_checked_file(injected_path, {}, empty);
    require(result.ok() && empty.output.empty(), "empty room name writes zero bytes successfully");
    require(empty.writes == 0 && empty.flushes == 1 && empty.closes == 1, "empty payload still requires flush and close");
}

class OwnedFiles {
public:
    std::filesystem::path root;
    std::filesystem::path unicode;
    std::vector<std::filesystem::path> paths;
    OwnedFiles() {
        static unsigned int sequence = 0;
        root = std::filesystem::temp_directory_path() /
            (L"nstu-file-output-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++sequence));
        require(root.is_absolute() && CreateDirectoryW(root.c_str(), nullptr), "owned unique temp directory creation");
        unicode = root / L"Ph\u00f2ng h\u1ecdc \u1ebf";
        require(CreateDirectoryW(unicode.c_str(), nullptr), "owned Unicode directory creation");
    }
    ~OwnedFiles() {
        for (const auto& path : paths) {
            if (path.parent_path() == root || path.parent_path() == unicode) DeleteFileW(path.c_str());
        }
        RemoveDirectoryW(unicode.c_str());
        RemoveDirectoryW(root.c_str());
    }
    std::filesystem::path file(const wchar_t* name, bool wide = false) {
        auto path = (wide ? unicode : root) / name;
        paths.push_back(path);
        return path;
    }
};

void actual_native_readback() {
    OwnedFiles owned;
    const std::string payload("A\n\0B\xC4\x91\xE1\xBA\xBF\n", 10);
    for (const bool unicode : {false, true}) {
        const auto path = owned.file(L"k\u1ebft qu\u1ea3.bin", unicode);
        const auto result = write_checked_file(path, payload);
        require(result.ok() && !result.release_unconfirmed, "actual native write/flush/close succeeds");
        std::ifstream file(path, std::ios::binary);
        const std::string actual{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        require(actual == payload, "native byte readback including Unicode path/NUL/newline exactly matches input");
        file.close();
        const auto cleared = write_checked_file(path, {});
        require(cleared.ok() && std::filesystem::file_size(path) == 0, "actual empty payload truncates to zero bytes");
    }
}

void owned_native_sharing_denied() {
    OwnedFiles owned;
    const auto path = owned.file(L"held.bin");
    require(write_checked_file(path, "original").ok(), "create own denial fixture");
    struct HeldFile {
        HANDLE handle;
        ~HeldFile() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    } held{CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    require(held.handle != INVALID_HANDLE_VALUE, "own file exclusively held");
    const auto denied = write_checked_file(path, "replacement");
    require(!denied.ok() && denied.failure == FileFailureStage::open && denied.native_error == ERROR_SHARING_VIOLATION, "actual sharing-denied path refuses before truncation");
    require(!denied.release_unconfirmed, "failed open owns no unknown resource");
    require(CloseHandle(held.handle) != FALSE, "release owned denial handle");
    held.handle = INVALID_HANDLE_VALUE;
    std::ifstream file(path, std::ios::binary);
    const std::string actual{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    require(actual == "original", "sharing-denied open leaves existing fixture contents intact");
}

HRESULT WINAPI failed_documents_query(REFKNOWNFOLDERID folder, DWORD flags, HANDLE token, PWSTR* path) {
    require(IsEqualGUID(folder, FOLDERID_Documents) && flags == 0 && token == nullptr, "Documents query must use actual known folder/current user/flags0");
    const wchar_t value[] = L"C:\\owned\\unused";
    *path = static_cast<PWSTR>(CoTaskMemAlloc(sizeof(value)));
    require(*path != nullptr, "owned COM test allocation");
    std::memcpy(*path, value, sizeof(value));
    return E_ACCESSDENIED;
}

void documents_resolution() {
    const auto failed = current_user_documents_path(&failed_documents_query);
    require(!failed.ok() && failed.error == E_ACCESSDENIED && failed.path.empty(), "failed Documents resolution never guesses a fallback");
    require(!current_user_documents_path(nullptr).ok(), "missing resolver fails explicitly");
    const auto actual = current_user_documents_path();
    require(actual.ok() ? actual.path.is_absolute() : actual.path.empty(), "native Known Documents returns a wide absolute path or explicit failure");
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> cases{
        {"complete_write_flush_close_required_and_short_write_complete", complete_and_short_writes},
        {"late_disk_full_injected", failed_write_is_not_saved},
        {"open_failure_zero_progress_oversized_count", open_and_progress_failures},
        {"flush_close_sticky_failure_and_ownership", flush_close_and_sticky_failures},
        {"exceptional_acquisition_write_flush_close_cleanup", exceptional_ownership_cleanup},
        {"empty_path_and_payload", paths_and_empty_payload},
        {"actual_owned_temp_and_unicode_readback", actual_native_readback},
        {"actual_owned_sharing_denied", owned_native_sharing_denied},
        {"Documents_resolution_failure_no_fallback", documents_resolution}};
    unsigned int failed = 0;
    for (const auto& [name, run] : cases) {
        try { run(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { ++failed; std::cout << "FAIL " << name << ": " << error.what() << '\n'; }
        catch (...) { ++failed; std::cout << "FAIL " << name << ": unknown exception\n"; }
    }
    std::cout << cases.size() - failed << "/" << cases.size() << " checked native file-output cases passed\n";
    return failed ? 1 : 0;
}
