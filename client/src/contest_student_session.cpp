#include "nstu/contest_student_session.hpp"
#include "nstu/programming_outbox.hpp"
#include "nstu/secret_store.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <wincrypt.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

namespace nstu::client {
namespace {
using Clock = std::chrono::steady_clock;
using Request = exam::programming::Request;
using Operation = exam::programming::Operation;
using Kind = contest_room::Kind;
constexpr auto kSafeInteger = exam::programming::kMaximumDraftRevision;
constexpr std::size_t kQueueLimit = 16, kDurableLimit = 256;
void fail(std::string* error, const char* message) { if (error) *error = message; }
std::string hex(std::span<const std::byte> data) {
    constexpr char digits[] = "0123456789abcdef"; std::string result;
    for (const auto b : data) { const auto n = std::to_integer<unsigned>(b); result += digits[n >> 4u]; result += digits[n & 15u]; }
    return result;
}
std::vector<std::byte> bytes(std::string_view text) {
    const auto wire = std::as_bytes(std::span(text)); return {wire.begin(), wire.end()};
}
bool same_context(const exam::StateRequest& a, const exam::StateRequest& b) {
    return a.package_id == b.package_id && a.package_digest == b.package_digest && a.client_id == b.client_id &&
        a.session_id == b.session_id && a.candidate_id == b.candidate_id;
}
bool durable_operation(Operation op) { return op == Operation::draft || op == Operation::submit || op == Operation::finalize; }
bool recovery_operation(Operation op) { return op == Operation::draft || op == Operation::submit || op == Operation::recover || op == Operation::finalize; }
bool valid_label(std::string_view text) {
    if (text.empty() || text.size() > 64) return false;
    const auto n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!n) return false;
    std::wstring wide(static_cast<std::size_t>(n), L'\0');
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), wide.data(), n) &&
        std::none_of(wide.begin(), wide.end(), [](wchar_t c) { return c < 32 || (c >= 127 && c <= 159); });
}
template <std::size_t N> bool unhex(std::string_view text, std::array<std::byte, N>& value) {
    if (text.size() != N * 2) return false;
    for (std::size_t i = 0; i < N; ++i) {
        unsigned n = 0;
        for (unsigned p = 0; p < 2; ++p) { const auto c = text[i * 2 + p];
            if (c >= '0' && c <= '9') n = n * 16 + static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') n = n * 16 + static_cast<unsigned>(c - 'a' + 10); else return false; }
        value[i] = static_cast<std::byte>(n);
    }
    return true;
}
struct Json {
    enum class Type { object, array, text, number, boolean } type = Type::object;
    std::map<std::string, Json> fields;
    std::vector<Json> elements;
    std::string text;
    std::uint64_t number = 0;
    bool boolean = false;
};
// Bounded room metadata reader: no null, floating point or unknown authority.
class JsonReader {
public:
    explicit JsonReader(std::string_view text) : text_(text) {}
    std::optional<Json> parse() { Json j; if (text_.empty() || text_.size() > contest_room::kMaximumSnapshotBytes || !value(j, 0)) return {}; ws(); return pos_ == text_.size() ? std::optional{j} : std::nullopt; }
private:
    void ws() { while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\r' || text_[pos_] == '\n')) ++pos_; }
    bool take(char c) { ws(); if (pos_ == text_.size() || text_[pos_] != c) return false; ++pos_; return true; }
    bool string(std::string& result) {
        if (!take('"')) return false;
        while (pos_ < text_.size()) {
            const auto c = static_cast<unsigned char>(text_[pos_++]);
            if (c == '"') return result.find('\0') == std::string::npos && (result.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, result.data(), static_cast<int>(result.size()), nullptr, 0));
            if (c < 32 || result.size() > 4096) return false;
            if (c != '\\') { result += static_cast<char>(c); continue; }
            if (pos_ == text_.size()) return false;
            const auto escape = text_[pos_++];
            if (escape == '"' || escape == '\\' || escape == '/') result += escape;
            else if (escape == 'n') result += '\n'; else if (escape == 'r') result += '\r'; else if (escape == 't') result += '\t';
            else if (escape == 'u') {
                const auto point = [&]() -> std::optional<unsigned> { if (text_.size() - pos_ < 4) return {}; unsigned n = 0;
                    for (unsigned i = 0; i < 4; ++i) { const char d = text_[pos_++]; n <<= 4u; if (d >= '0' && d <= '9') n |= static_cast<unsigned>(d - '0');
                        else if (d >= 'a' && d <= 'f') n |= static_cast<unsigned>(d - 'a' + 10); else if (d >= 'A' && d <= 'F') n |= static_cast<unsigned>(d - 'A' + 10); else return {}; } return n; };
                auto p = point(); if (!p || !*p) return false;
                if (*p >= 0xd800 && *p <= 0xdbff) { if (text_.substr(pos_, 2) != "\\u") return false; pos_ += 2; auto low = point();
                    if (!low || *low < 0xdc00 || *low > 0xdfff) return false;
                    *p = 0x10000 + ((*p - 0xd800) << 10u) + *low - 0xdc00; }
                else if (*p >= 0xdc00 && *p <= 0xdfff) return false;
                if (*p < 0x80) result += static_cast<char>(*p);
                else if (*p < 0x800) { result += static_cast<char>(0xc0 | (*p >> 6u)); result += static_cast<char>(0x80 | (*p & 63u)); }
                else if (*p < 0x10000) { result += static_cast<char>(0xe0 | (*p >> 12u)); result += static_cast<char>(0x80 | ((*p >> 6u) & 63u)); result += static_cast<char>(0x80 | (*p & 63u)); }
                else { result += static_cast<char>(0xf0 | (*p >> 18u)); result += static_cast<char>(0x80 | ((*p >> 12u) & 63u)); result += static_cast<char>(0x80 | ((*p >> 6u) & 63u)); result += static_cast<char>(0x80 | (*p & 63u)); }
            } else return false;
        }
        return false;
    }
    bool value(Json& j, unsigned depth) {
        if (depth > 4 || ++nodes_ > 512) return false;
        ws(); if (pos_ == text_.size()) return false;
        if (text_[pos_] == '{') { ++pos_; j.type = Json::Type::object; if (take('}')) return true;
            do { std::string key; Json field; if (j.fields.size() >= 16 || !string(key) || key.size() > 64 || !take(':') || !value(field, depth + 1) || !j.fields.emplace(key, std::move(field)).second) return false;
                if (take('}')) return true; } while (take(',')); return false; }
        if (text_[pos_] == '[') { ++pos_; j.type = Json::Type::array; if (take(']')) return true;
            do { Json item; if (j.elements.size() >= 64 || !value(item, depth + 1)) return false; j.elements.push_back(std::move(item)); if (take(']')) return true; } while (take(',')); return false; }
        if (text_[pos_] == '"') { j.type = Json::Type::text; return string(j.text); }
        if (text_.substr(pos_, 4) == "true") { pos_ += 4; j.type = Json::Type::boolean; j.boolean = true; return true; }
        if (text_.substr(pos_, 5) == "false") { pos_ += 5; j.type = Json::Type::boolean; return true; }
        const auto begin = pos_; while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        if (begin == pos_ || (text_[begin] == '0' && pos_ != begin + 1)) return false;
        j.type = Json::Type::number; const auto result = std::from_chars(text_.data() + begin, text_.data() + pos_, j.number);
        return result.ec == std::errc{} && j.number <= kSafeInteger;
    }
    std::string_view text_; std::size_t pos_ = 0, nodes_ = 0;
};
bool fields(const Json& j, std::string_view allowed, std::size_t count) {
    return j.type == Json::Type::object && j.fields.size() == count && std::all_of(j.fields.begin(), j.fields.end(), [&](const auto& f) { return allowed.find("|" + f.first + "|") != std::string_view::npos; });
}
const Json* field(const Json& j, const char* key, Json::Type type) { const auto f = j.fields.find(key); return f != j.fields.end() && f->second.type == type ? &f->second : nullptr; }
bool text_field(const Json& j, const char* key, std::string& out) { const auto v = field(j, key, Json::Type::text); if (!v) return false; out = v->text; return true; }
bool number_field(const Json& j, const char* key, std::uint64_t& out) { const auto v = field(j, key, Json::Type::number); if (!v) return false; out = v->number; return true; }
bool parse_context(const Json& j, exam::StateRequest& out) {
    std::string digest, client, session;
    return fields(j, "|packageId|packageDigestHex|clientIdHex|sessionIdHex|candidateId|", 5) && text_field(j, "packageId", out.package_id) &&
        text_field(j, "candidateId", out.candidate_id) && text_field(j, "packageDigestHex", digest) && unhex(digest, out.package_digest) &&
        text_field(j, "clientIdHex", client) && unhex(client, out.client_id) && text_field(j, "sessionIdHex", session) && unhex(session, out.session_id) &&
        !exam::encode_state_request(out).empty();
}
std::optional<StudentRoomPhase> phase(std::string_view value) {
    if (value == "draft") return StudentRoomPhase::draft;
    if (value == "joining") return StudentRoomPhase::joining;
    if (value == "prepared") return StudentRoomPhase::prepared;
    if (value == "running") return StudentRoomPhase::running;
    if (value == "closed") return StudentRoomPhase::closed;
    return {};
}
}

