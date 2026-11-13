#include "nstu/programming_outbox.hpp"
#include "nstu/secret_store.hpp"

#include <windows.h>
#include <sddl.h>
#include <aclapi.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace nstu::client {
namespace {
using exam::programming::Operation;
using exam::programming::Request;
constexpr std::size_t kMaximumRecords = 256;
constexpr std::size_t kMaximumBytes = 8u * 1024u * 1024u;
constexpr char kEntropy[] = "NSTU-CS-OUTBOX-V1";
const auto entropy = std::as_bytes(std::span(kEntropy, sizeof(kEntropy) - 1));

void error_text(std::string* error, const char* text) { if (error) *error = text; }
bool durable(Operation operation) {
    return operation == Operation::draft || operation == Operation::submit || operation == Operation::finalize;
}
std::string hex(std::span<const std::byte> bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto value : bytes) {
        const auto byte = std::to_integer<unsigned>(value);
        result += digits[byte >> 4u]; result += digits[byte & 15u];
    }
    return result;
}
std::string key_for(const Request& request) {
    auto bytes = exam::encode_state_request(request.context);
    bytes.push_back(static_cast<std::byte>(request.operation));
    // Drafts coalesce by document; immutable submissions retain request identity.
    const auto identity = request.operation == Operation::draft
        ? request.problem_id + '\0' + request.language + '\0' + std::to_string(request.problem_revision)
        : request.request_id;
    const auto text = std::as_bytes(std::span(identity.data(), identity.size()));
    bytes.insert(bytes.end(), text.begin(), text.end());
    const auto digest = security::sha256(bytes);
    return digest ? hex(*digest) : std::string{};
}
bool no_reparse(const std::filesystem::path& path) {
    for (auto part = path; !part.empty(); part = part.parent_path()) {
        const auto attributes = GetFileAttributesW(part.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        if (part == part.parent_path()) break;
    }
    return true;
}
struct RestrictedAttributes {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    SECURITY_ATTRIBUTES attributes{};
    RestrictedAttributes() {
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;OW)", SDDL_REVISION_1, &descriptor, nullptr)) {
            attributes = {sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
        }
    }
    ~RestrictedAttributes() { if (descriptor) LocalFree(descriptor); }
};
bool restricted_owner_and_acl(HANDLE handle) {
    RestrictedAttributes expected;
    PSID owner = nullptr;
    PACL actual_acl = nullptr, expected_acl = nullptr;
    PSECURITY_DESCRIPTOR actual = nullptr;
    if (!expected.descriptor || GetSecurityInfo(handle, SE_FILE_OBJECT,
            OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
            &owner, nullptr, &actual_acl, nullptr, &actual) != ERROR_SUCCESS) return false;
    BOOL present = FALSE, defaulted = FALSE;
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    bool valid = owner && IsValidSid(owner) && actual_acl && IsValidAcl(actual_acl) &&
        GetSecurityDescriptorControl(actual, &control, &revision) && (control & SE_DACL_PROTECTED) &&
        GetSecurityDescriptorDacl(expected.descriptor, &present, &expected_acl, &defaulted) && present && expected_acl &&
        actual_acl->AclSize == expected_acl->AclSize &&
        std::memcmp(actual_acl, expected_acl, actual_acl->AclSize) == 0;
    // OWNER RIGHTS is safe only when the owner is the service, administrators or this process.
    std::array<std::byte, SECURITY_MAX_SID_SIZE> system_sid{}, admin_sid{};
    DWORD system_size = static_cast<DWORD>(system_sid.size()), admin_size = static_cast<DWORD>(admin_sid.size());
    HANDLE token = nullptr;
    alignas(TOKEN_USER) std::array<std::byte, 1024> token_user{};
    DWORD token_bytes = 0;
    const bool token_open = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != FALSE;
    valid = valid && CreateWellKnownSid(WinLocalSystemSid, nullptr, system_sid.data(), &system_size) &&
        CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admin_sid.data(), &admin_size) && token_open &&
        GetTokenInformation(token, TokenUser, token_user.data(), static_cast<DWORD>(token_user.size()), &token_bytes) &&
        (EqualSid(owner, system_sid.data()) || EqualSid(owner, admin_sid.data()) ||
         EqualSid(owner, reinterpret_cast<const TOKEN_USER*>(token_user.data())->User.Sid));
    if (token_open) CloseHandle(token);
    LocalFree(actual);
    return valid;
}
std::vector<std::byte> read_wire(const std::filesystem::path& path) {
    if (!no_reparse(path) || !std::filesystem::is_regular_file(path) ||
        std::filesystem::file_size(path) > exam::kMaximumExamPayloadBytes + 4096u) return {};
    std::ifstream input(path, std::ios::binary);
    const std::string stored{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (input.bad() || stored.empty()) return {};
    return security::unprotect_machine_secret(std::as_bytes(std::span(stored.data(), stored.size())), entropy);
}
bool write_wire(const std::filesystem::path& path, std::span<const std::byte> wire, bool replace) {
    const auto blob = security::protect_machine_secret(wire, entropy);
    std::array<std::byte, 16> nonce{};
    if (blob.empty() || blob.size() > exam::kMaximumExamPayloadBytes + 4096u ||
        !security::generate_random(nonce) || !no_reparse(path)) return false;
    const auto temporary = path.parent_path() / (path.filename().string() + ".tmp-" + hex(nonce));
    RestrictedAttributes security_attributes;
    if (!security_attributes.descriptor) return false;
    const auto file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, &security_attributes.attributes,
        CREATE_NEW, FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool succeeded = WriteFile(file, blob.data(), static_cast<DWORD>(blob.size()), &written, nullptr) &&
        written == blob.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (succeeded) succeeded = MoveFileExW(temporary.c_str(), path.c_str(),
        MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0)) != FALSE;
    if (!succeeded) DeleteFileW(temporary.c_str());
    return succeeded;
}
bool hex_key(std::string_view name) {
    return name.size() == 64 && std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
}

