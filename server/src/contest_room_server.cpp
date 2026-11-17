#include "nstu/contest_room_server.hpp"
#include "nstu/multicast.hpp"
#include "nstu/secret_store.hpp"

#include <winsock2.h>
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include "nstu/exam_host.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <wincrypt.h>

namespace nstu::server {
namespace {
using Clock = std::chrono::steady_clock;
using Kind = contest_room::Kind;
constexpr std::size_t kMaximumProfileBytes = 1024u * 1024u;

void fail(std::string* error, const char* text) { if (error) *error = text; }
std::uint64_t unix_now() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}
std::uint64_t unix_milliseconds() {
    const auto count = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    return count > 0 ? static_cast<std::uint64_t>(count) : 0;
}
std::string hex(std::span<const std::byte> bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (const auto b : bytes) {
        const auto value = std::to_integer<unsigned>(b);
        result += digits[value >> 4u]; result += digits[value & 15u];
    }
    return result;
}
bool same_context(const exam::StateRequest& a, const exam::StateRequest& b) {
    return a.package_id == b.package_id && a.package_digest == b.package_digest &&
        a.client_id == b.client_id && a.session_id == b.session_id && a.candidate_id == b.candidate_id;
}
std::optional<std::string> display_name(std::string_view envelope) {
    constexpr std::string_view prefix = "nstu-name-v1:";
    if (!envelope.starts_with(prefix) || envelope.size() > 101) return std::nullopt;
    const auto encoded = envelope.substr(prefix.size());
    DWORD length = 0;
    if (encoded.empty() || !CryptStringToBinaryA(encoded.data(), static_cast<DWORD>(encoded.size()),
        CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT, nullptr, &length, nullptr, nullptr) || length == 0 || length > 64) return std::nullopt;
    std::string name(length, '\0');
    if (!CryptStringToBinaryA(encoded.data(), static_cast<DWORD>(encoded.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT,
        reinterpret_cast<BYTE*>(name.data()), &length, nullptr, nullptr)) return std::nullopt;
    DWORD canonical_length = 0;
    if (!CryptBinaryToStringA(reinterpret_cast<const BYTE*>(name.data()), length,
        CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &canonical_length)) return std::nullopt;
    std::string canonical(canonical_length, '\0');
    if (!CryptBinaryToStringA(reinterpret_cast<const BYTE*>(name.data()), length, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
        canonical.data(), &canonical_length)) return std::nullopt;
    canonical.resize(canonical_length);
    if (canonical != encoded) return std::nullopt;
    const auto wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()), nullptr, 0);
    if (wide_length <= 0) return std::nullopt;
    std::wstring wide(static_cast<std::size_t>(wide_length), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()), wide.data(), wide_length) ||
        std::any_of(wide.begin(), wide.end(), [](wchar_t c) { return c < 32 || (c >= 127 && c <= 159); })) return std::nullopt;
    return name;
}
bool same_immutable(const ImmutableRoomState& a, const ImmutableRoomState& b) {
    return a.room_id == b.room_id && a.round_id == b.round_id && a.owner_id == b.owner_id &&
        a.package_id == b.package_id && a.package_digest == b.package_digest &&
        a.grading_digest == b.grading_digest && a.runtime_identity == b.runtime_identity &&
        a.publication_policy == b.publication_policy && a.duration_seconds == b.duration_seconds;
}
contest_room::Realm realm_for(const ImmutableRoomState& room) {
    return {contest_room::kVersion, room.owner_id, room.room_id, room.round_id};
}
std::string json_string(std::string_view value) {
    std::string result = "\"";
    for (const auto c : value) {
        const auto code = static_cast<unsigned char>(c);
        if (code < 0x20) {
            constexpr char digits[] = "0123456789abcdef";
            result += "\\u00"; result += digits[code >> 4u]; result += digits[code & 15u];
            continue;
        }
        if (c == '"' || c == '\\') result += '\\';
        result += c;
    }
    return result + '"';
}
std::vector<std::byte> bytes(std::string_view value) {
    const auto data = std::as_bytes(std::span(value.data(), value.size()));
    return {data.begin(), data.end()};
}
std::string phase_name(RoomPhase phase) {
    switch (phase) {
    case RoomPhase::draft: return "draft";
    case RoomPhase::joining: return "joining";
    case RoomPhase::prepared: return "prepared";
    case RoomPhase::running: return "running";
    case RoomPhase::closed: return "closed";
    }
    return "unavailable";
}
bool joining_open(RoomPhase phase) {
    return phase == RoomPhase::draft || phase == RoomPhase::joining || phase == RoomPhase::prepared;
}
std::vector<std::byte> lifecycle(const RoomState& room) {
    return bytes("{\"phase\":" + json_string(phase_name(room.phase)) +
        ",\"revision\":" + std::to_string(room.revision) +
        ",\"serverStart\":" + std::to_string(room.server_start) + ",\"deadline\":" +
        std::to_string(room.deadline) + ",\"serverTime\":" + std::to_string(room.server_time) + "}");
}
std::vector<std::byte> join_status(const exam::StateRequest& context, const RoomState& current) {
    return bytes("{\"status\":\"approved\",\"context\":" + exam::programming::context_json(context) +
        ",\"publicationPolicy\":" + json_string(current.immutable.publication_policy == RoomPublicationPolicy::live ? "live" : "teacher_only") +
        ",\"recoveryOnly\":" + (current.phase == RoomPhase::closed ? "true" : "false") +
        ",\"revision\":" + std::to_string(current.revision) + ",\"serverTime\":" + std::to_string(current.server_time) + "}");
}
std::vector<std::byte> standings_wire(const StandingsSnapshot& standings) {
    std::string result = "{\"roomId\":" + json_string(standings.room_id) + ",\"roundId\":" +
        json_string(standings.round_id) + ",\"revision\":" + std::to_string(standings.revision) +
        ",\"generatedTime\":" + std::to_string(standings.generated_time) + ",\"freshness\":" +
        json_string(standings.freshness == StandingsFreshness::current ? "current" :
            standings.freshness == StandingsFreshness::pending ? "pending" : "unavailable") + ",\"rows\":[";
    for (std::size_t i = 0; i < standings.rows.size(); ++i) {
        const auto& row = standings.rows[i];
        if (i) result += ',';
        result += "{\"rank\":" + std::to_string(row.rank) + ",\"displayName\":" + json_string(row.display_name) +
            ",\"solved\":" + std::to_string(row.solved) + ",\"penaltyMinutes\":" + std::to_string(row.penalty_minutes) + "}";
    }
    return bytes(result + "]}");
}