std::optional<StudentApproval> parse_student_approval(std::string_view json) {
    if (json.size() > 4096) return {};
    const auto j = JsonReader(json).parse(); StudentApproval result; std::string status;
    std::string publication;
    if (!j || !fields(*j, "|status|context|recoveryOnly|revision|serverTime|publicationPolicy|", 6) || !text_field(*j, "status", status) || status != "approved" ||
        !text_field(*j, "publicationPolicy", publication) || (publication != "live" && publication != "teacher_only") ||
        !number_field(*j, "revision", result.revision) || !result.revision || !number_field(*j, "serverTime", result.server_time) || !result.server_time) return {};
    const auto c = field(*j, "context", Json::Type::object); const auto recovery = field(*j, "recoveryOnly", Json::Type::boolean);
    if (!c || !recovery || !parse_context(*c, result.context)) return {};
    result.recovery_only = recovery->boolean; result.live_standings = publication == "live"; return result;
}
std::optional<StudentLifecycle> parse_student_lifecycle(std::string_view json) {
    if (json.size() > 256) return {};
    const auto j = JsonReader(json).parse(); StudentLifecycle result; std::string name;
    if (!j || !fields(*j, "|phase|revision|serverTime|serverStart|deadline|", 5) || !text_field(*j, "phase", name)) return {};
    const auto p = phase(name); if (!p) return {}; result.phase = *p;
    if (!number_field(*j, "revision", result.revision) || !result.revision || !number_field(*j, "serverTime", result.server_time) || !result.server_time ||
        !number_field(*j, "serverStart", result.server_start) || !number_field(*j, "deadline", result.deadline) ||
        ((result.server_start == 0) != (result.deadline == 0)) || (result.server_start &&
            (result.deadline <= result.server_start || result.deadline - result.server_start > 86400000 || result.server_time < result.server_start)) ||
        (result.phase == StudentRoomPhase::running && !result.server_start) ||
        (result.phase != StudentRoomPhase::running && result.phase != StudentRoomPhase::closed && result.server_start)) return {};
    return result;
}
bool student_lifecycle_extends(const StudentLifecycle& old, const StudentLifecycle& next) noexcept {
    if (!old.revision) return old.phase != StudentRoomPhase::closed || next.phase == StudentRoomPhase::closed;
    return next.revision >= old.revision && next.server_time >= old.server_time &&
        (next.revision != old.revision || (next.phase == old.phase && next.server_start == old.server_start && next.deadline == old.deadline)) &&
        static_cast<unsigned>(next.phase) >= static_cast<unsigned>(old.phase) &&
        ((!old.server_start && old.phase != StudentRoomPhase::closed) || (old.server_start == next.server_start && old.deadline == next.deadline));
}
std::optional<StudentStandings> parse_student_standings(std::string_view json, const contest_room::Realm& realm) {
    const auto j = JsonReader(json).parse(); StudentStandings result; std::string room, round;
    if (!j || !fields(*j, "|roomId|roundId|revision|generatedTime|freshness|rows|", 6) || !text_field(*j, "roomId", room) || room != realm.room_id ||
        !text_field(*j, "roundId", round) || round != realm.round_id || !number_field(*j, "revision", result.revision) || !result.revision ||
        !number_field(*j, "generatedTime", result.generated_time) || !result.generated_time || !text_field(*j, "freshness", result.freshness) ||
        (result.freshness != "current" && result.freshness != "pending" && result.freshness != "unavailable")) return {};
    const auto rows = field(*j, "rows", Json::Type::array); if (!rows) return {};
    for (const auto& r : rows->elements) {
        StudentStandingRow row; std::uint64_t rank = 0, solved = 0;
        if (!fields(r, "|rank|displayName|solved|penaltyMinutes|", 4) || !text_field(r, "displayName", row.display_name) || !valid_label(row.display_name) ||
            !number_field(r, "rank", rank) || rank == 0 || rank > 64 || !number_field(r, "solved", solved) || solved > 128 ||
            !number_field(r, "penaltyMinutes", row.penalty_minutes)) return {};
        row.rank = static_cast<std::uint32_t>(rank); row.solved = static_cast<std::uint32_t>(solved);
        if (!result.rows.empty()) { const auto& previous = result.rows.back();
            const bool tied = row.solved == previous.solved && row.penalty_minutes == previous.penalty_minutes;
            if (row.solved > previous.solved || (row.solved == previous.solved && row.penalty_minutes < previous.penalty_minutes) ||
                row.rank != (tied ? previous.rank : result.rows.size() + 1)) return {}; }
        else if (row.rank != 1) return {};
        result.rows.push_back(std::move(row));
    }
    return result;
}
bool student_capture_preserved(const ExamCloseCaptureReport& report, const exam::StateRequest& context,
    std::span<const Request> durable, const std::map<std::string, Request>& pending) {
    security::Sha256Digest nonce{};
    if (!report.complete || !report.local_saved || !report.reason.empty() || !same_context(report.context, context) || !report.generation || !unhex(report.nonce, nonce) || report.documents.size() > 128 || durable.size() > kDurableLimit || pending.size() > 128) return false;
    std::map<std::string, const Request*> by_id;
    for (const auto& saved : durable) if (!exam::programming::validate_request(saved) || !durable_operation(saved.operation) || !same_context(saved.context, context) || !by_id.emplace(saved.request_id, &saved).second) return false;
    const auto exact = [&](const Request& request) { const auto saved = by_id.find(request.request_id); return saved != by_id.end() && exam::programming::encode_request(*saved->second) == exam::programming::encode_request(request); };
    for (const auto& [id, request] : pending) if (id != request.request_id || !exam::programming::validate_request(request) || !same_context(request.context, context) || (durable_operation(request.operation) && !exact(request))) return false;
    std::set<std::string> documents, requests;
    for (const auto& doc : report.documents) {
        if ((doc.status != "forwarded" && doc.status != "received") || !documents.insert(doc.problem_id + '\0' + doc.language).second || !requests.insert(doc.request_id).second) return false;
        const auto found = by_id.find(doc.request_id); if (found == by_id.end()) return false; const auto& saved = *found->second;
        if ((saved.operation != Operation::draft && saved.operation != Operation::submit) || saved.problem_id != doc.problem_id || saved.problem_revision != doc.problem_revision ||
            saved.language != doc.language || saved.draft_revision != doc.draft_revision) return false;
        const auto digest = security::sha256(std::as_bytes(std::span(saved.source))); if (!digest || hex(*digest) != doc.source_digest) return false;
    }
    return true;
}