ProgrammingOutbox::~ProgrammingOutbox() { close(); }
bool ProgrammingOutbox::open(const std::filesystem::path& directory, std::string* error) {
    std::scoped_lock lock(mutex_);
    if (lock_handle_) { error_text(error, "programming outbox is already open"); return false; }
    try {
        if (!directory.is_absolute() || directory.filename() != L"programming-outbox" ||
            !std::filesystem::is_directory(directory.parent_path()) || !no_reparse(directory)) {
            error_text(error, "programming outbox path is not an approved directory"); return false;
        }
        RestrictedAttributes attributes;
        if (!attributes.descriptor || (!CreateDirectoryW(directory.c_str(), &attributes.attributes) &&
            GetLastError() != ERROR_ALREADY_EXISTS) || !no_reparse(directory)) {
            error_text(error, "programming outbox directory is unavailable"); return false;
        }
        root_ = std::filesystem::canonical(directory);
        const auto directory_handle = CreateFileW(root_.c_str(), READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        const bool restricted_directory = directory_handle != INVALID_HANDLE_VALUE && restricted_owner_and_acl(directory_handle);
        if (directory_handle != INVALID_HANDLE_VALUE) CloseHandle(directory_handle);
        if (!restricted_directory) {
            root_.clear(); error_text(error, "programming outbox directory ownership or ACL is not trusted"); return false;
        }
        const auto lock_path = root_ / L"outbox.lock";
        if (!no_reparse(lock_path)) { root_.clear(); return false; }
        const auto handle = CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
            &attributes.attributes, OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE || !restricted_owner_and_acl(handle)) {
            if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
            root_.clear(); error_text(error, "programming outbox ownership is unavailable"); return false;
        }
        lock_handle_ = handle;
        std::uintmax_t disk_bytes = 0;
        std::size_t disk_records = 0;
        for (const auto& entry : std::filesystem::directory_iterator(root_)) {
            if (!no_reparse(entry.path()) || !entry.is_regular_file()) throw std::runtime_error("unsafe record");
            disk_bytes += entry.file_size(); ++disk_records;
            if (disk_bytes > kMaximumBytes + 1024u * 1024u || disk_records > kMaximumRecords * 2 + 1) throw std::runtime_error("quota");
            if (entry.path().filename() == L"outbox.lock") continue;
            const auto name = entry.path().filename().string();
            // Retain an interrupted encrypted write for recovery; never erase it.
            if (name.size() == 105 && name.substr(64, 9) == ".csb.tmp-" && hex_key(name.substr(0, 64))) continue;
            if (entry.path().extension() != L".csb" || !hex_key(entry.path().stem().string())) throw std::runtime_error("unknown record");
            const auto wire = read_wire(entry.path());
            const auto request = exam::programming::decode_request(wire);
            if (!request || !durable(request->operation) || key_for(*request) != entry.path().stem().string()) throw std::runtime_error("invalid record");
            if (pending_.size() >= kMaximumRecords || bytes_ + wire.size() > kMaximumBytes) throw std::runtime_error("quota");
            pending_.emplace(entry.path().stem().string(), *request); bytes_ += wire.size();
        }
        return true;
    } catch (const std::exception&) {
        if (lock_handle_) CloseHandle(lock_handle_);
        lock_handle_ = nullptr; root_.clear(); pending_.clear(); bytes_ = 0;
        error_text(error, "programming outbox recovery failed; original records retained"); return false;
    }
}
bool ProgrammingOutbox::enqueue(const Request& request, std::string* error) {
    std::scoped_lock lock(mutex_);
    try {
        if (!lock_handle_ || !no_reparse(root_) || !durable(request.operation) ||
            !exam::programming::validate_request(request)) { error_text(error, "programming outbox request is unavailable or invalid"); return false; }
        const auto wire = exam::programming::encode_request(request);
        const auto key = key_for(request);
        if (wire.empty() || key.empty()) return false;
        const auto found = pending_.find(key);
        std::size_t previous_bytes = 0;
        if (found != pending_.end()) {
            const auto previous = exam::programming::encode_request(found->second);
            if (previous == wire) return true;
            if (request.operation != Operation::draft || request.request_id == found->second.request_id ||
                request.draft_revision < found->second.draft_revision ||
                (request.draft_revision == found->second.draft_revision && request.source != found->second.source)) {
                error_text(error, "programming outbox identity or revision conflict"); return false;
            }
            previous_bytes = previous.size();
        }
        if ((found == pending_.end() && pending_.size() >= kMaximumRecords) ||
            bytes_ - previous_bytes + wire.size() > kMaximumBytes) { error_text(error, "programming outbox quota exceeded"); return false; }
        Request prepared = request;
        auto [position, inserted] = pending_.try_emplace(key, request);
        if (!write_wire(root_ / (key + ".csb"), wire, !inserted)) {
            if (inserted) pending_.erase(position);
            error_text(error, "programming outbox write or flush failed"); return false;
        }
        if (!inserted) std::swap(position->second, prepared);
        bytes_ = bytes_ - previous_bytes + wire.size();
        return true;
    } catch (const std::exception&) { error_text(error, "programming outbox could not preserve source"); return false; }
}
std::vector<Request> ProgrammingOutbox::pending_for_client(const security::ClientId& client_id, std::size_t maximum) const {
    std::scoped_lock lock(mutex_);
    std::vector<Request> result;
    if (!lock_handle_ || maximum == 0) return result;
    for (const auto& [key, request] : pending_) {
        (void)key;
        if (request.context.client_id == client_id) result.push_back(request);
        if (result.size() >= std::min(maximum, kMaximumRecords)) break;
    }
    return result;
}
bool ProgrammingOutbox::acknowledge(const exam::programming::Response& response, std::string* error) {
    std::scoped_lock lock(mutex_);
    if (!lock_handle_) return false;
    try {
        for (auto entry = pending_.begin(); entry != pending_.end(); ++entry) {
            if (entry->second.request_id != response.request_id ||
                exam::encode_state_request(entry->second.context) != exam::encode_state_request(response.context)) continue;
            const auto checked = exam::programming::parse_response_json(response.json, &entry->second);
            if (!checked || !((entry->second.operation == Operation::draft && checked->status == "saved") ||
                (entry->second.operation == Operation::submit && (checked->status == "received" || checked->status == "late")) ||
                (entry->second.operation == Operation::finalize && checked->status == "finalized"))) return false;
            const auto path = root_ / (entry->first + ".csb");
            const auto wire = exam::programming::encode_request(entry->second);
            if (read_wire(path) != wire || !no_reparse(path) || !DeleteFileW(path.c_str())) {
                error_text(error, "programming outbox acknowledgement could not remove its exact record"); return false;
            }
            bytes_ -= wire.size(); pending_.erase(entry); return true;
        }
        return true; // A late ACK for a coalesced draft cannot erase its successor.
    } catch (const std::exception&) { error_text(error, "programming outbox acknowledgement failed"); return false; }
}
void ProgrammingOutbox::close() noexcept {
    try {
        std::scoped_lock lock(mutex_);
        if (lock_handle_) CloseHandle(lock_handle_);
        lock_handle_ = nullptr; pending_.clear(); root_.clear(); bytes_ = 0;
    } catch (...) {}
}
} // namespace nstu::client