// Local binary profile is wholly inside DPAPI. No key is exported as text.
struct Binding {
    contest_room::Realm realm;
    exam::StateRequest context;
    std::string uuid;
    std::string display_name;
    std::uint32_t key_id = 0;
    security::Sha256Digest key{};
    bool revoked = false;
    std::uint64_t transfer_id = 0, transfer_started = 0, transfer_sent = 0, archive_bytes = 0;
    security::Sha256Digest archive_digest{};
    ~Binding() { security::secure_zero(key); }
};
struct LifecycleWitness {
    contest_room::Realm realm;
    std::uint64_t revision = 0;
    std::uint64_t server_time = 0;
    RoomPhase phase = RoomPhase::draft;
    std::uint64_t server_start = 0;
    std::uint64_t deadline = 0;
};
LifecycleWitness witness_for(const RoomState& state) {
    return {realm_for(state.immutable), state.revision, state.server_time, state.phase, state.server_start, state.deadline};
}
bool extends_witness(const LifecycleWitness& old, const RoomState& next) {
    return old.realm == realm_for(next.immutable) && next.revision >= old.revision && next.server_time >= old.server_time &&
        (next.revision != old.revision || next.phase == old.phase) &&
        ((old.phase != RoomPhase::running && old.phase != RoomPhase::closed) ||
            (next.server_start == old.server_start && next.deadline == old.deadline)) &&
        (old.phase != RoomPhase::closed || next.phase == RoomPhase::closed) &&
        (old.phase != RoomPhase::running || next.phase == RoomPhase::running || next.phase == RoomPhase::closed);
}
struct ProfileRecords {
    std::vector<Binding> bindings;
    std::vector<LifecycleWitness> witnesses;
};
struct Writer {
    std::vector<std::byte> data;
    void number(std::uint32_t n) { for (unsigned i = 0; i < 4; ++i) data.push_back(static_cast<std::byte>((n >> (8u * i)) & 255u)); }
    void wide(std::uint64_t n) { number(static_cast<std::uint32_t>(n)); number(static_cast<std::uint32_t>(n >> 32u)); }
    void blob(std::span<const std::byte> value) {
        number(static_cast<std::uint32_t>(value.size())); data.insert(data.end(), value.begin(), value.end());
    }
    void text(std::string_view value) { blob(std::as_bytes(std::span(value.data(), value.size()))); }
};
struct Reader {
    std::span<const std::byte> data;
    std::size_t offset = 0;
    bool valid = true;
    std::uint32_t number() {
        if (data.size() - offset < 4) { valid = false; return 0; }
        std::uint32_t n = 0;
        for (unsigned i = 0; i < 4; ++i) n |= std::to_integer<std::uint32_t>(data[offset++]) << (8u * i);
        return n;
    }
    std::uint64_t wide() { const auto low = number(); return low | (static_cast<std::uint64_t>(number()) << 32u); }
    std::span<const std::byte> blob(std::size_t limit) {
        const auto n = number();
        if (!valid || n > limit || n > data.size() - offset) { valid = false; return {}; }
        const auto value = data.subspan(offset, n); offset += n; return value;
    }
    std::string text(std::size_t limit) {
        const auto value = blob(limit); return {reinterpret_cast<const char*>(value.data()), value.size()};
    }
};
std::vector<std::byte> encode_bindings(const std::vector<Binding>& bindings, const std::vector<LifecycleWitness>& witnesses) {
    Writer writer; writer.number(0x3352434eu); writer.number(static_cast<std::uint32_t>(bindings.size()));
    for (const auto& binding : bindings) {
        writer.text(binding.realm.teacher_id); writer.text(binding.realm.room_id); writer.text(binding.realm.round_id);
        writer.blob(exam::encode_state_request(binding.context)); writer.text(binding.uuid); writer.text(binding.display_name);
        writer.number(binding.key_id); writer.blob(binding.key); writer.number(binding.revoked ? 1 : 0);
        writer.wide(binding.transfer_id); writer.wide(binding.transfer_started); writer.wide(binding.transfer_sent);
        writer.wide(binding.archive_bytes); writer.blob(binding.archive_digest);
    }
    writer.number(static_cast<std::uint32_t>(witnesses.size()));
    for (const auto& witness : witnesses) {
        writer.text(witness.realm.teacher_id); writer.text(witness.realm.room_id); writer.text(witness.realm.round_id);
        writer.wide(witness.revision); writer.wide(witness.server_time); writer.number(static_cast<std::uint32_t>(witness.phase));
        writer.wide(witness.server_start); writer.wide(witness.deadline);
    }
    return writer.data;
}
std::optional<ProfileRecords> decode_bindings(std::span<const std::byte> data) {
    if (data.empty()) return ProfileRecords{};
    if (data.size() > kMaximumProfileBytes) return std::nullopt;
    Reader reader{data};
    if (reader.number() != 0x3352434eu) return std::nullopt;
    const auto count = reader.number();
    if (count > kMaximumRoomHistory) return std::nullopt;
    ProfileRecords result;
    for (std::uint32_t i = 0; i < count; ++i) {
        Binding binding;
        binding.realm.teacher_id = reader.text(96); binding.realm.room_id = reader.text(96); binding.realm.round_id = reader.text(96);
        const auto context = exam::decode_state_request(reader.blob(1024));
        binding.uuid = reader.text(128); binding.display_name = reader.text(64); binding.key_id = reader.number();
        const auto key = reader.blob(32); const auto revoked = reader.number();
        const auto id = pairing::client_id_from_uuid(binding.uuid);
        if (!reader.valid || !context || !id || *id != context->client_id || !contest_room::encode_realm(binding.realm) ||
            binding.key_id == 0 || key.size() != 32 || revoked > 1) return std::nullopt;
        binding.context = *context; std::copy(key.begin(), key.end(), binding.key.begin()); binding.revoked = revoked == 1;
        binding.transfer_id = reader.wide(); binding.transfer_started = reader.wide(); binding.transfer_sent = reader.wide();
        binding.archive_bytes = reader.wide(); const auto archive_digest = reader.blob(32);
        if (!reader.valid || archive_digest.size() != 32) return std::nullopt;
        std::copy(archive_digest.begin(), archive_digest.end(), binding.archive_digest.begin());
        const bool pin = std::any_of(binding.archive_digest.begin(), binding.archive_digest.end(), [](std::byte b) { return b != std::byte{0}; });
        if ((!binding.transfer_id && (binding.transfer_started || binding.transfer_sent || binding.archive_bytes || pin)) ||
            (binding.transfer_id && (!binding.transfer_started || binding.transfer_started > (1ULL << 53u) - 1 || !pin ||
                !binding.archive_bytes || binding.archive_bytes > contest_package::kMaximumArchiveBytes ||
                binding.transfer_sent > binding.archive_bytes ||
                (binding.transfer_sent != binding.archive_bytes && binding.transfer_sent % contest_package::kChunkDataBytes != 0)))) return std::nullopt;
        if (std::any_of(result.bindings.begin(), result.bindings.end(), [&](const Binding& old) {
            return old.realm == binding.realm && old.context.client_id == binding.context.client_id;
        })) return std::nullopt;
        result.bindings.push_back(binding);
    }
    const auto witness_count = reader.number();
    if (!reader.valid || witness_count > kMaximumRoomHistory) return std::nullopt;
    for (std::uint32_t i = 0; i < witness_count; ++i) {
        LifecycleWitness witness;
        witness.realm.teacher_id = reader.text(96); witness.realm.room_id = reader.text(96); witness.realm.round_id = reader.text(96);
        witness.revision = reader.wide(); witness.server_time = reader.wide();
        const auto phase = reader.number(); witness.server_start = reader.wide(); witness.deadline = reader.wide();
        constexpr std::uint64_t safe = (1ULL << 53u) - 1;
        if (!reader.valid || !contest_room::encode_realm(witness.realm) || phase > static_cast<std::uint32_t>(RoomPhase::closed) ||
            !witness.revision || witness.revision > safe || !witness.server_time || witness.server_time > safe ||
            witness.server_start > safe || witness.deadline > safe ||
            ((witness.server_start == 0) != (witness.deadline == 0)) ||
            (witness.server_start && (witness.deadline <= witness.server_start || witness.deadline - witness.server_start > 86400000))) return std::nullopt;
        witness.phase = static_cast<RoomPhase>(phase);
        if (std::any_of(result.witnesses.begin(), result.witnesses.end(), [&](const LifecycleWitness& old) { return old.realm == witness.realm; })) return std::nullopt;
        result.witnesses.push_back(witness);
    }
    for (const auto& binding : result.bindings)
        if (std::none_of(result.witnesses.begin(), result.witnesses.end(), [&](const LifecycleWitness& w) { return w.realm == binding.realm; })) return std::nullopt;
    if (!reader.valid || reader.offset != data.size()) return std::nullopt;
    return result;
}