namespace {
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    Handle() = default; explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { if (value != INVALID_HANDLE_VALUE && value) CloseHandle(value); }
    Handle(Handle&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
    Handle& operator=(Handle&& other) noexcept { if (this != &other) { if (value != INVALID_HANDLE_VALUE && value) CloseHandle(value); value = std::exchange(other.value, INVALID_HANDLE_VALUE); } return *this; }
    Handle(const Handle&) = delete; Handle& operator=(const Handle&) = delete;
    [[nodiscard]] bool valid() const noexcept { return value && value != INVALID_HANDLE_VALUE; }
};
struct UserSecurity {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    SECURITY_ATTRIBUTES attributes{};
    UserSecurity() {
        HANDLE raw = nullptr; alignas(TOKEN_USER) std::array<std::byte, 1024> storage{}; DWORD n = 0;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return;
        Handle token(raw); LPWSTR sid = nullptr;
        if (!GetTokenInformation(raw, TokenUser, storage.data(), static_cast<DWORD>(storage.size()), &n) ||
            !ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid, &sid)) return;
        const std::wstring sddl = L"O:" + std::wstring(sid) + L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;OW)"; LocalFree(sid);
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) attributes = {sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    }
    ~UserSecurity() { if (descriptor) LocalFree(descriptor); }
    bool trusted(HANDLE h, bool leaf, bool protected_root = true, bool asset = false) const {
        PSID owner = nullptr, user = nullptr; PACL acl = nullptr; PSECURITY_DESCRIPTOR actual = nullptr; BOOL defaulted = FALSE;
        SECURITY_DESCRIPTOR_CONTROL control = 0; DWORD revision = 0;
        if (!descriptor || GetSecurityInfo(h, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr, &acl, nullptr, &actual) != ERROR_SUCCESS) return false;
        std::array<std::byte, SECURITY_MAX_SID_SIZE> system{}, admin{}, owner_rights{}, creator{};
        PSID trusted_installer = nullptr;
        const bool installer_sid = ConvertStringSidToSidW(L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464", &trusted_installer) != FALSE;
        DWORD system_size = static_cast<DWORD>(system.size()), admin_size = static_cast<DWORD>(admin.size()), rights_size = static_cast<DWORD>(owner_rights.size()), creator_size = static_cast<DWORD>(creator.size());
        bool ok = installer_sid && owner && acl && IsValidAcl(acl) && GetSecurityDescriptorOwner(descriptor, &user, &defaulted) &&
            CreateWellKnownSid(WinLocalSystemSid, nullptr, system.data(), &system_size) && CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admin.data(), &admin_size) &&
            CreateWellKnownSid(WinCreatorOwnerRightsSid, nullptr, owner_rights.data(), &rights_size) && CreateWellKnownSid(WinCreatorOwnerSid, nullptr, creator.data(), &creator_size) &&
            (EqualSid(owner, user) || (!leaf && (EqualSid(owner, system.data()) || EqualSid(owner, admin.data()) || EqualSid(owner, trusted_installer))));
        if (leaf && protected_root) ok = ok && GetSecurityDescriptorControl(actual, &control, &revision) && (control & SE_DACL_PROTECTED);
        for (DWORD i = 0; ok && i < acl->AceCount; ++i) {
            void* raw = nullptr; if (!GetAce(acl, i, &raw)) { ok = false; break; }
            const auto header = static_cast<ACE_HEADER*>(raw);
            if (header->AceFlags & INHERIT_ONLY_ACE) continue;
            if (header->AceType == ACCESS_DENIED_ACE_TYPE) continue;
            if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) { ok = false; break; }
            const auto ace = static_cast<ACCESS_ALLOWED_ACE*>(raw); const auto sid = const_cast<DWORD*>(&ace->SidStart);
            const bool allowed = EqualSid(sid, user) || EqualSid(sid, system.data()) || EqualSid(sid, owner_rights.data()) || EqualSid(sid, creator.data()) ||
                (!leaf && (EqualSid(sid, admin.data()) || EqualSid(sid, trusted_installer)));
            const DWORD replacement = FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_ALL | GENERIC_WRITE |
                (asset ? FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES : 0);
            if (!allowed && (leaf || (ace->Mask & replacement))) ok = false;
        }
        if (trusted_installer) LocalFree(trusted_installer);
        LocalFree(actual); return ok;
    }
};
bool safe_path(const std::filesystem::path& path) {
    const auto drive = path.root_name().native();
    if (!path.is_absolute() || drive.size() != 2 || drive[1] != L':' ||
        !((drive[0] >= L'A' && drive[0] <= L'Z') || (drive[0] >= L'a' && drive[0] <= L'z')) ||
        path.native().size() > 2048 || path.lexically_normal() != path || path.native().find(L'\0') != std::wstring::npos || path.native().find(L'"') != std::wstring::npos) return false;
    for (auto p = path; !p.empty(); p = p.parent_path()) { const auto a = GetFileAttributesW(p.c_str()); if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT)) return false; if (p == p.parent_path()) break; }
    return true;
}
class UserStorage {
public:
    bool open(const ContestStudentConfig& config) {
        fixture_ = config.memory_fixture; const auto realm = contest_room::encode_realm(config.realm); if (!realm) return false;
        entropy_ = *realm; const auto digest = security::sha256(*realm); if (!digest) return false;
        if (fixture_) return fixture_->load_profile && fixture_->save_profile && fixture_->load_requests && fixture_->save_request && fixture_->load_rooms && fixture_->save_rooms;
        if (!open_root(config.user_root) || !directory(root_ / L"sessions") || !directory(root_ / L"packages") || !directory(root_ / L"profiles") || !directory(root_ / L"temporary")) return false;
        session_ = root_ / L"sessions" / hex(*digest); if (!directory(session_)) return false;
        return lock_namespace(L"identity.dpapi");
    }
    bool open_registry(const std::filesystem::path& root) {
        if (!open_root(root)) return false;
        session_ = root_; entropy_ = bytes("NSTU-STUDENT-REMEMBERED-ROOMS-V1");
        return lock_namespace(L"remembered-rooms.dpapi");
    }
    bool open_root(const std::filesystem::path& root) {
        if (!safe_path(root) || root == root.root_path()) return false;
        UserSecurity security; if (!security.descriptor) return false;
        std::vector<std::filesystem::path> missing;
        auto ancestor = root;
        while (GetFileAttributesW(ancestor.c_str()) == INVALID_FILE_ATTRIBUTES) {
            const auto error = GetLastError();
            if ((error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) || ancestor == ancestor.parent_path() || missing.size() >= 64) return false;
            missing.push_back(ancestor); ancestor = ancestor.parent_path();
        }
        for (auto p = ancestor; !p.empty(); p = p.parent_path()) {
            Handle directory(CreateFileW(p.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            BY_HANDLE_FILE_INFORMATION info{};
            if (!directory.valid() || !GetFileInformationByHandle(directory.value, &info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || !security.trusted(directory.value, p == root)) return false;
            pins_.push_back(std::move(directory)); if (p == p.parent_path()) break;
        }
        root_ = root;
        // Existing ancestry is pinned before creation. Never rewrite an
        // existing owner's ACL to turn unsafe storage into an accepted root.
        for (auto p = missing.rbegin(); p != missing.rend(); ++p) if (!directory(*p)) return false;
        return true;
    }
    bool lock_namespace(std::wstring_view filename) {
        UserSecurity security; filename_ = filename;
        const auto canonical = std::filesystem::canonical(root_).native(); const auto root_bytes = std::as_bytes(std::span(canonical)); entropy_.insert(entropy_.end(), root_bytes.begin(), root_bytes.end());
        lock_ = Handle(CreateFileW((session_ / (filename_ + L".lock")).c_str(), GENERIC_READ | GENERIC_WRITE | READ_CONTROL, 0, &security.attributes, OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        BY_HANDLE_FILE_INFORMATION lock_info{};
        return lock_.valid() && GetFileInformationByHandle(lock_.value, &lock_info) && !(lock_info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) && security.trusted(lock_.value, true);
    }
    bool directory(const std::filesystem::path& path) {
        UserSecurity security;
        if (!safe_path(path) || !security.descriptor || (!CreateDirectoryW(path.c_str(), &security.attributes) && GetLastError() != ERROR_ALREADY_EXISTS)) return false;
        Handle h(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        BY_HANDLE_FILE_INFORMATION info{};
        if (!h.valid() || !GetFileInformationByHandle(h.value, &info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || !security.trusted(h.value, true)) return false;
        pins_.push_back(std::move(h)); return true;
    }
    std::optional<std::vector<std::byte>> load() {
        if (fixture_) return fixture_->load_profile();
        const auto path = session_ / filename_;
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return GetLastError() == ERROR_FILE_NOT_FOUND ? std::optional{std::vector<std::byte>{}} : std::nullopt;
        UserSecurity security; Handle file(CreateFileW(path.c_str(), GENERIC_READ | READ_CONTROL, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        LARGE_INTEGER size{}; BY_HANDLE_FILE_INFORMATION info{};
        if (!file.valid() || !GetFileInformationByHandle(file.value, &info) || (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
            !security.trusted(file.value, true) || !GetFileSizeEx(file.value, &size) || size.QuadPart <= 0 || size.QuadPart > 65536) return {};
        std::vector<std::byte> data(static_cast<std::size_t>(size.QuadPart)); DWORD read = 0;
        if (!ReadFile(file.value, data.data(), static_cast<DWORD>(data.size()), &read, nullptr) || read != data.size()) return {};
        auto plain = security::unprotect_user_secret(data, entropy_); return plain.empty() ? std::nullopt : std::optional{plain};
    }
    bool save(std::span<const std::byte> wire) {
        if (fixture_) return fixture_->save_profile(wire);
        const auto blob = security::protect_user_secret(wire, entropy_); security::Nonce nonce{};
        if (blob.empty() || blob.size() > 65536 || !security::generate_random(nonce)) return false;
        UserSecurity security; const auto temporary = session_ / (hex(nonce) + ".tmp");
        Handle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, &security.attributes, CREATE_NEW, FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        DWORD written = 0; if (!file.valid()) return false;
        bool ok = WriteFile(file.value, blob.data(), static_cast<DWORD>(blob.size()), &written, nullptr) && written == blob.size() && FlushFileBuffers(file.value);
        file = Handle{};
        if (ok) ok = MoveFileExW(temporary.c_str(), (session_ / filename_).c_str(), MOVEFILE_WRITE_THROUGH | MOVEFILE_REPLACE_EXISTING) != FALSE;
        if (!ok) DeleteFileW(temporary.c_str());
        return ok;
    }
    const std::filesystem::path& root() const noexcept { return root_; }
    const std::filesystem::path& session() const noexcept { return session_; }
private:
    std::filesystem::path root_, session_; std::vector<std::byte> entropy_; std::vector<Handle> pins_; Handle lock_;
    std::optional<StudentMemoryFixture> fixture_;
    std::wstring filename_;
};
struct Identity {
    std::string uuid;
    std::uint32_t key_id = 0;
    security::Sha256Digest key{};
    std::optional<exam::StateRequest> context;
    StudentLifecycle lifecycle;
    bool live_standings = false;
    std::uint64_t approval_revision = 0, approval_time = 0;
    ~Identity() { security::secure_zero(key); }
};
struct Writer {
    std::vector<std::byte> wire;
    void number(std::uint64_t n) { for (unsigned i = 0; i < 8; ++i) wire.push_back(static_cast<std::byte>((n >> (i * 8u)) & 255u)); }
    void blob(std::span<const std::byte> data) { number(data.size()); wire.insert(wire.end(), data.begin(), data.end()); }
};
std::vector<std::byte> identity_wire(const Identity& id) {
    Writer w; w.number(0x3144534e); w.blob(bytes(id.uuid)); w.number(id.key_id); w.blob(id.key);
    w.blob(id.context ? exam::encode_state_request(*id.context) : std::vector<std::byte>{});
    w.number(static_cast<std::uint64_t>(id.lifecycle.phase)); w.number(id.lifecycle.revision); w.number(id.lifecycle.server_time); w.number(id.lifecycle.server_start); w.number(id.lifecycle.deadline); w.number(id.live_standings ? 1 : 0); w.number(id.approval_revision); w.number(id.approval_time); return w.wire;
}
bool identity_read(std::span<const std::byte> wire, Identity& id) {
    std::size_t pos = 0; bool valid = true;
    const auto number = [&]() { std::uint64_t n = 0; if (wire.size() - pos < 8) { valid = false; return n; }
        for (unsigned i = 0; i < 8; ++i) n |= std::to_integer<std::uint64_t>(wire[pos++]) << (i * 8u);
        return n; };
    const auto blob = [&](std::size_t maximum) { const auto n = number(); if (!valid || n > maximum || n > wire.size() - pos) { valid = false; return std::span<const std::byte>{}; }
        const auto data = wire.subspan(pos, static_cast<std::size_t>(n)); pos += static_cast<std::size_t>(n); return data; };
    if (number() != 0x3144534e) return false;
    const auto uuid = blob(128); id.uuid.assign(reinterpret_cast<const char*>(uuid.data()), uuid.size()); const auto key_id = number(); const auto key = blob(32); const auto context = blob(1024);
    const auto room_phase = number(); id.lifecycle.revision = number(); id.lifecycle.server_time = number(); id.lifecycle.server_start = number(); id.lifecycle.deadline = number(); const auto live = number();
    id.approval_revision = number(); id.approval_time = number();
    const auto client = pairing::client_id_from_uuid(id.uuid);
    if (!valid || pos != wire.size() || !client || key_id > UINT32_MAX || key.size() != 32 || room_phase > 4 || live > 1 || id.approval_revision > kSafeInteger || id.approval_time > kSafeInteger || id.lifecycle.revision > kSafeInteger || id.lifecycle.server_time > kSafeInteger || id.lifecycle.server_start > kSafeInteger || id.lifecycle.deadline > kSafeInteger) return false;
    id.key_id = static_cast<std::uint32_t>(key_id); id.live_standings = live == 1; std::copy(key.begin(), key.end(), id.key.begin()); id.lifecycle.phase = static_cast<StudentRoomPhase>(room_phase);
    if ((id.key_id == 0) != std::all_of(id.key.begin(), id.key.end(), [](std::byte b) { return b == std::byte{0}; })) return false;
    if (!context.empty()) { id.context = exam::decode_state_request(context); if (!id.context || id.context->client_id != *client || !id.key_id) return false; }
    if (id.lifecycle.revision && (!id.context || !id.lifecycle.server_time || ((id.lifecycle.server_start == 0) != (id.lifecycle.deadline == 0)) ||
        (id.lifecycle.server_start && (id.lifecycle.deadline <= id.lifecycle.server_start || id.lifecycle.deadline - id.lifecycle.server_start > 86400000 || id.lifecycle.server_time < id.lifecycle.server_start)) ||
        (id.lifecycle.phase == StudentRoomPhase::running && !id.lifecycle.server_start) ||
        (id.lifecycle.phase != StudentRoomPhase::running && id.lifecycle.phase != StudentRoomPhase::closed && id.lifecycle.server_start))) return false;
    if ((id.approval_revision == 0) != (id.approval_time == 0) || (id.approval_revision && !id.context)) return false;
    return true;
}
bool read_exact(net::TcpSocket& socket, std::span<std::byte> output, Clock::time_point deadline, const std::atomic_bool& running) {
    std::size_t received = 0;
    while (running && Clock::now() < deadline && received < output.size()) { if (!socket.wait_readable(20)) continue;
        const auto n = socket.receive(output.subspan(received)); if (n <= 0) return false; received += static_cast<std::size_t>(n); }
    return received == output.size();
}
std::optional<protocol::TcpFrame> pairing_frame(net::TcpSocket& socket, Clock::time_point deadline, const std::atomic_bool& running) {
    std::array<std::byte, 4> prefix{}; if (!read_exact(socket, prefix, deadline, running)) return {};
    std::uint32_t size = 0; for (unsigned i = 0; i < 4; ++i) size |= std::to_integer<std::uint32_t>(prefix[i]) << (i * 8u);
    if (size < protocol::kCommandHeaderBytes || size > pairing::kMaximumPairingMessageBytes + protocol::kCommandHeaderBytes) return {};
    std::vector<std::byte> data(size); if (!read_exact(socket, data, deadline, running)) return {};
    const auto envelope = protocol::decode_command_header(data); if (!envelope || envelope->payload_bytes != size - protocol::kCommandHeaderBytes) return {};
    return protocol::TcpFrame{*envelope, {data.begin() + protocol::kCommandHeaderBytes, data.end()}};
}
bool send_pairing(net::TcpSocket& socket, protocol::CommandType kind, std::uint64_t id, std::span<const std::byte> payload, Clock::time_point deadline) {
    const auto wire = protocol::encode_tcp_frame({1, kind, static_cast<std::uint32_t>(payload.size()), id}, payload);
    return !wire.empty() && socket.send_all(wire, deadline) == static_cast<int>(wire.size());
}
std::string name_envelope(std::string_view name) {
    DWORD n = 0; if (!CryptBinaryToStringA(reinterpret_cast<const BYTE*>(name.data()), static_cast<DWORD>(name.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &n)) return {};
    std::string encoded(n, '\0'); if (!CryptBinaryToStringA(reinterpret_cast<const BYTE*>(name.data()), static_cast<DWORD>(name.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, encoded.data(), &n)) return {};
    encoded.resize(n); return "nstu-name-v1:" + encoded;
}
}

std::vector<std::byte> encode_student_remembered_rooms(std::span<const StudentRememberedRoom> rooms) {
    if (rooms.size() > 32) return {};
    Writer writer; writer.number(0x31524853); writer.number(rooms.size()); std::set<std::string> realms;
    for (const auto& room : rooms) {
        IN_ADDR address{}; const auto realm = contest_room::encode_realm(room.realm);
        if (!realm || !realms.insert(hex(*realm)).second || !room.endpoint.port || room.endpoint.address.size() > 15 || room.endpoint.address.find('\0') != std::string::npos ||
            InetPtonA(AF_INET, room.endpoint.address.c_str(), &address) != 1 || !valid_label(room.display_label)) return {};
        writer.number(room.realm.version); writer.blob(bytes(room.realm.teacher_id)); writer.blob(bytes(room.realm.room_id)); writer.blob(bytes(room.realm.round_id));
        writer.blob(bytes(room.endpoint.address)); writer.number(room.endpoint.port); writer.blob(bytes(room.display_label));
    }
    return writer.wire;
}
std::optional<std::vector<StudentRememberedRoom>> decode_student_remembered_rooms(std::span<const std::byte> wire) {
    if (wire.empty()) return std::vector<StudentRememberedRoom>{};
    if (wire.size() > 16384) return {};
    std::size_t pos = 0; bool valid = true;
    const auto number = [&]() { std::uint64_t n = 0; if (wire.size() - pos < 8) { valid = false; return n; }
        for (unsigned i = 0; i < 8; ++i) n |= std::to_integer<std::uint64_t>(wire[pos++]) << (i * 8u);
        return n; };
    const auto text = [&](std::size_t maximum) { const auto n = number(); if (!valid || n > maximum || n > wire.size() - pos) { valid = false; return std::string{}; }
        if (!n) return std::string{};
        std::string result(reinterpret_cast<const char*>(wire.data() + pos), static_cast<std::size_t>(n)); pos += static_cast<std::size_t>(n); return result; };
    if (number() != 0x31524853) return {};
    const auto count = number(); if (!valid || count > 32) return {};
    std::vector<StudentRememberedRoom> result;
    for (std::uint64_t i = 0; i < count; ++i) {
        StudentRememberedRoom room; const auto version = number(); room.realm.teacher_id = text(96); room.realm.room_id = text(32); room.realm.round_id = text(32);
        room.endpoint.address = text(15); const auto port = number(); room.display_label = text(64);
        if (!valid || version != contest_room::kVersion || !port || port > UINT16_MAX) return {};
        room.realm.version = static_cast<std::uint16_t>(version); room.endpoint.port = static_cast<std::uint16_t>(port); result.push_back(std::move(room));
    }
    if (!valid || pos != wire.size() || encode_student_remembered_rooms(result).empty()) return {};
    return result;
}
std::optional<std::vector<StudentRememberedRoom>> load_student_remembered_rooms(const std::filesystem::path& user_root, std::string* error) {
    try {
        if (!safe_path(user_root)) { fail(error, "remembered room root is invalid"); return {}; }
        if (GetFileAttributesW(user_root.c_str()) == INVALID_FILE_ATTRIBUTES) {
            const auto code = GetLastError(); if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return std::vector<StudentRememberedRoom>{};
            fail(error, "remembered room storage is unavailable"); return {};
        }
        UserStorage storage;
        if (!storage.open_registry(user_root)) { fail(error, "remembered room ownership is unavailable"); return {}; }
        const auto wire = storage.load(); const auto rooms = wire ? decode_student_remembered_rooms(*wire) : std::nullopt;
        if (!rooms) fail(error, "remembered rooms could not be recovered; original data retained");
        return rooms;
    } catch (...) { fail(error, "remembered room storage is unavailable; original data retained"); return {}; }
}

namespace {
std::optional<security::Sha256Digest> hash_file(HANDLE h, std::uint64_t maximum) {
    LARGE_INTEGER size{}; BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(h, &info) || (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
        GetFileType(h) != FILE_TYPE_DISK || !GetFileSizeEx(h, &size) || size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) > maximum) return {};
    // Public archives are bounded at 256MiB; hash in chunks with CNG.
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr; DWORD object_bytes = 0, read = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    struct Cleanup { BCRYPT_ALG_HANDLE& algorithm; BCRYPT_HASH_HANDLE& hash; ~Cleanup() { if (hash) BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0); } } cleanup{algorithm, hash};
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_bytes), sizeof(object_bytes), &read, 0) < 0) return {};
    std::vector<std::byte> object(object_bytes); security::Sha256Digest digest{};
    if (BCryptCreateHash(algorithm, &hash, reinterpret_cast<PUCHAR>(object.data()), object_bytes, nullptr, 0, 0) < 0) return {};
    std::array<std::byte, 65536> buffer{}; std::uint64_t count = 0;
    LARGE_INTEGER start{}; if (!SetFilePointerEx(h, start, nullptr, FILE_BEGIN)) return {};
    while (count < static_cast<std::uint64_t>(size.QuadPart)) {
        const auto next = static_cast<DWORD>(std::min<std::uint64_t>(buffer.size(), static_cast<std::uint64_t>(size.QuadPart) - count));
        if (!ReadFile(h, buffer.data(), next, &read, nullptr) || read != next || BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), read, 0) < 0) return {};
        count += read;
    }
    return BCryptFinishHash(hash, reinterpret_cast<PUCHAR>(digest.data()), static_cast<ULONG>(digest.size()), 0) < 0 ? std::nullopt : std::optional{digest};
}
bool private_tree(const std::filesystem::path& root) {
    UserSecurity security;
    const auto check = [&](const std::filesystem::path& path, bool root_path) {
        if (!safe_path(path)) return false;
        Handle h(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        BY_HANDLE_FILE_INFORMATION info{};
        return h.valid() && GetFileInformationByHandle(h.value, &info) && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && security.trusted(h.value, true, root_path);
    };
    if (!check(root, true)) return false;
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) if (++count > 4096 || !check(entry.path(), false)) return false;
    return true;
}
bool safe_runtime_tree(const std::filesystem::path& root) {
    UserSecurity security;
    const auto check = [&](const std::filesystem::path& path) {
        Handle h(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        BY_HANDLE_FILE_INFORMATION info{};
        return safe_path(path) && h.valid() && GetFileInformationByHandle(h.value, &info) && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && security.trusted(h.value, false, false, true);
    };
    if (!check(root)) return false;
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) if (++count > 4096 || !check(entry.path())) return false;
    return true;
}
bool extract_public(const ContestStudentConfig& config, const std::filesystem::path& archive,
    const contest_package::PackageOffer& offer, const std::atomic_bool& running) {
    if (!safe_path(config.extractor_script) || !safe_path(config.powershell_executable) || !safe_path(archive)) return false;
    UserSecurity security; std::vector<Handle> asset_directories;
    for (const auto& asset : {config.extractor_script, config.powershell_executable}) {
        for (auto directory = asset.parent_path(); !directory.empty(); directory = directory.parent_path()) {
            Handle handle(CreateFileW(directory.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (!handle.valid() || !security.trusted(handle.value, false)) return false;
            asset_directories.push_back(std::move(handle)); if (directory == directory.parent_path()) break;
        }
    }
    Handle script(CreateFileW(config.extractor_script.c_str(), GENERIC_READ | READ_CONTROL, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    Handle executable(CreateFileW(config.powershell_executable.c_str(), GENERIC_READ | READ_CONTROL, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    const auto digest = script.valid() ? hash_file(script.value, 65536) : std::nullopt;
    const auto runtime_digest = executable.valid() ? hash_file(executable.value, 128u * 1024u * 1024u) : std::nullopt;
    if (!digest || *digest != config.extractor_digest || hex(*digest) != "23dfce986a4d0f212def8d6c630b8488515e4a3747b9a664b921762a805403a6" ||
        !runtime_digest || *runtime_digest != config.powershell_digest || !security.trusted(script.value, false, false, true) ||
        !security.trusted(executable.value, false, false, true) || !safe_runtime_tree(config.powershell_executable.parent_path())) return false;
    Handle job(CreateJobObjectW(nullptr, nullptr)); JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    if (!job.valid() || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) return false;
    const auto quoted = [](const std::filesystem::path& p) { return L"\"" + p.wstring() + L"\""; };
    const auto wide = [](std::string_view value) { return std::wstring(value.begin(), value.end()); };
    std::wstring command = quoted(config.powershell_executable) + L" -NoLogo -NoProfile -NonInteractive -File " + quoted(config.extractor_script) +
        L" -ArchivePath " + quoted(archive) + L" -ExpectedArchiveSha256 " + wide(offer.archive_digest) + L" -ExpectedContentSha256 " + wide(offer.content_digest) +
        L" -Mode Publish -PackagesRoot " + quoted(config.user_root / L"packages");
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
    std::array<wchar_t, MAX_PATH> windows{}; const auto windows_length = GetWindowsDirectoryW(windows.data(), static_cast<UINT>(windows.size()));
    if (!windows_length || windows_length >= windows.size()) return false;
    std::wstring environment;
    const auto env = [&](std::wstring value) { environment += value; environment.push_back(L'\0'); };
    env(L"SystemRoot=" + std::wstring(windows.data(), windows_length));
    env(L"PSModulePath=" + (config.powershell_executable.parent_path() / L"Modules").wstring());
    env(L"TEMP=" + (config.user_root / L"temporary").wstring());
    env(L"TMP=" + (config.user_root / L"temporary").wstring()); environment.push_back(L'\0');
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(config.powershell_executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
        environment.data(), config.powershell_executable.parent_path().c_str(), &startup, &process)) return false;
    Handle child(process.hProcess), thread(process.hThread);
    bool ok = AssignProcessToJobObject(job.value, child.value) && ResumeThread(thread.value) != static_cast<DWORD>(-1);
    const auto deadline = Clock::now() + contest_package::kTransferBudget; DWORD wait = WAIT_TIMEOUT;
    while (ok && running && Clock::now() < deadline && (wait = WaitForSingleObject(child.value, 20)) == WAIT_TIMEOUT) {}
    DWORD exit_code = 1;
    if (!ok || !running || wait != WAIT_OBJECT_0) { (void)TerminateJobObject(job.value, 1); (void)TerminateProcess(child.value, 1); return false; }
    return GetExitCodeProcess(child.value, &exit_code) && exit_code == 0;
}
}

struct ContestStudentSession::Impl {
    mutable std::mutex mutex;
    std::atomic_bool running{false};
    std::atomic_bool io_enabled{false};
    std::thread worker;
    ContestStudentConfig config;
    ContestStudentSnapshot state;
    std::deque<Request> queue;
    std::deque<exam::programming::Response> replies;
    std::vector<Request> durable;
    std::optional<ExamCloseCaptureReport> capture;
    bool connect_requested = true, disconnect_requested = false, standings_requested = false;
    int sas_decision = 0;
    Clock::time_point last_standings{};
    Clock::time_point clock_observed{};
    std::uint64_t clock_server = 0;
    UserStorage storage;
    ProgrammingOutbox outbox;
    Identity identity;
    std::unique_ptr<contest_package::ReceiverTracker> receiver;
    std::uint64_t transfer_id = 0;
    Handle archive;
    std::filesystem::path archive_path;
    std::vector<std::byte> memory_archive;
    std::map<std::uint64_t, Request> inflight;
    std::map<std::string, std::vector<std::byte>> sent;
    std::uint64_t next_request = 1;
    bool remembered = false;

    std::uint64_t estimated_time() const {
        if (!clock_server) return 0;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - clock_observed).count();
        return std::min(kSafeInteger, clock_server + static_cast<std::uint64_t>(std::max<std::int64_t>(0, elapsed)));
    }
    void anchor_time(std::uint64_t server_time) {
        clock_server = std::max(estimated_time(), server_time); clock_observed = Clock::now();
    }

    void error(const char* message, StudentPhase phase = StudentPhase::failed) {
        std::scoped_lock lock(mutex); state.error = message; state.phase = phase; state.connected = false; state.sas.clear();
    }
    void publish_identity() {
        std::scoped_lock lock(mutex); state.context = identity.context; state.lifecycle = identity.lifecycle;
        state.recovery_only = identity.lifecycle.phase == StudentRoomPhase::closed; state.live_standings = identity.live_standings;
    }
    bool request_allowed(const Request& request) const {
        if (!running || !state.context || !same_context(request.context, *state.context) || request.workstation_policy_ready || !exam::programming::validate_request(request)) return false;
        if (state.recovery_only) return recovery_operation(request.operation);
        if (request.operation == Operation::run) return state.lifecycle.phase == StudentRoomPhase::running && state.package_verified && estimated_time() < state.lifecycle.deadline;
        if (request.operation == Operation::submit) return state.lifecycle.phase == StudentRoomPhase::running && state.package_verified;
        if (request.operation == Operation::ready && state.lifecycle.deadline && estimated_time() >= state.lifecycle.deadline) return false;
        return state.package_verified;
    }
    bool enqueue(const Request& request) {
        std::scoped_lock lock(mutex); if (!request_allowed(request) || queue.size() >= kQueueLimit) return false;
        for (const auto& saved : durable) if (saved.request_id == request.request_id && exam::programming::encode_request(saved) != exam::programming::encode_request(request)) return false;
        for (const auto& queued : queue) if (queued.request_id == request.request_id) return exam::programming::encode_request(queued) == exam::programming::encode_request(request);
        queue.push_back(request); state.queued_requests = queue.size(); return true;
    }
    bool preserve(const ExamCloseCaptureReport& report, const std::map<std::string, Request>& pending) const {
        std::scoped_lock lock(mutex);
        return running && state.context && student_capture_preserved(report, *state.context, durable, pending);
    }
    bool initialize() {
        if (!storage.open(config)) { error("student owner storage is unavailable; existing data retained"); return false; }
        const auto wire = storage.load(); if (!wire) { error("student identity cannot be recovered; existing data retained"); return false; }
        if (wire->empty()) { security::Nonce nonce{}; if (!security::generate_random(nonce)) return false; identity.uuid = hex(nonce);
            if (!storage.save(identity_wire(identity))) { error("student identity could not be preserved"); return false; } }
        else if (!identity_read(*wire, identity)) { error("student identity binding is invalid; existing data retained"); return false; }
        if (config.memory_fixture) durable = config.memory_fixture->load_requests();
        else { if (!outbox.open_user(storage.session() / L"programming-outbox", storage.root())) { error("student outbox recovery failed; original source retained"); return false; }
            durable = outbox.pending_for_client(*pairing::client_id_from_uuid(identity.uuid)); }
        std::size_t size = 0;
        for (const auto& request : durable) { size += exam::programming::encode_request(request).size();
            if (!identity.context || !same_context(*identity.context, request.context) || !durable_operation(request.operation) || request.workstation_policy_ready || !exam::programming::validate_request(request)) return false; }
        if (durable.size() > kDurableLimit || size > 8u * 1024u * 1024u) return false;
        publish_identity(); { std::scoped_lock lock(mutex); state.durable_requests = durable.size(); } return true;
    }
    bool pair(net::TcpSocket& socket, Clock::time_point deadline) {
        auto ephemeral = pairing::EphemeralKeyPair::generate(); if (!ephemeral) return false;
        pairing::PairingHello hello; hello.agreement = ephemeral->agreement(); hello.client_uuid = identity.uuid; hello.client_hostname = name_envelope(config.display_name);
        hello.client_public_key.assign(ephemeral->public_key().begin(), ephemeral->public_key().end()); if (!security::generate_random(hello.client_nonce)) return false;
        constexpr std::uint64_t pairing_id = 1;
        if (!send_pairing(socket, protocol::CommandType::pairing_hello, pairing_id, pairing::encode_pairing_hello(hello), deadline)) return false;
        const auto frame = pairing_frame(socket, deadline, io_enabled);
        const auto offer = frame && frame->envelope.type == protocol::CommandType::pairing_offer && frame->envelope.request_id == pairing_id ? pairing::decode_pairing_offer(frame->payload) : std::nullopt;
        const auto transcript = offer ? pairing::make_transcript(hello, *offer) : std::nullopt;
        auto shared = offer ? ephemeral->agree(offer->server_public_key) : std::nullopt;
        if (!transcript || !shared) return false;
        auto secrets = contest_room::derive_room_secrets(*shared, config.realm, *transcript); security::secure_zero(*shared); ephemeral.reset(); if (!secrets) return false;
        struct Wipe { pairing::PairingSecrets& secrets; ~Wipe() { pairing::secure_zero(secrets); } } wipe{*secrets};
        { std::scoped_lock lock(mutex); state.phase = StudentPhase::confirm_teacher; state.sas = secrets->short_authentication_string; sas_decision = 0; }
        int decision = 0;
        while (running && io_enabled && Clock::now() < deadline && !decision) { { std::scoped_lock lock(mutex); decision = sas_decision; if (disconnect_requested) decision = -1; } if (!decision) std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        if (decision != 1) return false;
        const auto tag = pairing::confirmation_tag(*secrets, *transcript, pairing::ConfirmationRole::client);
        if (!tag || !send_pairing(socket, protocol::CommandType::pairing_confirm, pairing_id, pairing::encode_pairing_confirm({*tag}), deadline)) return false;
        { std::scoped_lock lock(mutex); state.phase = StudentPhase::awaiting_admission; state.sas.clear(); }
        const auto acceptance = pairing_frame(socket, deadline, io_enabled);
        const auto accepted = acceptance && acceptance->envelope.type == protocol::CommandType::pairing_accept && acceptance->envelope.request_id == pairing_id ? pairing::decode_pairing_accept(acceptance->payload) : std::nullopt;
        if (!accepted || !accepted->key_id || !pairing::verify_confirmation_tag(*secrets, *transcript, pairing::ConfirmationRole::server, accepted->server_tag)) return false;
        identity.key_id = accepted->key_id; identity.key = secrets->enrolled_key;
        return storage.save(identity_wire(identity));
    }
    ExamHostOptions options(const ExamPackageReport& package) const {
        ExamHostOptions result; result.policy = ExamHostPolicy::ordinary_user; result.package_root = package.package_root; result.web_root = package.web_root;
        result.allowed_package_root = config.user_root / L"packages"; result.allowed_user_data_root = config.user_root / L"profiles";
        auto context_wire = *contest_room::encode_realm(config.realm); const auto tuple = exam::encode_state_request(*identity.context); context_wire.insert(context_wire.end(), tuple.begin(), tuple.end()); const auto digest = security::sha256(context_wire);
        result.user_data_root = result.allowed_user_data_root / (digest ? hex(*digest) : "invalid"); result.expected_digest_hex = hex(identity.context->package_digest);
        result.context_json = exam::programming::context_json(*identity.context); result.context_json.pop_back(); result.context_json += ",\"synchronizedStart\":true}"; return result;
    }
    bool restore_package() {
        if (!identity.context || config.memory_fixture) return true;
        const auto root = config.user_root / L"packages" / hex(identity.context->package_digest);
        if (!std::filesystem::exists(root)) return true;
        ExamPackageReport report;
        if (!private_tree(root) || !inspect_exam_package(root, root / L"exam" / L"web", hex(identity.context->package_digest), true, report) ||
            !validate_programming_manifest(report.manifest_json) || !validate_exam_host_options(options(report), report)) {
            std::scoped_lock lock(mutex); state.package.reset(); state.package_verified = false;
            state.error = "public package is unavailable; original source retained";
            return state.recovery_only;
        }
        std::scoped_lock lock(mutex); state.package = report; state.package_verified = true; return true;
    }
    bool remember_room() {
        if (remembered) return true;
        UserStorage registry;
        if (!config.memory_fixture && !registry.open_registry(config.user_root)) return false;
        const auto wire = config.memory_fixture ? config.memory_fixture->load_rooms() : registry.load();
        auto rooms = wire ? decode_student_remembered_rooms(*wire) : std::nullopt; if (!rooms) return false;
        std::erase_if(*rooms, [&](const StudentRememberedRoom& room) { return room.realm == config.realm; });
        // The bounded recent-hint list never erases identity, source or receipts.
        if (rooms->size() == 32) rooms->erase(rooms->begin());
        const auto label = config.room_label.empty() ? config.realm.teacher_id.substr(0, 64) : config.room_label;
        rooms->push_back({config.realm, config.endpoint, label}); const auto encoded = encode_student_remembered_rooms(*rooms); if (encoded.empty()) return false;
        remembered = config.memory_fixture ? config.memory_fixture->save_rooms(encoded) : registry.save(encoded); return remembered;
    }
    bool approval(const contest_room::Message& message) {
        const auto next = parse_student_approval(std::string_view(reinterpret_cast<const char*>(message.payload.data()), message.payload.size()));
        const auto id = pairing::client_id_from_uuid(identity.uuid);
        if (!next || !id || next->context.client_id != *id || (identity.context && (!same_context(*identity.context, next->context) || identity.live_standings != next->live_standings)) ||
            next->revision < std::max(identity.lifecycle.revision, identity.approval_revision) || next->server_time < std::max(identity.lifecycle.server_time, identity.approval_time) ||
            (identity.lifecycle.phase == StudentRoomPhase::closed && !next->recovery_only)) return false;
        const auto previous = identity.context; const auto previous_lifecycle = identity.lifecycle;
        identity.context = next->context; identity.live_standings = next->live_standings;
        if (next->recovery_only) identity.lifecycle.phase = StudentRoomPhase::closed;
        identity.approval_revision = next->revision; identity.approval_time = next->server_time;
        if (!storage.save(identity_wire(identity))) { identity.context = previous; identity.lifecycle = previous_lifecycle; return false; }
        if (!remember_room()) return false;
        publish_identity();
        { std::scoped_lock lock(mutex); state.recovery_only = state.recovery_only || next->recovery_only; anchor_time(next->server_time); }
        return restore_package();
    }
    bool lifecycle(const contest_room::Message& message) {
        const auto next = parse_student_lifecycle(std::string_view(reinterpret_cast<const char*>(message.payload.data()), message.payload.size()));
        if (!identity.context || !next || next->revision < identity.approval_revision || next->server_time < identity.approval_time || !student_lifecycle_extends(identity.lifecycle, *next)) return false;
        const auto old = identity.lifecycle; identity.lifecycle = *next;
        if (!storage.save(identity_wire(identity))) { identity.lifecycle = old; return false; }
        publish_identity(); std::scoped_lock lock(mutex); anchor_time(next->server_time);
        state.phase = next->phase == StudentRoomPhase::closed ? StudentPhase::closed : next->phase == StudentRoomPhase::running ? StudentPhase::running : state.package_verified ? StudentPhase::prepared : StudentPhase::approved;
        return true;
    }
    bool offer(const contest_room::Message& message, contest_room::RoomChannel& channel) {
        if (!identity.context || !message.request_id || identity.lifecycle.phase == StudentRoomPhase::closed) return false;
        const auto offered = contest_package::decode_offer(message.payload, identity.context->package_id, hex(identity.context->package_digest)); if (!offered) return false;
        if (receiver) {
            if (transfer_id != message.request_id || receiver->offer() != *offered) return false;
            if (receiver->status(Clock::now()) == contest_package::TransferStatus::prepared) {
                const auto ack = receiver->acknowledgment(Clock::now()); const auto prepared = receiver->prepared(offered->content_digest, offered->archive_digest, Clock::now());
                return ack && prepared && channel.send({Kind::package_ack, transfer_id, contest_package::encode_ack(*ack)}, Clock::now() + std::chrono::seconds(1)) &&
                    channel.send({Kind::prepared, transfer_id, contest_package::encode_prepared(*prepared)}, Clock::now() + std::chrono::seconds(1));
            }
            if (!receiver->resume(*offered, Clock::now())) return false;
        }
        else {
            receiver = std::make_unique<contest_package::ReceiverTracker>(*offered, Clock::now()); transfer_id = message.request_id;
            if (!config.memory_fixture) { UserSecurity security; security::Nonce nonce{}; if (!security::generate_random(nonce)) return false;
                archive_path = config.user_root / L"packages" / (".receive-" + hex(nonce) + ".nstuexam");
                archive = Handle(CreateFileW(archive_path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, &security.attributes, CREATE_NEW, FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)); if (!archive.valid()) return false; }
        }
        const auto ack = receiver->acknowledgment(Clock::now()); if (!ack) return false;
        { std::scoped_lock lock(mutex); state.phase = StudentPhase::receiving; state.archive_bytes = offered->archive_byte_count; }
        return channel.send({Kind::package_ack, transfer_id, contest_package::encode_ack(*ack)}, Clock::now() + std::chrono::seconds(1));
    }
    bool chunk(const contest_room::Message& message, contest_room::RoomChannel& channel) {
        if (!receiver || transfer_id != message.request_id || !identity.context || identity.lifecycle.phase == StudentRoomPhase::closed) return false;
        const auto data = contest_package::decode_chunk(message.payload); if (!data) return false;
        const auto inspect = receiver->inspect(*data, Clock::now()); if (inspect == contest_package::ReceiveResult::rejected) return false;
        if (inspect == contest_package::ReceiveResult::accepted) {
            if (config.memory_fixture) { if (memory_archive.size() + data->data.size() > 8u * 1024u * 1024u) return false; memory_archive.insert(memory_archive.end(), data->data.begin(), data->data.end()); }
            else { LARGE_INTEGER offset{}; offset.QuadPart = static_cast<LONGLONG>(data->offset); DWORD written = 0;
                if (!SetFilePointerEx(archive.value, offset, nullptr, FILE_BEGIN) || !WriteFile(archive.value, data->data.data(), static_cast<DWORD>(data->data.size()), &written, nullptr) || written != data->data.size() || !FlushFileBuffers(archive.value)) return false; }
            if (receiver->accept(*data, Clock::now()) != contest_package::ReceiveResult::accepted) return false;
        }
        const auto ack = receiver->acknowledgment(Clock::now()); if (!ack || !channel.send({Kind::package_ack, transfer_id, contest_package::encode_ack(*ack)}, Clock::now() + std::chrono::seconds(1))) return false;
        { std::scoped_lock lock(mutex); state.received_bytes = receiver->received_bytes(); }
        if (receiver->status(Clock::now()) != contest_package::TransferStatus::complete) return true;
        ExamPackageReport report;
        if (config.memory_fixture) {
            if (!config.memory_fixture->verify_package) return false;
            const auto verified = config.memory_fixture->verify_package(receiver->offer(), memory_archive); if (!verified || verified->digest_hex != receiver->offer().content_digest) return false; report = *verified;
        } else {
            archive = Handle{};
            archive = Handle(CreateFileW(archive_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (!archive.valid()) return false;
            const auto observed = hash_file(archive.value, contest_package::kMaximumArchiveBytes);
            if (!observed || hex(*observed) != receiver->offer().archive_digest || !extract_public(config, archive_path, receiver->offer(), io_enabled)) return false;
            const auto root = config.user_root / L"packages" / receiver->offer().content_digest;
            if (!private_tree(root) || !inspect_exam_package(root, root / L"exam" / L"web", receiver->offer().content_digest, true, report) ||
                !validate_programming_manifest(report.manifest_json) || !validate_exam_host_options(options(report), report)) return false;
        }
        const auto prepared = receiver->prepared(report.digest_hex, receiver->offer().archive_digest, Clock::now());
        if (!prepared || !channel.send({Kind::prepared, transfer_id, contest_package::encode_prepared(*prepared)}, Clock::now() + std::chrono::seconds(1))) return false;
        { std::scoped_lock lock(mutex); state.package = report; state.package_verified = true; state.phase = StudentPhase::prepared; }
        return true;
    }
    bool persist_request(const Request& request) {
        if (!durable_operation(request.operation)) return true;
        // shortcut: retain acknowledged requests within the 256-record/8MiB outbox; add a durable receipt store before increasing the round capacity.
        if (config.memory_fixture ? !config.memory_fixture->save_request(request) : !outbox.enqueue(request)) return false;
        auto records = config.memory_fixture ? config.memory_fixture->load_requests() : outbox.pending_for_client(request.context.client_id);
        if (records.size() > kDurableLimit) return false;
        std::scoped_lock lock(mutex); durable = std::move(records); state.durable_requests = durable.size(); return true;
    }
    bool programming_reply(const contest_room::Message& message) {
        const auto found = inflight.find(message.request_id); if (found == inflight.end()) return false;
        const auto response = exam::programming::parse_response_json(std::string_view(reinterpret_cast<const char*>(message.payload.data()), message.payload.size()), &found->second);
        if (!response) return false;
        // Immutable durable acknowledgments retain their original timestamp after lifecycle refresh/reconnect.
        const bool immutable_ack = durable_operation(found->second.operation) &&
            (response->status == "saved" || response->status == "received" || response->status == "finalized" ||
                (found->second.operation == Operation::submit && response->status == "late"));
        if (!immutable_ack && response->server_time < identity.lifecycle.server_time) return false;
        if (response->status == "unavailable") sent.erase(found->second.request_id);
        std::scoped_lock lock(mutex); if (replies.size() >= 64) replies.pop_front(); replies.push_back(*response); inflight.erase(found); return true;
    }
    void connected() {
        net::TcpSocket socket; if (!socket.connect_with_timeout(config.endpoint.address, config.endpoint.port, 1000) || !socket.set_io_timeouts(500, 500)) { error("teacher connection is unavailable", StudentPhase::disconnected); return; }
        const auto deadline = Clock::now() + config.pairing_timeout; security::Nonce nonce{};
        if (!security::generate_random(nonce) || !contest_room::send_preface(socket, config.realm, contest_room::Side::candidate, nonce, Clock::now() + std::chrono::seconds(1)) ||
            !contest_room::receive_preface(socket, config.realm, contest_room::Side::teacher, Clock::now() + std::chrono::seconds(2), nonce) || (!identity.key_id && !pair(socket, deadline))) { error("teacher confirmation or admission failed", StudentPhase::disconnected); return; }
        const auto client = pairing::client_id_from_uuid(identity.uuid); const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        auto session = client ? control::client_handshake(socket, *client, identity.key_id, identity.key, now) : std::nullopt;
        if (!session || !session->aead_enabled) { error("teacher authenticated encryption failed", StudentPhase::disconnected); return; }
        const auto native = static_cast<SOCKET>(socket.native_handle()); contest_room::RoomChannel channel(std::move(socket), std::move(*session), config.realm, contest_room::Side::candidate);
        if (!channel.is_open()) return;
        { std::scoped_lock lock(mutex); state.connected = true; state.error.clear(); state.sas.clear(); }
        bool have_approval = false, have_lifecycle = false; auto last_activity = Clock::now(), last_heartbeat = Clock::now(); auto last_retry = Clock::time_point{}, persistence_retry = Clock::time_point{};
        inflight.clear(); sent.clear();
        while (running && io_enabled) {
            bool disconnect = false, standings = false; std::optional<Request> request;
            { std::scoped_lock lock(mutex); disconnect = disconnect_requested; if (have_approval && have_lifecycle) { standings = std::exchange(standings_requested, false);
                if (inflight.size() < 4 && !queue.empty() && Clock::now() >= persistence_retry) request = queue.front(); } }
            if (disconnect) break;
            if (request) {
                bool permitted = false; { std::scoped_lock lock(mutex); permitted = request_allowed(*request); }
                if (!permitted) { std::scoped_lock lock(mutex); queue.pop_front(); state.queued_requests = queue.size(); state.error = "room operation is unavailable in this lifecycle"; continue; }
                if (!persist_request(*request)) { std::scoped_lock lock(mutex); state.error = "source preservation failed; keep the editor open"; persistence_retry = Clock::now() + std::chrono::seconds(1); continue; }
                { std::scoped_lock lock(mutex); queue.pop_front(); state.queued_requests = queue.size(); }
                if (next_request == UINT64_MAX) break;
                const auto id = next_request++; if (!channel.send({Kind::programming_request, id, exam::programming::encode_request(*request)}, Clock::now() + std::chrono::seconds(1))) break;
                sent[request->request_id] = exam::programming::encode_request(*request); inflight.emplace(id, std::move(*request));
            }
            if (have_approval && have_lifecycle && Clock::now() - last_retry >= std::chrono::seconds(5)) {
                last_retry = Clock::now(); std::vector<Request> records; { std::scoped_lock lock(mutex); records = durable; }
                for (const auto& saved : records) { if (inflight.size() >= 4 || next_request == UINT64_MAX) break;
                    const bool awaiting = std::any_of(inflight.begin(), inflight.end(), [&](const auto& item) { return item.second.request_id == saved.request_id; }); if (awaiting || (sent.contains(saved.request_id) && sent.at(saved.request_id) == exam::programming::encode_request(saved))) continue;
                    const auto id = next_request++; if (!channel.send({Kind::programming_request, id, exam::programming::encode_request(saved)}, Clock::now() + std::chrono::seconds(1))) return;
                    sent[saved.request_id] = exam::programming::encode_request(saved); inflight.emplace(id, saved); }
            }
            if (standings && identity.live_standings && next_request != UINT64_MAX && !channel.send({Kind::standings_request, next_request++, {}}, Clock::now() + std::chrono::seconds(1))) break;
            if (Clock::now() - last_heartbeat >= std::chrono::seconds(3)) { last_heartbeat = Clock::now(); if (!channel.send({Kind::heartbeat, 0, {}}, Clock::now() + std::chrono::seconds(1))) break; }
            fd_set readable; FD_ZERO(&readable); FD_SET(native, &readable); timeval wait{0, 20000}; const auto ready = select(0, &readable, nullptr, nullptr, &wait);
            if (ready == SOCKET_ERROR || Clock::now() - last_activity > std::chrono::seconds(15)) break;
            if (!ready) continue;
            const auto message = channel.receive(Clock::now() + std::chrono::milliseconds(500)); if (!message) break; last_activity = Clock::now();
            bool valid = false;
            if (message->kind == Kind::join_status) { valid = approval(*message); have_approval = valid; }
            else if (message->kind == Kind::lifecycle && have_approval) { valid = lifecycle(*message); have_lifecycle = valid; }
            else if (message->kind == Kind::package_offer && have_lifecycle) valid = offer(*message, channel);
            else if (message->kind == Kind::package_chunk && have_lifecycle) valid = chunk(*message, channel);
            else if (message->kind == Kind::programming_reply && have_lifecycle) valid = programming_reply(*message);
            else if (message->kind == Kind::standings && have_lifecycle && identity.live_standings) {
                const auto parsed = parse_student_standings(std::string_view(reinterpret_cast<const char*>(message->payload.data()), message->payload.size()), config.realm);
                if (parsed) { std::scoped_lock lock(mutex); valid = true; if (!state.standings || (parsed->revision >= state.standings->revision && parsed->generated_time >= state.standings->generated_time)) state.standings = parsed; }
            }
            if (!valid) { error("teacher room message failed validation; source retained", StudentPhase::disconnected); break; }
        }
        if (receiver && receiver->status(Clock::now()) != contest_package::TransferStatus::prepared) (void)receiver->pause(Clock::now());
        { std::scoped_lock lock(mutex); state.connected = false; state.sas.clear(); if (state.phase != StudentPhase::closed) state.phase = StudentPhase::disconnected; }
    }
    void run() {
        try {
            if (!initialize()) return;
            while (running) {
                bool connect = false; { std::scoped_lock lock(mutex); connect = std::exchange(connect_requested, false); if (connect) { disconnect_requested = false; state.phase = StudentPhase::connecting; } }
                if (connect) { io_enabled = true; connected();
                    bool retry = false; { std::scoped_lock lock(mutex); retry = running && identity.key_id && !disconnect_requested; }
                    if (retry) { const auto deadline = Clock::now() + std::chrono::seconds(1); while (running && Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(20));
                        std::scoped_lock lock(mutex); if (!disconnect_requested) connect_requested = true; } }
                else {
                    std::optional<Request> pending;
                    { std::scoped_lock lock(mutex); const auto found = std::find_if(queue.begin(), queue.end(), [](const Request& r) { return durable_operation(r.operation); }); if (found != queue.end()) pending = *found; }
                    if (pending) { const bool preserved = persist_request(*pending); std::scoped_lock lock(mutex);
                        if (!preserved) state.error = "source preservation failed; keep the editor open";
                        else { const auto found = std::find_if(queue.begin(), queue.end(), [&](const Request& r) { return r.request_id == pending->request_id; }); if (found != queue.end()) queue.erase(found); state.queued_requests = queue.size(); } }
                    std::this_thread::sleep_for(std::chrono::milliseconds(pending ? 100 : 20));
                }
            }
        } catch (...) { error("student session failed; existing source retained"); }
        outbox.close();
    }
};

ContestStudentSession::ContestStudentSession() : impl_(std::make_shared<Impl>()) {}
ContestStudentSession::~ContestStudentSession() { stop(); }
bool ContestStudentSession::start(ContestStudentConfig config, std::string* error) {
    if (impl_->running || !contest_room::encode_realm(config.realm) || !valid_label(config.display_name) || !config.endpoint.port ||
        (!config.room_label.empty() && !valid_label(config.room_label)) ||
        config.endpoint.address.empty() || config.endpoint.address.size() > 15 || config.endpoint.address.find('\0') != std::string::npos || config.pairing_timeout < std::chrono::milliseconds(100) || config.pairing_timeout > std::chrono::minutes(2)) { fail(error, "student room configuration is invalid or already active"); return false; }
    IN_ADDR address{}; if (InetPtonA(AF_INET, config.endpoint.address.c_str(), &address) != 1) { fail(error, "student endpoint must be a literal IPv4 address"); return false; }
    impl_ = std::make_shared<Impl>(); impl_->config = std::move(config); impl_->running = true;
    auto owner = impl_; impl_->worker = std::thread([owner] { owner->run(); }); return true;
}
void ContestStudentSession::stop() noexcept {
    try { impl_->running = false; impl_->io_enabled = false; { std::scoped_lock lock(impl_->mutex); impl_->capture.reset(); impl_->disconnect_requested = true; }
        if (impl_->worker.joinable()) impl_->worker.join(); } catch (...) {}
}
bool ContestStudentSession::confirm_sas(std::string_view sas) {
    std::scoped_lock lock(impl_->mutex); if (!impl_->running || impl_->state.phase != StudentPhase::confirm_teacher || sas != impl_->state.sas || sas.size() != pairing::kSasDigits) return false;
    impl_->sas_decision = 1; return true;
}
void ContestStudentSession::cancel_pairing() noexcept { try { impl_->io_enabled = false; std::scoped_lock lock(impl_->mutex); impl_->sas_decision = -1; impl_->disconnect_requested = true; } catch (...) {} }
bool ContestStudentSession::reconnect() {
    std::scoped_lock lock(impl_->mutex); if (!impl_->running || impl_->state.connected || impl_->connect_requested || impl_->state.phase == StudentPhase::connecting || impl_->state.phase == StudentPhase::confirm_teacher || impl_->state.phase == StudentPhase::awaiting_admission) return false;
    impl_->connect_requested = true; return true;
}
void ContestStudentSession::disconnect() noexcept { try { impl_->io_enabled = false; std::scoped_lock lock(impl_->mutex); impl_->disconnect_requested = true; } catch (...) {} }
bool ContestStudentSession::queue_programming(const Request& request) { return impl_->enqueue(request); }
bool ContestStudentSession::request_standings() {
    std::scoped_lock lock(impl_->mutex); const auto now = Clock::now();
    if (!impl_->running || !impl_->state.connected || !impl_->state.context || !impl_->state.live_standings || now - impl_->last_standings < std::chrono::seconds(5)) return false;
    impl_->last_standings = now; impl_->standings_requested = true; return true;
}
ContestStudentSnapshot ContestStudentSession::snapshot() const { std::scoped_lock lock(impl_->mutex); auto result = impl_->state; result.server_time = impl_->estimated_time(); return result; }
std::vector<exam::programming::Response> ContestStudentSession::take_replies() {
    std::scoped_lock lock(impl_->mutex); std::vector<exam::programming::Response> result(impl_->replies.begin(), impl_->replies.end()); impl_->replies.clear(); return result;
}
std::optional<ExamHostOptions> ContestStudentSession::host_options() const {
    std::scoped_lock lock(impl_->mutex); if (!impl_->running || !impl_->state.package_verified || !impl_->state.package || !impl_->state.context || impl_->config.memory_fixture) return {};
    // Identity writes belong to the worker; derive only from its published tuple.
    ExamHostOptions options; options.policy = ExamHostPolicy::ordinary_user; options.package_root = impl_->state.package->package_root; options.web_root = impl_->state.package->web_root;
    options.allowed_package_root = impl_->config.user_root / L"packages"; options.allowed_user_data_root = impl_->config.user_root / L"profiles";
    auto namespace_wire = *contest_room::encode_realm(impl_->config.realm); const auto tuple = exam::encode_state_request(*impl_->state.context); namespace_wire.insert(namespace_wire.end(), tuple.begin(), tuple.end());
    const auto digest = security::sha256(namespace_wire); if (!digest) return {};
    options.user_data_root = options.allowed_user_data_root / hex(*digest); options.expected_digest_hex = hex(impl_->state.context->package_digest);
    options.context_json = exam::programming::context_json(*impl_->state.context); options.context_json.pop_back(); options.context_json += ",\"synchronizedStart\":true}"; return options;
}
bool ContestStudentSession::preserve_capture(const ExamCloseCaptureReport& report, const std::map<std::string, Request>& pending) const { return impl_->preserve(report, pending); }
ExamHostCallbacks ContestStudentSession::host_callbacks() {
    std::weak_ptr<Impl> weak = impl_; ExamHostCallbacks callbacks;
    callbacks.enqueue_programming_request = [weak](const Request& request) { const auto owner = weak.lock(); return owner && owner->enqueue(request); };
    callbacks.report_ready = [weak](const exam::ExamReadyReport& ready) { const auto owner = weak.lock(); if (!owner) return false; std::scoped_lock lock(owner->mutex);
        const exam::StateRequest context{ready.package_id, ready.package_digest, ready.client_id, ready.session_id, ready.candidate_id};
        return owner->running && owner->state.package_verified && owner->state.context && same_context(*owner->state.context, context) && !owner->state.recovery_only; };
    callbacks.capture_updated = [weak](std::optional<ExamCloseCaptureReport> report) { const auto owner = weak.lock(); if (!owner) return; std::scoped_lock lock(owner->mutex); owner->capture = std::move(report); };
    callbacks.before_close = [weak](const std::map<std::string, Request>& pending) { const auto owner = weak.lock(); if (!owner) return false;
        std::scoped_lock lock(owner->mutex); return owner->running && owner->state.context && (owner->capture ? student_capture_preserved(*owner->capture, *owner->state.context, owner->durable, pending) : pending.empty()); };
    return callbacks;
}

} // namespace nstu::client