struct NativeHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    NativeHandle() = default;
    explicit NativeHandle(HANDLE h) : value(h) {}
    ~NativeHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    NativeHandle(NativeHandle&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
    NativeHandle& operator=(NativeHandle&& other) noexcept {
        if (this != &other) { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); value = std::exchange(other.value, INVALID_HANDLE_VALUE); }
        return *this;
    }
    NativeHandle(const NativeHandle&) = delete;
    NativeHandle& operator=(const NativeHandle&) = delete;
};
struct UserSecurity {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    SECURITY_ATTRIBUTES attributes{};
    UserSecurity() {
        NativeHandle token; alignas(TOKEN_USER) std::array<std::byte, 1024> storage{}; DWORD needed = 0;
        HANDLE h = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &h)) return;
        token.value = h;
        LPWSTR sid = nullptr;
        if (!GetTokenInformation(h, TokenUser, storage.data(), static_cast<DWORD>(storage.size()), &needed) ||
            !ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid, &sid)) return;
        const std::wstring text = L"O:" + std::wstring(sid) + L"D:P(A;;FA;;;OW)"; LocalFree(sid);
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(text.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
            attributes = {sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    }
    ~UserSecurity() { if (descriptor) LocalFree(descriptor); }
    UserSecurity(const UserSecurity&) = delete;
    UserSecurity& operator=(const UserSecurity&) = delete;
    bool trusted(HANDLE file) const {
        PSID owner = nullptr, expected_owner = nullptr; PACL acl = nullptr, expected_acl = nullptr;
        PSECURITY_DESCRIPTOR actual = nullptr; BOOL defaulted = FALSE, present = FALSE;
        SECURITY_DESCRIPTOR_CONTROL control = 0; DWORD revision = 0;
        if (!descriptor || GetSecurityInfo(file, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
            &owner, nullptr, &acl, nullptr, &actual) != ERROR_SUCCESS) return false;
        const bool ok = owner && acl && GetSecurityDescriptorOwner(descriptor, &expected_owner, &defaulted) &&
            EqualSid(owner, expected_owner) && GetSecurityDescriptorControl(actual, &control, &revision) &&
            (control & SE_DACL_PROTECTED) && GetSecurityDescriptorDacl(descriptor, &present, &expected_acl, &defaulted) &&
            present && expected_acl && acl->AclSize == expected_acl->AclSize && std::memcmp(acl, expected_acl, acl->AclSize) == 0;
        LocalFree(actual); return ok;
    }
};
class Profile {
public:
    bool open(const ContestRoomServerConfig& config) {
        fixture_ = config.memory_fixture;
        // A teacher profile can retain previous rounds, so its protection is
        // bound to its trusted owner/root; individual records bind each realm.
        entropy_ = bytes("NSTU-TEACHER-ROOM-PROFILE-V1/" + config.room.owner_id);
        if (fixture_) return fixture_->load && fixture_->save;
        try {
            if (!config.state_root.is_absolute() || config.state_root == config.state_root.root_path() ||
                config.state_root.native().find(L'\0') != std::wstring::npos ||
                config.state_root.lexically_normal() != config.state_root) return false;
            UserSecurity security; if (!security.descriptor) return false;
            // Held directory handles disallow rename/removal, closing the
            // reparse-check/use race for the hierarchy while this profile lives.
            for (auto path = config.state_root; !path.empty(); path = path.parent_path()) {
                NativeHandle directory(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
                BY_HANDLE_FILE_INFORMATION info{};
                if (directory.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(directory.value, &info) ||
                    !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                    (path == config.state_root && !security.trusted(directory.value))) return false;
                directories_.push_back(std::move(directory));
                if (path == path.parent_path()) break;
            }
            root_ = config.state_root / L"contest-profile";
            const auto owner_root = std::filesystem::canonical(config.state_root).native();
            entropy_.push_back(std::byte{0});
            const auto root_bytes = std::as_bytes(std::span(owner_root));
            entropy_.insert(entropy_.end(), root_bytes.begin(), root_bytes.end());
            if (!CreateDirectoryW(root_.c_str(), &security.attributes) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
            NativeHandle directory(CreateFileW(root_.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            BY_HANDLE_FILE_INFORMATION info{};
            if (directory.value == INVALID_HANDLE_VALUE || !security.trusted(directory.value) ||
                !GetFileInformationByHandle(directory.value, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
            directories_.push_back(std::move(directory));
            lock_ = NativeHandle(CreateFileW((root_ / L"profile.lock").c_str(), GENERIC_READ | GENERIC_WRITE | READ_CONTROL,
                0, &security.attributes, OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            BY_HANDLE_FILE_INFORMATION lock_info{};
            return lock_.value != INVALID_HANDLE_VALUE && security.trusted(lock_.value) &&
                GetFileInformationByHandle(lock_.value, &lock_info) &&
                !(lock_info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY));
        } catch (...) { return false; }
    }
    std::optional<std::vector<std::byte>> load() {
        if (fixture_) return fixture_->load();
        UserSecurity security;
        NativeHandle file(CreateFileW((root_ / L"members.dpapi").c_str(), GENERIC_READ | READ_CONTROL,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (file.value == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_NOT_FOUND) return std::vector<std::byte>{};
            return std::nullopt;
        }
        BY_HANDLE_FILE_INFORMATION info{}; LARGE_INTEGER size{};
        if (!security.trusted(file.value) || !GetFileInformationByHandle(file.value, &info) ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
            !GetFileSizeEx(file.value, &size) || size.QuadPart <= 0 || size.QuadPart > static_cast<LONGLONG>(kMaximumProfileBytes + 4096)) return std::nullopt;
        std::vector<std::byte> blob(static_cast<std::size_t>(size.QuadPart)); DWORD read = 0;
        if (!ReadFile(file.value, blob.data(), static_cast<DWORD>(blob.size()), &read, nullptr) || read != blob.size()) return std::nullopt;
        auto wire = security::unprotect_user_secret(blob, entropy_);
        if (wire.empty()) return std::nullopt;
        return wire;
    }
    bool save(std::span<const std::byte> wire, const std::function<bool()>& active = {}) {
        const auto permitted = [&] { return !active || active(); };
        if (wire.empty() || wire.size() > kMaximumProfileBytes || !permitted()) return false;
        if (fixture_) { const bool saved = fixture_->save(wire); return saved && permitted(); }
        auto blob = security::protect_user_secret(wire, entropy_);
        std::array<std::byte, 16> nonce{};
        if (blob.empty() || !security::generate_random(nonce) || !permitted()) return false;
        const auto destination = root_ / L"members.dpapi";
        const auto temporary = root_ / ("members.tmp-" + hex(nonce));
        UserSecurity security;
        NativeHandle file(CreateFileW(temporary.c_str(), GENERIC_WRITE | READ_CONTROL, 0,
            &security.attributes, CREATE_NEW, FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (file.value == INVALID_HANDLE_VALUE || !security.trusted(file.value)) return false;
        DWORD written = 0;
        bool ok = WriteFile(file.value, blob.data(), static_cast<DWORD>(blob.size()), &written, nullptr) &&
            written == blob.size() && FlushFileBuffers(file.value);
        if (!CloseHandle(std::exchange(file.value, INVALID_HANDLE_VALUE))) ok = false;
        if (ok) {
            NativeHandle old(CreateFileW(destination.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
                nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (old.value != INVALID_HANDLE_VALUE) {
                BY_HANDLE_FILE_INFORMATION info{};
                ok = security.trusted(old.value) && GetFileInformationByHandle(old.value, &info) &&
                    !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY));
            } else ok = GetLastError() == ERROR_FILE_NOT_FOUND;
        }
        if (ok && !permitted()) ok = false;
        if (ok) ok = MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
        if (ok && !permitted()) ok = false;
        if (!ok) DeleteFileW(temporary.c_str());
        return ok;
    }
private:
    std::optional<RoomMemoryPersistenceFixture> fixture_;
    std::filesystem::path root_;
    std::vector<NativeHandle> directories_;
    NativeHandle lock_;
    std::vector<std::byte> entropy_;
};

bool absolute_normal_path(const std::filesystem::path& path) {
    return path.is_absolute() && path != path.root_path() && path.lexically_normal() == path &&
        path.native().find(L'\0') == std::wstring::npos && path.native().find_first_of(L"*?\"") == std::wstring::npos;
}
// Keep every ancestor and leaf against replacement while validating/reading.
bool hold_path(const std::filesystem::path& path, bool directory,
               std::vector<NativeHandle>& held, bool protected_leaf = false) {
    if (!absolute_normal_path(path)) return false;
    UserSecurity security;
    std::vector<std::filesystem::path> paths;
    for (auto parent = path.parent_path(); !parent.empty(); parent = parent.parent_path()) {
        paths.push_back(parent); if (parent == parent.parent_path()) break;
    }
    std::reverse(paths.begin(), paths.end()); paths.push_back(path);
    for (const auto& current : paths) {
        const bool leaf = current == path;
        NativeHandle handle(CreateFileW(current.c_str(), (leaf && !directory ? GENERIC_READ : FILE_READ_ATTRIBUTES) | READ_CONTROL,
            leaf && !directory ? FILE_SHARE_READ : FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        BY_HANDLE_FILE_INFORMATION info{};
        if (handle.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(handle.value, &info) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            static_cast<bool>(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != (!leaf || directory) ||
            (leaf && protected_leaf && !security.trusted(handle.value))) return false;
        held.push_back(std::move(handle));
    }
    return true;
}
std::optional<std::vector<std::byte>> read_held_file(HANDLE file, std::uint64_t maximum) {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || static_cast<std::uint64_t>(size.QuadPart) > maximum) return std::nullopt;
    std::vector<std::byte> result(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    if (!ReadFile(file, result.data(), static_cast<DWORD>(result.size()), &read, nullptr) || read != result.size()) return std::nullopt;
    return result;
}
std::wstring command_argument(std::wstring_view text) {
    std::wstring result = L"\""; std::size_t slashes = 0;
    for (const auto c : text) {
        if (c == L'\\') { ++slashes; continue; }
        result.append(slashes * (c == L'\"' ? 2u : 1u), L'\\'); slashes = 0;
        if (c == L'\"') result += L'\\';
        result += c;
    }
    result.append(slashes * 2u, L'\\'); return result + L'\"';
}
bool publish_archive(const RoomPublicArchiveSource& source, const contest_package::PackageOffer& offer) {
    const auto& executable = source.powershell_executable;
    // Deployment selects/pins a modern .NET runtime; candidates and PATH
    // cannot choose the executable. Hold its whole safe-owned ancestry.
    std::vector<NativeHandle> executor;
    const auto expected = contest_package::digest_from_hex(source.powershell_sha256);
    if (!expected || !hold_path(executable, false, executor)) return false;
    const auto runtime = read_held_file(executor.back().value, 64u * 1024u * 1024u);
    const auto actual = runtime ? security::sha256(*runtime) : std::nullopt;
    if (!actual || *actual != *expected) return false;
    UserSecurity security; PSID current_user = nullptr; BOOL defaulted = FALSE;
    if (!security.descriptor || !GetSecurityDescriptorOwner(security.descriptor, &current_user, &defaulted)) return false;
    PSID installer = nullptr;
    (void)ConvertStringSidToSidW(L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464", &installer);
    const auto trusted_sid = [&](PSID sid) {
        return sid && (EqualSid(sid, current_user) || IsWellKnownSid(sid, WinLocalSystemSid) || IsWellKnownSid(sid, WinBuiltinAdministratorsSid) ||
            (installer && EqualSid(sid, installer)));
    };
    bool trusted = true;
    for (std::size_t index = 0; trusted && index < executor.size(); ++index) {
        PSID owner = nullptr; PACL acl = nullptr; PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (GetSecurityInfo(executor[index].value, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
            &owner, nullptr, &acl, nullptr, &descriptor) != ERROR_SUCCESS) { trusted = false; break; }
        trusted = trusted_sid(owner) && acl;
        const DWORD writes = WRITE_DAC | WRITE_OWNER | DELETE | FILE_DELETE_CHILD | FILE_WRITE_DATA |
            FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES | GENERIC_ALL | GENERIC_WRITE |
            (index + 1 == executor.size() ? FILE_APPEND_DATA : 0);
        for (DWORD i = 0; trusted && i < acl->AceCount; ++i) {
            void* raw = nullptr;
            if (!GetAce(acl, i, &raw)) { trusted = false; break; }
            const auto* header = static_cast<const ACE_HEADER*>(raw);
            if (header->AceFlags & INHERIT_ONLY_ACE) continue;
            if (header->AceType == ACCESS_ALLOWED_ACE_TYPE) {
                const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
                if ((ace->Mask & writes) && !trusted_sid(const_cast<DWORD*>(&ace->SidStart))) trusted = false;
            } else if (header->AceType != ACCESS_DENIED_ACE_TYPE) trusted = false;
        }
        LocalFree(descriptor);
    }
    if (installer) LocalFree(installer);
    if (!trusted) return false;
    std::wstring command = command_argument(executable.native()) + L" -NoLogo -NoProfile -NonInteractive -File " +
        command_argument(source.extractor_script.native()) + L" -ArchivePath " + command_argument(source.archive.native()) +
        L" -ExpectedArchiveSha256 " + command_argument(std::wstring(offer.archive_digest.begin(), offer.archive_digest.end())) +
        L" -ExpectedContentSha256 " + command_argument(std::wstring(offer.content_digest.begin(), offer.content_digest.end())) +
        L" -Mode Publish -PackagesRoot " + command_argument(source.packages_root.native());
    if (command.size() >= 32767) return false;
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, nullptr, &startup, &process)) return false;
    NativeHandle handle(process.hProcess), thread(process.hThread);
    if (WaitForSingleObject(handle.value, 120000) != WAIT_OBJECT_0) {
        (void)TerminateProcess(handle.value, 1); (void)WaitForSingleObject(handle.value, 5000); return false;
    }
    DWORD exit = 1; return GetExitCodeProcess(handle.value, &exit) && exit == 0;
}
struct PublicArchive {
    contest_package::PackageOffer offer;
    // One bounded cache per teacher, shared by all members. Bytes come only
    // from a held real archive after helper and native public validation.
    std::vector<std::byte> data;
};
std::shared_ptr<const PublicArchive> load_public_archive(const ContestRoomServerConfig& config) {
    auto archive = std::make_shared<PublicArchive>();
    archive->offer.public_package_id = config.room.package_id;
    archive->offer.content_digest = hex(config.room.package_digest);
    if (config.archive_fixture) {
        if (!config.memory_fixture || config.public_archive || config.archive_fixture->archive.empty() ||
            config.archive_fixture->archive.size() > contest_package::kMaximumArchiveBytes) return {};
        archive->data = config.archive_fixture->archive;
    } else {
        if (!config.public_archive) return {};
        const auto& source = *config.public_archive;
        std::vector<NativeHandle> held;
        if (!hold_path(source.archive, false, held, true)) return {};
        auto data = read_held_file(held.back().value, contest_package::kMaximumArchiveBytes); if (!data) return {};
        archive->data = std::move(*data);
        if (!hold_path(source.extractor_script, false, held)) return {};
        const auto script = read_held_file(held.back().value, 256u * 1024u);
        const auto script_digest = script ? security::sha256(*script) : std::nullopt;
        // Pin the reviewed public-only extractor; changing it needs a review
        // and matching packaging/consumer pin update, never an approval bypass.
        if (!script_digest || hex(*script_digest) != "23dfce986a4d0f212def8d6c630b8488515e4a3747b9a664b921762a805403a6" ||
            !hold_path(source.packages_root, true, held, true)) return {};
        const auto digest = security::sha256(archive->data); if (!digest) return {};
        archive->offer.archive_digest = hex(*digest); archive->offer.archive_byte_count = archive->data.size();
        if (contest_package::encode_offer(archive->offer).empty() || !publish_archive(source, archive->offer)) return {};
        client::ExamHostOptions options;
        options.policy = client::ExamHostPolicy::ordinary_user;
        options.package_root = source.packages_root / archive->offer.content_digest;
        options.allowed_package_root = source.packages_root;
        options.allowed_user_data_root = config.state_root;
        options.user_data_root = config.state_root / L"contest-profile" / L"package-validation";
        options.expected_digest_hex = archive->offer.content_digest;
        exam::StateRequest context; context.package_id = config.room.package_id; context.package_digest = config.room.package_digest;
        context.client_id[0] = std::byte{1}; context.session_id[0] = std::byte{1}; context.candidate_id = "package-validation";
        options.context_json = exam::programming::context_json(context);
        client::ExamPackageReport report;
        if (!client::validate_exam_host_options(options, report) || report.digest_hex != archive->offer.content_digest) return {};
    }
    if (archive->data.empty() || archive->data.size() > contest_package::kMaximumArchiveBytes) return {};
    const auto digest = security::sha256(archive->data); if (!digest) return {};
    archive->offer.archive_digest = hex(*digest); archive->offer.archive_byte_count = archive->data.size();
    return contest_package::encode_offer(archive->offer).empty() ? nullptr : archive;
}

bool read_exact(net::TcpSocket& socket, std::span<std::byte> data, Clock::time_point deadline,
                const std::atomic_bool& running) {
    std::size_t offset = 0;
    while (offset < data.size() && running && Clock::now() < deadline) {
        if (!socket.wait_readable(50)) continue;
        const auto received = socket.receive(data.subspan(offset));
        if (received <= 0) return false;
        offset += static_cast<std::size_t>(received);
    }
    return offset == data.size();
}
std::optional<protocol::TcpFrame> read_pairing(net::TcpSocket& socket, Clock::time_point deadline,
                                             const std::atomic_bool& running) {
    std::array<std::byte, 4> prefix{};
    if (!read_exact(socket, prefix, deadline, running)) return std::nullopt;
    std::uint32_t body_size = 0;
    for (unsigned i = 0; i < 4; ++i) body_size |= std::to_integer<std::uint32_t>(prefix[i]) << (i * 8u);
    if (body_size < protocol::kCommandHeaderBytes || body_size > protocol::kCommandHeaderBytes + pairing::kMaximumPairingMessageBytes) return std::nullopt;
    std::array<std::byte, protocol::kCommandHeaderBytes> header{};
    if (!read_exact(socket, header, deadline, running)) return std::nullopt;
    const auto envelope = protocol::decode_command_header(header);
    if (!envelope || !envelope->request_id || envelope->payload_bytes > pairing::kMaximumPairingMessageBytes ||
        body_size != protocol::kCommandHeaderBytes + envelope->payload_bytes) return std::nullopt;
    std::vector<std::byte> payload(envelope->payload_bytes);
    if (!read_exact(socket, payload, deadline, running)) return std::nullopt;
    return protocol::TcpFrame{*envelope, std::move(payload)};
}
bool send_pairing(net::TcpSocket& socket, protocol::CommandType type, std::uint64_t id,
                  std::span<const std::byte> payload, Clock::time_point deadline) {
    const auto wire = protocol::encode_tcp_frame({protocol::kCommandVersion, type, static_cast<std::uint32_t>(payload.size()), id}, payload);
    return !wire.empty() && socket.send_all(wire, deadline) == static_cast<int>(wire.size());
}
} // namespace

struct ContestRoomServer::Impl {
    struct Transfer {
        contest_room::Realm realm;
        exam::StateRequest context;
        std::uint64_t request_id;
        Clock::time_point started;
        contest_package::SenderWindow sender;
        std::mutex mutex;
        std::uint64_t peer_id = 0, next_offset = 0;
        bool waiting_resume = true, prepare_queued = false, final_verified = false;
        Transfer(const Binding& binding, const PublicArchive& archive, Clock::time_point original_start)
            : realm(binding.realm), context(binding.context), request_id(binding.transfer_id), started(original_start), sender(archive.offer, started) {
            // Reconstruct only durable actual emission, then require this
            // receiver's exact offset ACK before any new bytes or readiness.
            while (next_offset < binding.transfer_sent) {
                const auto count = std::min<std::size_t>(contest_package::kChunkDataBytes, archive.data.size() - static_cast<std::size_t>(next_offset));
                const auto chunk = sender.next_chunk(std::span(archive.data).subspan(static_cast<std::size_t>(next_offset), count), started);
                if (!chunk) { sender.cancel(); return; }
                next_offset += chunk->data.size();
                if (!sender.acknowledge({chunk->digest, next_offset}, started)) { sender.cancel(); return; }
            }
        }
    };
    struct Peer {
        std::uint64_t id = 0;
        std::atomic_bool done{false};
        std::atomic_uint source_inflight{0};
        std::mutex mutex;
        std::deque<contest_room::Message> outgoing;
        std::size_t queue_bytes = 0;
        bool failed = false;
        std::jthread thread;
        bool queue(contest_room::Message message) {
            std::scoped_lock lock(mutex);
            if (done || failed || outgoing.size() >= kMaximumPeerMessages ||
                message.payload.size() > kMaximumPeerQueueBytes - queue_bytes) { failed = true; return false; }
            queue_bytes += message.payload.size();
            if (message.kind == Kind::programming_reply || message.kind == Kind::lifecycle) outgoing.push_front(std::move(message));
            else outgoing.push_back(std::move(message));
            return true;
        }
    };
    enum class Action { approve, reject, begin, finish, remove, refresh, prepare };
    struct Operation { Action action; std::uint64_t peer = 0; std::string value; exam::StateRequest context; };
    struct Pending { PendingRoomPairing display; int decision = 0; };
    explicit Impl(ProgrammingBroker& b) : broker(b) {}
    ProgrammingBroker& broker;
    net::WinsockRuntime winsock;
    ContestRoomServerConfig config;
    std::unique_ptr<Profile> profile;
    mutable std::mutex mutex;
    // Serializes broker/profile mutations, never held by UI queue/snapshot.
    std::mutex authority;
    std::optional<RoomState> state;
    std::vector<Binding> bindings;
    std::vector<LifecycleWitness> witnesses;
    std::vector<std::shared_ptr<Peer>> peers;
    std::vector<Pending> pending;
    std::deque<Operation> operations;
    std::vector<RoomPeerStatus> statuses;
    FinalAckHandler final_ack;
    std::shared_ptr<const PublicArchive> archive;
    std::vector<std::shared_ptr<Transfer>> transfers;
    std::atomic_bool running{false};
    std::jthread owner;
    contest_discovery::RoomDiscoveryResponder discovery;
    security::ReplayProtector replay;
    // Callback ownership can outlive a disconnected peer or stopped listener.
    std::shared_ptr<std::atomic_uint> source_inflight = std::make_shared<std::atomic_uint>(0);
    std::uint16_t tcp_port = 0;
    std::string error;
    std::uint64_t next_peer = 1;

    bool enqueue(Operation operation) {
        std::scoped_lock lock(mutex);
        if (!running || operations.size() >= kMaximumRoomOperations) return false;
        if ((operation.action == Action::approve || operation.action == Action::reject) &&
            std::none_of(pending.begin(), pending.end(), [&](const Pending& p) { return p.display.peer_id == operation.peer && p.decision == 0; })) return false;
        operations.push_back(std::move(operation)); return true;
    }
    void report(const char* text) { std::scoped_lock lock(mutex); error = text; }
    std::shared_ptr<Transfer> attach_transfer(const Binding& binding, std::uint64_t peer_id) {
        std::scoped_lock io(authority);
        std::shared_ptr<Transfer> transfer;
        std::vector<Binding> copy;
        {
            std::scoped_lock lock(mutex);
            if (!archive || !state || !joining_open(state->phase)) return {};
            const auto member = std::find_if(state->members.begin(), state->members.end(), [&](const RoomMember& m) { return same_context(m.context, binding.context); });
            if (member == state->members.end() || member->prepared) return {};
            const auto found = std::find_if(transfers.begin(), transfers.end(), [&](const auto& t) { return t->realm == binding.realm && same_context(t->context, binding.context); });
            if (found == transfers.end()) {
                if (transfers.size() >= kMaximumRoomMembers) return {};
                copy = bindings;
            } else transfer = *found;
        }
        if (!transfer) {
            const auto record = std::find_if(copy.begin(), copy.end(), [&](const Binding& b) { return b.realm == binding.realm && same_context(b.context, binding.context); });
            if (record == copy.end()) return {};
            const auto wall = unix_milliseconds(); if (!wall) return {};
            if (!record->transfer_id) {
                std::array<std::byte, 8> random{}; if (!security::generate_random(random)) return {};
                for (unsigned i = 0; i < 8; ++i) record->transfer_id |= std::to_integer<std::uint64_t>(random[i]) << (8u * i);
                if (!record->transfer_id || std::any_of(copy.begin(), copy.end(), [&](const Binding& b) { return &b != &*record && b.transfer_id == record->transfer_id; })) return {};
                record->transfer_started = wall; record->archive_bytes = archive->offer.archive_byte_count;
                record->archive_digest = *contest_package::digest_from_hex(archive->offer.archive_digest);
                if (!persist(copy)) { report("room transfer witness flush failed; listener stopped"); running = false; return {}; }
                { std::scoped_lock lock(mutex); bindings = copy; }
            }
            if (wall < record->transfer_started || wall - record->transfer_started >= 600000 ||
                record->archive_bytes != archive->offer.archive_byte_count ||
                record->archive_digest != *contest_package::digest_from_hex(archive->offer.archive_digest)) {
                report("room transfer expired or archive/clock witness conflicts"); return {};
            }
            transfer = std::make_shared<Transfer>(*record, *archive, Clock::now() - std::chrono::milliseconds(wall - record->transfer_started));
            { std::scoped_lock lock(mutex); transfers.push_back(transfer); }
        }
        std::scoped_lock lock(transfer->mutex);
        if (transfer->sender.offer() != archive->offer || !transfer->sender.pause(Clock::now())) return {};
        transfer->peer_id = peer_id; transfer->waiting_resume = true;
        return transfer;
    }
    bool send_archive_chunk(const std::shared_ptr<Transfer>& transfer, std::uint64_t peer_id,
                            contest_room::RoomChannel& channel) {
        if (!transfer) return true;
        std::optional<contest_package::SenderWindow> staged;
        std::optional<contest_package::PackageChunk> chunk;
        {
            std::scoped_lock lock(transfer->mutex);
            if (transfer->peer_id != peer_id) return false;
            const auto status = transfer->sender.status(Clock::now());
            if (status == contest_package::TransferStatus::expired || status == contest_package::TransferStatus::cancelled) return true;
            if (status == contest_package::TransferStatus::paused) { transfer->waiting_resume = true; return true; }
            if (transfer->waiting_resume || transfer->next_offset >= archive->data.size()) return true;
            staged = transfer->sender;
            const auto count = std::min<std::size_t>(contest_package::kChunkDataBytes, archive->data.size() - static_cast<std::size_t>(transfer->next_offset));
            chunk = staged->next_chunk(std::span(archive->data).subspan(static_cast<std::size_t>(transfer->next_offset), count), Clock::now());
        }
        if (!chunk) return true;
        // Commit emission only after the bounded actual socket send succeeds.
        if (!channel.send({Kind::package_chunk, transfer->request_id, contest_package::encode_chunk(*chunk)}, Clock::now() + std::chrono::milliseconds(500))) return false;
        {
            std::scoped_lock lock(transfer->mutex);
            if (transfer->peer_id != peer_id) return false;
            transfer->sender = std::move(*staged); transfer->next_offset = chunk->offset + chunk->data.size();
        }
        // Checkpoint actual successful emission before the next chunk. A
        // teacher restart cannot accept an ACK beyond this protected witness.
        std::scoped_lock io(authority);
        std::vector<Binding> copy; { std::scoped_lock lock(mutex); copy = bindings; }
        const auto record = std::find_if(copy.begin(), copy.end(), [&](const Binding& b) {
            return b.realm == transfer->realm && same_context(b.context, transfer->context) && b.transfer_id == transfer->request_id;
        });
        if (record == copy.end() || record->revoked) return false;
        record->transfer_sent = std::max(record->transfer_sent, chunk->offset + chunk->data.size());
        if (!persist(copy)) { report("room transfer progress flush failed; listener stopped"); running = false; return false; }
        { std::scoped_lock lock(mutex); bindings = std::move(copy); }
        return true;
    }
    bool transfer_ack(const std::shared_ptr<Transfer>& transfer, std::uint64_t peer_id,
                      const contest_room::Message& message) {
        if (!transfer || message.request_id != transfer->request_id) return false;
        std::scoped_lock lock(transfer->mutex);
        if (transfer->peer_id != peer_id || transfer->realm != realm_for(config.room)) return false;
        const auto now = Clock::now();
        if (message.kind == Kind::package_ack) {
            const auto ack = contest_package::decode_ack(message.payload); if (!ack) return false;
            if (transfer->waiting_resume) {
                const auto digest = contest_package::digest_from_hex(transfer->sender.offer().archive_digest);
                if (!digest || ack->digest != *digest || !transfer->sender.resume(archive->offer, ack->next_offset, now)) return false;
                transfer->next_offset = ack->next_offset; transfer->waiting_resume = false; return true;
            }
            return transfer->sender.acknowledge(*ack, now);
        }
        const auto prepared = contest_package::decode_prepared(message.payload, archive->offer);
        auto verified = transfer->sender;
        if (transfer->waiting_resume || !prepared || !verified.accept_prepared(*prepared, now)) return false;
        transfer->final_verified = true;
        if (transfer->prepare_queued) return true;
        // Queue only an authenticated current transfer witness, never bare ACK.
        if (!enqueue({Action::prepare, transfer->request_id, {}, transfer->context})) return false;
        transfer->prepare_queued = true; return true;
    }
    void cancel_preparation(const RoomState& prepared, const exam::StateRequest& context) {
        const auto member = std::find_if(prepared.members.begin(), prepared.members.end(), [&](const RoomMember& m) { return same_context(m.context, context); });
        const auto removed = member == prepared.members.end() ? std::nullopt : broker.remove_member(prepared, context, member->member_version);
        std::vector<Binding> copy;
        { std::scoped_lock lock(mutex); copy = bindings; }
        for (auto& binding : copy) if (binding.realm == realm_for(prepared.immutable) && same_context(binding.context, context)) binding.revoked = true;
        const bool saved = persist(copy, removed ? removed : std::optional{prepared});
        { std::scoped_lock lock(mutex); bindings = std::move(copy); }
        if (removed && saved) (void)reconcile(*removed);
        report(saved ? "room preparation cancelled; membership revoked" : "room preparation compensation/storage unavailable; owner reconciliation required");
        running = false;
    }
    bool persist(const std::vector<Binding>& records, const std::optional<RoomState>& next = std::nullopt,
                 const std::function<bool()>& active = {}) {
        std::vector<LifecycleWitness> copy;
        { std::scoped_lock lock(mutex); copy = witnesses; }
        if (next) {
            const auto found = std::find_if(copy.begin(), copy.end(), [&](const LifecycleWitness& old) { return old.realm == realm_for(next->immutable); });
            if (found == copy.end()) {
                if (copy.size() >= kMaximumRoomHistory) return false;
                copy.push_back(witness_for(*next));
            } else {
                if (!extends_witness(*found, *next)) return false;
                *found = witness_for(*next);
            }
        }
        auto wire = encode_bindings(records, copy);
        const bool ok = profile->save(wire, active); security::secure_zero(wire); return ok;
    }
    void advertise(const RoomState& room) {
        if (room.phase == RoomPhase::closed) { (void)discovery.set_beacon(std::nullopt); return; }
        contest_discovery::RoomAdvertisement advertisement{room.immutable.room_id, room.immutable.round_id,
            config.title, config.teacher_label,
            room.phase == RoomPhase::running ? contest_discovery::JoinState::join_closed : contest_discovery::JoinState::open,
            contest_room::kVersion, tcp_port, room.immutable.owner_id};
        (void)discovery.set_beacon(advertisement);
    }
    bool reconcile(const RoomState& next, bool publish = true,
                   const std::function<bool()>& publication_guard = {}) {
        std::scoped_lock lock(mutex);
        if (!same_immutable(next.immutable, config.room) || next.members.size() > kMaximumRoomMembers ||
            (state && next.revision < state->revision)) return false;
        const auto witness = std::find_if(witnesses.begin(), witnesses.end(), [&](const LifecycleWitness& old) { return old.realm == realm_for(next.immutable); });
        if ((witness != witnesses.end() && !extends_witness(*witness, next)) ||
            (witness == witnesses.end() && witnesses.size() >= kMaximumRoomHistory)) return false;
        if (state && ((state->server_start && (next.server_start != state->server_start || next.deadline != state->deadline)) ||
            (state->phase == RoomPhase::closed && next.phase != RoomPhase::closed) ||
            (state->phase == RoomPhase::running && next.phase != RoomPhase::running && next.phase != RoomPhase::closed))) return false;
        for (const auto& member : next.members) {
            const auto found = std::find_if(bindings.begin(), bindings.end(), [&](const Binding& b) {
                return !b.revoked && b.realm == realm_for(config.room) && same_context(b.context, member.context) && b.display_name == member.display_name;
            });
            if (found == bindings.end()) return false;
        }
        if (!publish) return true;
        // Stop and preparation publication share this linearization point;
        // the guard is pure state/clock validation, never broker/profile I/O.
        if (publication_guard && !publication_guard()) return false;
        for (auto& binding : bindings) {
            if (binding.realm == realm_for(config.room) && !binding.revoked &&
                std::none_of(next.members.begin(), next.members.end(), [&](const RoomMember& member) { return same_context(member.context, binding.context); }))
                binding.revoked = true;
        }
        for (auto& peer : statuses) peer.recovery_only = next.phase == RoomPhase::closed;
        std::erase_if(transfers, [&](const auto& transfer) {
            return transfer->realm != realm_for(next.immutable) ||
                std::none_of(next.members.begin(), next.members.end(), [&](const RoomMember& member) { return same_context(member.context, transfer->context); });
        });
        if (witness == witnesses.end()) witnesses.push_back(witness_for(next));
        else *witness = witness_for(next);
        state = next; return true;
    }
    std::optional<Binding> lookup(const security::ClientId& client, std::uint32_t key_id) {
        std::scoped_lock lock(mutex);
        const auto found = std::find_if(bindings.begin(), bindings.end(), [&](const Binding& binding) {
            return !binding.revoked && binding.realm == realm_for(config.room) && binding.context.client_id == client && binding.key_id == key_id &&
                state && std::any_of(state->members.begin(), state->members.end(), [&](const RoomMember& member) { return same_context(member.context, binding.context); });
        });
        if (found == bindings.end()) return std::nullopt;
        return *found;
    }
    bool admission_current(const RoomState& expected, Clock::time_point deadline) {
        std::scoped_lock lock(mutex);
        if (!running || Clock::now() >= deadline || !state || !same_immutable(state->immutable, expected.immutable) ||
            !joining_open(state->phase) || state->phase != expected.phase || state->revision != expected.revision ||
            state->server_time < expected.server_time || state->server_start != expected.server_start || state->deadline != expected.deadline ||
            state->members.size() != expected.members.size()) return false;
        for (const auto& member : expected.members)
            if (std::none_of(state->members.begin(), state->members.end(), [&](const RoomMember& actual) {
                return same_context(actual.context, member.context) && actual.display_name == member.display_name &&
                    actual.member_version == member.member_version && actual.prepared == member.prepared;
            })) return false;
        return true;
    }
    void rollback_admission(const RoomState& admitted, const Binding& binding, const RoomState& original,
                            const std::vector<Binding>& originals) {
        const auto member = std::find_if(admitted.members.begin(), admitted.members.end(), [&](const RoomMember& m) { return same_context(m.context, binding.context); });
        auto removed = member == admitted.members.end() ? std::nullopt : broker.remove_member(admitted, binding.context, member->member_version);
        { std::scoped_lock lock(mutex); bindings = originals; state = original; }
        if (removed && reconcile(*removed) && persist(originals, *removed)) {
            advertise(*removed); return;
        }
        // Retain only original keys even when broker compensation fails. A
        // later broker/member mismatch cannot manufacture missing authority.
        const bool restored = persist(originals, admitted);
        running = false;
        report(restored ? "room admission cancelled; broker compensation unavailable" : "room admission rollback storage failed; listener stopped");
    }
    bool admit(const pairing::PairingHello& hello, const pairing::PairingSecrets& secrets,
               Clock::time_point deadline, const std::function<bool(const Binding&)>& accept) {
        if (!running) return false;
        std::scoped_lock io(authority);
        RoomState current;
        std::vector<Binding> next;
        {
            std::scoped_lock lock(mutex);
            if (!running || !state || !joining_open(state->phase) ||
                state->members.size() >= kMaximumRoomMembers || bindings.size() >= kMaximumRoomHistory) return false;
            current = *state; next = bindings;
        }
        const auto id = pairing::client_id_from_uuid(hello.client_uuid);
        if (!id || std::any_of(next.begin(), next.end(), [&](const Binding& b) { return b.realm == realm_for(config.room) && b.context.client_id == *id; })) return false;
        Binding binding; binding.realm = realm_for(config.room); binding.uuid = hello.client_uuid;
        const auto name = display_name(hello.client_hostname);
        if (!name) return false;
        binding.display_name = *name; binding.context.package_id = config.room.package_id;
        binding.context.package_digest = config.room.package_digest; binding.context.client_id = *id;
        std::array<std::byte, 16> candidate{}; std::array<std::byte, 4> key_id{};
        if (!security::generate_random(binding.context.session_id) || !security::generate_random(candidate) || !security::generate_random(key_id)) return false;
        binding.context.candidate_id = "candidate-" + hex(candidate); binding.key_id = 0;
        for (unsigned i = 0; i < 4; ++i) binding.key_id |= std::to_integer<std::uint32_t>(key_id[i]) << (8u * i);
        if (!binding.key_id) return false;
        binding.key = secrets.enrolled_key;
        const auto active = [&] { return admission_current(current, deadline); };
        if (!active() || !broker.prepare_session(binding.context, "contest", hex(config.room.grading_digest)) || !active()) return false;
        auto admitted = broker.approve_member(current, binding.context, binding.display_name);
        if (!admitted) return false;
        const auto originals = next;
        if (!active() || !joining_open(admitted->phase)) { rollback_admission(*admitted, binding, current, originals); return false; }
        next.push_back(binding);
        if (!persist(next, *admitted, active) || !active()) {
            rollback_admission(*admitted, binding, current, originals); return false;
        }
        // Stop changes running under this same mutex; publication is ordered
        // against cancellation without holding it across network/disk work.
        bool published = false;
        {
            std::scoped_lock lock(mutex);
            if (running && Clock::now() < deadline && state && state->revision == current.revision && joining_open(state->phase)) {
                bindings = std::move(next); state = *admitted;
                const auto witness = std::find_if(witnesses.begin(), witnesses.end(), [&](const LifecycleWitness& old) { return old.realm == binding.realm; });
                if (witness != witnesses.end()) *witness = witness_for(*admitted);
                published = true;
            }
        }
        if (!published || !admission_current(*admitted, deadline) || !accept(binding) || !admission_current(*admitted, deadline)) {
            rollback_admission(*admitted, binding, current, originals); return false;
        }
        advertise(*admitted); return true;
    }
    bool pair(net::TcpSocket& socket, const std::shared_ptr<Peer>& peer, Clock::time_point deadline) {
        const auto hello_frame = read_pairing(socket, deadline, running);
        if (!hello_frame || hello_frame->envelope.type != protocol::CommandType::pairing_hello) return false;
        const auto hello = pairing::decode_pairing_hello(hello_frame->payload);
        auto ephemeral = pairing::EphemeralKeyPair::generate();
        const auto name = hello ? display_name(hello->client_hostname) : std::nullopt;
        if (!hello || !name || !ephemeral || hello->agreement != ephemeral->agreement()) return false;
        pairing::PairingOffer offer; offer.agreement = ephemeral->agreement();
        offer.server_public_key.assign(ephemeral->public_key().begin(), ephemeral->public_key().end());
        if (!security::generate_random(offer.server_nonce)) return false;
        const auto transcript = pairing::make_transcript(*hello, offer);
        auto shared = ephemeral->agree(hello->client_public_key);
        if (!transcript || !shared) return false;
        auto secrets = contest_room::derive_room_secrets(*shared, realm_for(config.room), *transcript);
        security::secure_zero(*shared); ephemeral.reset();
        if (!secrets) return false;
        struct Wipe { pairing::PairingSecrets& value; ~Wipe() { pairing::secure_zero(value); } } wipe{*secrets};
        {
            std::scoped_lock lock(mutex);
            if (pending.size() >= kMaximumRoomMembers || !state || !joining_open(state->phase)) return false;
            pending.push_back({{peer->id, hello->client_uuid, *name, secrets->short_authentication_string}, 0});
        }
        const auto cleanup = [&] { std::scoped_lock lock(mutex); std::erase_if(pending, [&](const Pending& p) { return p.display.peer_id == peer->id; }); };
        if (!send_pairing(socket, protocol::CommandType::pairing_offer, hello_frame->envelope.request_id,
            pairing::encode_pairing_offer(offer), deadline)) { cleanup(); return false; }
        // Student confirmation is a role-bound proof; teacher approval is a
        // separate queued decision and is never an automatic tag echo.
        const auto confirmation = read_pairing(socket, deadline, running);
        const auto confirm = confirmation ? pairing::decode_pairing_confirm(confirmation->payload) : std::nullopt;
        if (!confirmation || confirmation->envelope.type != protocol::CommandType::pairing_confirm ||
            confirmation->envelope.request_id != hello_frame->envelope.request_id || !confirm ||
            !pairing::verify_confirmation_tag(*secrets, *transcript, pairing::ConfirmationRole::client, confirm->client_tag)) {
            (void)send_pairing(socket, protocol::CommandType::pairing_reject, hello_frame->envelope.request_id,
                pairing::encode_pairing_reject(pairing::PairingRejectReason::confirmation_failed), Clock::now() + std::chrono::milliseconds(500));
            cleanup(); return false;
        }
        int decision = 0;
        while (running && Clock::now() < deadline && decision == 0) {
            { std::scoped_lock lock(mutex);
              const auto p = std::find_if(pending.begin(), pending.end(), [&](const Pending& item) { return item.display.peer_id == peer->id; });
              decision = p == pending.end() ? -1 : p->decision; }
            if (!decision) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        cleanup();
        if (decision != 1 || !running || Clock::now() >= deadline) {
            (void)send_pairing(socket, protocol::CommandType::pairing_reject, hello_frame->envelope.request_id,
                pairing::encode_pairing_reject(decision < 0 ? pairing::PairingRejectReason::operator_declined : pairing::PairingRejectReason::timed_out),
                Clock::now() + std::chrono::milliseconds(500)); return false;
        }
        const auto tag = pairing::confirmation_tag(*secrets, *transcript, pairing::ConfirmationRole::server);
        if (!tag) return false;
        return admit(*hello, *secrets, deadline, [&](const Binding& binding) {
            return send_pairing(socket, protocol::CommandType::pairing_accept, hello_frame->envelope.request_id,
                pairing::encode_pairing_accept({binding.key_id, *tag}), std::min(deadline, Clock::now() + std::chrono::milliseconds(500)));
        });
    }
    void serve(net::TcpSocket socket, std::shared_ptr<Peer> peer) {
        struct Done { std::shared_ptr<Peer> peer; ~Done() { peer->done = true; } } done{peer};
        try {
            (void)socket.set_io_timeouts(500, 500);
            const auto deadline = Clock::now() + config.pairing_timeout;
            const auto realm = realm_for(config.room);
            const auto preface = contest_room::receive_preface(socket, realm, contest_room::Side::candidate, std::min(deadline, Clock::now() + std::chrono::seconds(2)));
            if (!preface || !contest_room::send_preface(socket, realm, contest_room::Side::teacher, preface->nonce, Clock::now() + std::chrono::seconds(1))) return;
            // Peek does not steal bytes from the existing authenticated handshake.
            std::array<std::byte, 4> prefix{}; int peeked = 0;
            while (running && Clock::now() < deadline && peeked < 4) {
                if (!socket.wait_readable(50)) continue;
                peeked = recv(static_cast<SOCKET>(socket.native_handle()), reinterpret_cast<char*>(prefix.data()), 4, MSG_PEEK);
                if (peeked <= 0) return;
                if (peeked < 4) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (peeked != 4) return;
            const auto preamble = protocol::encode_connection_preamble({});
            const bool resume = preamble.size() >= 4 && std::equal(prefix.begin(), prefix.end(), preamble.begin());
            if (!resume && !pair(socket, peer, deadline)) return;
            const control::KeyResolver resolver = [&](const security::ClientId& id, std::uint32_t key_id) -> std::optional<std::vector<std::byte>> {
                const auto binding = lookup(id, key_id);
                if (!binding) return std::nullopt;
                return std::vector<std::byte>(binding->key.begin(), binding->key.end());
            };
            auto session = control::server_handshake(socket, resolver, replay, unix_now());
            if (!session || !session->aead_enabled || !running) return;
            const auto binding = lookup(session->client_id, session->key_id);
            if (!binding) return;
            { std::scoped_lock lock(mutex); statuses.push_back({peer->id, binding->context, true, state && state->phase == RoomPhase::closed}); }
            struct StatusCleanup {
                Impl& owner; std::uint64_t peer;
                ~StatusCleanup() { std::scoped_lock lock(owner.mutex); std::erase_if(owner.statuses, [&](const RoomPeerStatus& status) { return status.peer_id == peer; }); }
            } status_cleanup{*this, peer->id};
            const auto native = static_cast<SOCKET>(socket.native_handle());
            contest_room::RoomChannel channel(std::move(socket), std::move(*session), realm, contest_room::Side::teacher);
            if (!channel.is_open()) return;
            RoomState current;
            { std::scoped_lock lock(mutex); current = *state; }
            if (!channel.send({Kind::join_status, 0, join_status(binding->context, current)}, Clock::now() + std::chrono::seconds(1)) ||
                !channel.send({Kind::lifecycle, 0, lifecycle(current)}, Clock::now() + std::chrono::seconds(1))) return;
            auto transfer = attach_transfer(*binding, peer->id);
            struct TransferCleanup {
                std::shared_ptr<Transfer> transfer; std::uint64_t peer_id;
                ~TransferCleanup() {
                    if (!transfer) return;
                    std::scoped_lock lock(transfer->mutex);
                    if (transfer->peer_id == peer_id) {
                        (void)transfer->sender.pause(Clock::now()); transfer->peer_id = 0;
                    }
                }
            } transfer_cleanup{transfer, peer->id};
            if (transfer && !channel.send({Kind::package_offer, transfer->request_id, contest_package::encode_offer(archive->offer)}, Clock::now() + std::chrono::seconds(1))) return;
            auto last_activity = Clock::now(); auto last_standings = Clock::time_point{};
            std::uint64_t last_revision = current.revision;
            while (running && Clock::now() - last_activity < config.peer_timeout) {
                if (!lookup(binding->context.client_id, binding->key_id)) return;
                { std::scoped_lock lock(mutex); current = *state; }
                if (transfer && !joining_open(current.phase)) {
                    std::scoped_lock lock(transfer->mutex); transfer->sender.cancel();
                }
                if (current.revision != last_revision) {
                    last_revision = current.revision;
                    if (!channel.send({Kind::lifecycle, 0, lifecycle(current)}, Clock::now() + std::chrono::milliseconds(500))) return;
                }
                std::deque<contest_room::Message> outgoing;
                { std::scoped_lock lock(peer->mutex); if (peer->failed) return; outgoing.swap(peer->outgoing); peer->queue_bytes = 0; }
                for (auto& message : outgoing) {
                    { std::scoped_lock lock(mutex); current = *state; }
                    if (message.kind == Kind::lifecycle) message.payload = lifecycle(current);
                    if (message.kind == Kind::join_status) message.payload = join_status(binding->context, current);
                    if (!channel.send(message, Clock::now() + std::chrono::milliseconds(500))) return;
                }
                fd_set readable; FD_ZERO(&readable); FD_SET(native, &readable);
                timeval wait{0, 50000};
                const auto ready = select(0, &readable, nullptr, nullptr, &wait);
                if (ready == SOCKET_ERROR) return;
                if (!ready) {
                    if (joining_open(current.phase) && !send_archive_chunk(transfer, peer->id, channel)) return;
                    continue;
                }
                auto message = channel.receive(Clock::now() + std::chrono::milliseconds(500));
                if (!message) return;
                last_activity = Clock::now();
                { std::scoped_lock lock(mutex); current = *state; }
                if (message->kind == Kind::heartbeat || message->kind == Kind::join) {
                    if (!channel.send({Kind::join_status, message->request_id, join_status(binding->context, current)}, Clock::now() + std::chrono::milliseconds(500))) return;
                } else if (message->kind == Kind::leave) return;
                else if (message->kind == Kind::programming_request) {
                    const auto request = exam::programming::decode_request(message->payload);
                    if (!request || request->workstation_policy_ready || !same_context(request->context, binding->context)) return;
                    const bool recovery = current.phase == RoomPhase::closed;
                    if (recovery && request->operation != exam::programming::Operation::draft &&
                        request->operation != exam::programming::Operation::submit && request->operation != exam::programming::Operation::recover &&
                        request->operation != exam::programming::Operation::finalize) return;
                    const auto unavailable = [&] {
                        return channel.send({Kind::programming_reply, message->request_id,
                            bytes(exam::programming::unavailable_response_json(*request, "broker_unavailable"))},
                            Clock::now() + std::chrono::milliseconds(500));
                    };
                    if (peer->source_inflight.fetch_add(1) >= 4) {
                        --peer->source_inflight;
                        if (!unavailable()) return;
                        continue;
                    }
                    auto quota = source_inflight;
                    if (quota->fetch_add(1) >= 64) {
                        --*quota; --peer->source_inflight;
                        if (!unavailable()) return;
                        continue;
                    }
                    std::weak_ptr<Peer> weak = peer;
                    const bool queued = broker.submit(*request,
                        [weak, quota, expected = *request, id = message->request_id](std::string reply) {
                            --*quota;
                            if (auto destination = weak.lock()) {
                                --destination->source_inflight;
                                const auto response = exam::programming::parse_response_json(reply, &expected);
                                const auto safe = response ? response->json : exam::programming::unavailable_response_json(expected, "broker_unavailable");
                                (void)destination->queue({Kind::programming_reply, id, bytes(safe)});
                            }
                        }, recovery);
                    if (!queued) {
                        --*quota; --peer->source_inflight;
                        if (!unavailable()) return;
                    }
                } else if (message->kind == Kind::standings_request) {
                    if (current.immutable.publication_policy != RoomPublicationPolicy::live || Clock::now() - last_standings < std::chrono::seconds(5)) continue;
                    last_standings = Clock::now();
                    const auto standings = broker.room_standings(current.immutable);
                    if (standings && !channel.send({Kind::standings, message->request_id, standings_wire(*standings)}, Clock::now() + std::chrono::milliseconds(500))) return;
                } else if (message->kind == Kind::prepared || message->kind == Kind::package_ack) {
                    if (!joining_open(current.phase)) return;
                    if (archive && (!transfer || !same_context(transfer->context, binding->context) || !transfer_ack(transfer, peer->id, *message))) return;
                    if (message->kind == Kind::package_ack) continue;
                    if (message->payload.size() != security::kSha256Bytes ||
                        !security::constant_time_equal(message->payload, config.room.package_digest)) return;
                    FinalAckHandler handler;
                    { std::scoped_lock lock(mutex); handler = final_ack; }
                    if (handler) handler(peer->id, binding->context, config.room.package_digest);
                } else return;
                if (joining_open(current.phase) && !send_archive_chunk(transfer, peer->id, channel)) return;
            }
        } catch (...) { report("room peer failed; no authority granted"); }
    }
    void operate(Operation op) {
        if (op.action == Action::approve || op.action == Action::reject) {
            std::scoped_lock lock(mutex);
            const auto p = std::find_if(pending.begin(), pending.end(), [&](const Pending& item) { return item.display.peer_id == op.peer; });
            if (p != pending.end() && !p->decision)
                p->decision = op.action == Action::approve && p->display.sas == op.value ? 1 : -1;
            return;
        }
        std::scoped_lock io(authority);
        RoomState current; { std::scoped_lock lock(mutex); if (!state) return; current = *state; }
        std::optional<RoomState> next;
        std::optional<Clock::time_point> preparation_started;
        if (op.action == Action::begin) next = broker.begin_room(current, op.value);
        else if (op.action == Action::finish) next = broker.stop_room(current, op.value);
        else if (op.action == Action::refresh) next = broker.room_state(current.immutable);
        else if (op.action == Action::remove) {
            const auto member = std::find_if(current.members.begin(), current.members.end(), [&](const RoomMember& m) { return same_context(m.context, op.context); });
            if (member != current.members.end()) next = broker.remove_member(current, op.context, member->member_version);
        } else if (op.action == Action::prepare) {
            if (!joining_open(current.phase) || !running || !archive) return;
            std::shared_ptr<Transfer> transfer;
            { std::scoped_lock lock(mutex);
              const auto found = std::find_if(transfers.begin(), transfers.end(), [&](const auto& t) {
                  return t->request_id == op.peer && t->realm == realm_for(current.immutable) && same_context(t->context, op.context);
              });
              if (found != transfers.end()) transfer = *found;
            }
            if (!transfer) return;
            {
                std::scoped_lock lock(transfer->mutex);
                const auto status = transfer->sender.status(Clock::now());
                if (Clock::now() - transfer->started >= contest_package::kTransferBudget ||
                    (status != contest_package::TransferStatus::complete && status != contest_package::TransferStatus::paused) ||
                    transfer->sender.acknowledged_bytes() != archive->offer.archive_byte_count ||
                    transfer->sender.offer() != archive->offer || !transfer->prepare_queued || !transfer->final_verified) return;
                preparation_started = transfer->started;
            }
            const auto member = std::find_if(current.members.begin(), current.members.end(), [&](const RoomMember& m) { return same_context(m.context, op.context); });
            if (member == current.members.end() || member->prepared) return;
            next = broker.prepare_member(current, op.context, member->member_version);
            if (!running || Clock::now() - transfer->started >= contest_package::kTransferBudget) {
                if (next) cancel_preparation(*next, op.context);
                else { report("room preparation cancelled; listener stopped without readiness publication"); running = false; }
                return;
            }
        }
        if (!next) {
            if (op.action == Action::prepare) {
                std::shared_ptr<Transfer> transfer;
                { std::scoped_lock lock(mutex);
                  const auto found = std::find_if(transfers.begin(), transfers.end(), [&](const auto& t) { return t->request_id == op.peer; });
                  if (found != transfers.end()) transfer = *found;
                }
                if (transfer) { std::scoped_lock lock(transfer->mutex); transfer->prepare_queued = false; }
            }
            report("room broker state is unavailable"); return;
        }
        if (!reconcile(*next, false)) { report("room broker state conflicts with protected membership; listener stopped"); running = false; return; }
        std::vector<Binding> copy; { std::scoped_lock lock(mutex); copy = bindings; }
        for (auto& binding : copy) {
            if (binding.realm == realm_for(next->immutable) && !binding.revoked &&
                std::none_of(next->members.begin(), next->members.end(), [&](const RoomMember& member) { return same_context(member.context, binding.context); })) binding.revoked = true;
        }
        // Publish lifecycle only after its witness/revocations are durable;
        // a queued finish or a failed flush is never a successful Closed ACK.
        const auto active = [&] {
            return running.load() && (!preparation_started || Clock::now() - *preparation_started < contest_package::kTransferBudget);
        };
        const bool can_publish = persist(copy, *next, op.action == Action::prepare ? std::function<bool()>(active) : std::function<bool()>{}) &&
            (op.action != Action::prepare || active());
        if (can_publish && op.action == Action::prepare && config.memory_fixture && config.memory_fixture->before_preparation_publication)
            config.memory_fixture->before_preparation_publication();
        const auto publication_active = [&] {
            return active() && state && state->revision == current.revision && state->phase == current.phase &&
                joining_open(state->phase) && same_immutable(state->immutable, current.immutable);
        };
        if (!can_publish || !reconcile(*next, true,
                op.action == Action::prepare ? std::function<bool()>(publication_active) : std::function<bool()>{})) {
            if (op.action == Action::prepare) cancel_preparation(*next, op.context);
            else { report("room state binding flush failed; listener stopped"); running = false; }
            return;
        }
        advertise(*next);
    }
    void run(net::TcpSocket listener) {
        try {
            std::deque<Clock::time_point> attempts;
            while (running) {
                Operation op{Action::refresh, 0, {}, {}}; bool have = false;
                { std::scoped_lock lock(mutex); if (!operations.empty()) { op = std::move(operations.front()); operations.pop_front(); have = true; } }
                if (have) operate(std::move(op));
                std::vector<std::shared_ptr<Peer>> retired;
                { std::scoped_lock lock(mutex);
                  for (auto it = peers.begin(); it != peers.end();) {
                      if ((*it)->done) { retired.push_back(*it); it = peers.erase(it); } else ++it;
                  } }
                for (auto& peer : retired) if (peer->thread.joinable()) peer->thread.join();
                retired.clear(); // Join outside the control mutex.
                if (!running || !listener.wait_readable(20)) continue;
                auto socket = listener.accept(); if (!socket.is_open()) continue;
                const auto now = Clock::now();
                while (!attempts.empty() && now - attempts.front() >= std::chrono::minutes(1)) attempts.pop_front();
                if (attempts.size() >= kMaximumRoomMembers) continue;
                attempts.push_back(now);
                auto peer = std::make_shared<Peer>();
                { std::scoped_lock lock(mutex); if (peers.size() >= kMaximumRoomMembers) continue; peer->id = next_peer++; peers.push_back(peer); }
                peer->thread = std::jthread([this, peer, socket = std::move(socket)]() mutable { serve(std::move(socket), peer); });
            }
        } catch (...) { report("room listener failed"); running = false; }
        listener.close();
        std::vector<std::shared_ptr<Peer>> retired;
        { std::scoped_lock lock(mutex); retired.swap(peers); }
        for (auto& peer : retired) if (peer->thread.joinable()) peer->thread.join();
    }
};

ContestRoomServer::ContestRoomServer(ProgrammingBroker& broker) : impl_(std::make_unique<Impl>(broker)) {}
ContestRoomServer::~ContestRoomServer() { stop(); }
bool ContestRoomServer::start(ContestRoomServerConfig config, std::string* error) {
    stop();
    const auto realm = realm_for(config.room);
    contest_discovery::RoomAdvertisement advertisement{config.room.room_id, config.room.round_id, config.title,
        config.teacher_label, contest_discovery::JoinState::open, contest_room::kVersion, config.tcp_port ? config.tcp_port : std::uint16_t{1}, config.room.owner_id};
    contest_discovery::Probe probe;
    if (!security::generate_random(probe.nonce) || !impl_->broker.configured() || !contest_room::encode_realm(realm) || contest_discovery::encode_reply(probe, advertisement).empty() ||
        config.pairing_timeout < std::chrono::milliseconds(100) || config.pairing_timeout > std::chrono::seconds(60) ||
        config.peer_timeout < std::chrono::milliseconds(100) || config.peer_timeout > std::chrono::seconds(60)) {
        fail(error, "room listener configuration is invalid"); return false;
    }
    impl_->config = std::move(config); impl_->profile = std::make_unique<Profile>();
    if (!impl_->profile->open(impl_->config)) { fail(error, "room protected owner storage is unavailable"); impl_->profile.reset(); return false; }
    const auto archive = load_public_archive(impl_->config);
    if ((!impl_->config.memory_fixture || impl_->config.public_archive || impl_->config.archive_fixture) && !archive) {
        fail(error, "room public archive validation/cache is unavailable"); impl_->profile.reset(); return false;
    }
    { std::scoped_lock lock(impl_->mutex);
      if (!impl_->archive || !archive || impl_->archive->offer != archive->offer) impl_->transfers.clear();
      impl_->archive = archive;
    }
    auto wire = impl_->profile->load();
    if (!wire) { fail(error, "room protected bindings could not be loaded"); impl_->profile.reset(); return false; }
    auto bindings = decode_bindings(*wire); security::secure_zero(*wire);
    if (!bindings) { fail(error, "room protected bindings are invalid"); impl_->profile.reset(); return false; }
    { std::scoped_lock lock(impl_->mutex); impl_->bindings = std::move(bindings->bindings); impl_->witnesses = std::move(bindings->witnesses); impl_->state.reset(); impl_->error.clear(); }
    auto state = impl_->broker.room_state(impl_->config.room);
    if (!state) {
        const bool existing_realm = std::any_of(impl_->bindings.begin(), impl_->bindings.end(), [&](const Binding& b) { return b.realm == realm; }) ||
            std::any_of(impl_->witnesses.begin(), impl_->witnesses.end(), [&](const LifecycleWitness& w) { return w.realm == realm; });
        if (!existing_realm) state = impl_->broker.create_room(impl_->config.room);
    }
    if (!state || !impl_->reconcile(*state) || !impl_->persist(impl_->bindings)) {
        fail(error, "room broker reconciliation failed; retained protected bindings"); impl_->profile.reset(); return false;
    }
    net::TcpSocket listener;
    if (!listener.listen(impl_->config.tcp_port) || !impl_->discovery.start(impl_->config.udp_port)) {
        fail(error, "room listener endpoint is unavailable"); impl_->discovery.stop(); impl_->profile.reset(); return false;
    }
    { std::scoped_lock lock(impl_->mutex); impl_->tcp_port = listener.local_port(); }
    impl_->advertise(*state); impl_->running = true;
    impl_->owner = std::jthread([this, listener = std::move(listener)]() mutable { impl_->run(std::move(listener)); });
    return true;
}
void ContestRoomServer::stop() noexcept {
    { std::scoped_lock lock(impl_->mutex); impl_->running = false; }
    if (impl_->owner.joinable()) impl_->owner.join();
    impl_->discovery.stop();
    { std::scoped_lock lock(impl_->mutex); impl_->pending.clear(); impl_->statuses.clear(); impl_->operations.clear(); impl_->tcp_port = 0; }
    impl_->profile.reset();
}
ContestRoomSnapshot ContestRoomServer::snapshot() const {
    std::scoped_lock lock(impl_->mutex);
    ContestRoomSnapshot snapshot{impl_->running, impl_->tcp_port, impl_->discovery.local_port(), impl_->state, {}, impl_->statuses, impl_->error};
    for (const auto& p : impl_->pending) snapshot.pending.push_back(p.display);
    return snapshot;
}
bool ContestRoomServer::approve(std::uint64_t peer_id, std::string_view sas) {
    if (sas.size() != pairing::kSasDigits || !std::all_of(sas.begin(), sas.end(), [](char c) { return c >= '0' && c <= '9'; })) return false;
    return impl_->enqueue({Impl::Action::approve, peer_id, std::string(sas), {}});
}
bool ContestRoomServer::reject(std::uint64_t peer_id) { return impl_->enqueue({Impl::Action::reject, peer_id, {}, {}}); }
bool ContestRoomServer::begin(std::string_view request_id) {
    if (request_id.empty() || request_id.size() > 96) return false;
    return impl_->enqueue({Impl::Action::begin, 0, std::string(request_id), {}});
}
bool ContestRoomServer::finish(std::string_view request_id) {
    if (request_id.empty() || request_id.size() > 96) return false;
    return impl_->enqueue({Impl::Action::finish, 0, std::string(request_id), {}});
}
bool ContestRoomServer::remove(const exam::StateRequest& member) {
    if (exam::encode_state_request(member).empty()) return false;
    return impl_->enqueue({Impl::Action::remove, 0, {}, member});
}
bool ContestRoomServer::refresh() { return impl_->enqueue({Impl::Action::refresh, 0, {}, {}}); }
std::optional<RoomPublicPackagePin> ContestRoomServer::public_package_pin() const {
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->running || !impl_->state) return std::nullopt;
    return RoomPublicPackagePin{realm_for(impl_->state->immutable), impl_->state->immutable.package_id, impl_->state->immutable.package_digest};
}
bool ContestRoomServer::queue_message(std::uint64_t id, contest_room::Message message) {
    std::shared_ptr<Impl::Peer> peer;
    { std::scoped_lock lock(impl_->mutex);
      if (!impl_->running || !impl_->state || !contest_room::allowed(contest_room::Side::teacher, message.kind) ||
          (impl_->archive && (message.kind == Kind::package_offer || message.kind == Kind::package_chunk)) ||
          contest_room::encode_message(realm_for(impl_->state->immutable), contest_room::Side::teacher, message).empty()) return false;
      const auto found = std::find_if(impl_->peers.begin(), impl_->peers.end(), [&](const auto& p) { return p->id == id; });
      if (found == impl_->peers.end() || std::none_of(impl_->statuses.begin(), impl_->statuses.end(), [&](const RoomPeerStatus& status) { return status.peer_id == id && status.connected; })) return false;
      peer = *found; }
    return peer->queue(std::move(message));
}
void ContestRoomServer::set_final_ack_handler(FinalAckHandler handler) { std::scoped_lock lock(impl_->mutex); impl_->final_ack = std::move(handler); }
} // namespace nstu::server
