#pragma once
#include <windows.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace nstu::server::authority {
using SidBytes = std::vector<std::byte>;
using Clock = std::chrono::steady_clock;
template<class T> struct ApiResult {
    bool ok{false};
    DWORD error{ERROR_INVALID_DATA};
    T value{};
};
struct TokenGroup { SidBytes sid; DWORD attributes{0}; };
struct TokenFacts {
    SidBytes user;
    std::vector<TokenGroup> groups;
    LUID authentication_id{};
    DWORD session_id{0};
    TOKEN_TYPE type{TokenPrimary};
    SECURITY_IMPERSONATION_LEVEL level{SecurityAnonymous};
    bool restricted{false}; // Successfully queried ever-filtered TokenHasRestrictions.
    bool app_container{false};
    bool anonymous{false};
};
struct AccountFacts {
    SidBytes sid;
    std::wstring name;
    std::wstring domain;
    SID_NAME_USE type{SidTypeUnknown};
};
struct DirectLocalGroups {
    DWORD status{ERROR_INVALID_DATA};
    DWORD entries{0};
    DWORD total{0};
    std::size_t buffer_bytes{0};
    bool structure_valid{false};
    std::vector<std::wstring> aliases;
};
struct SessionFacts { bool known{false}; bool active{false}; bool unlocked{false}; };
struct LocalRoleBinding {
    std::wstring qualified_local_alias;
    SidBytes expected_sid;
    bool operator==(const LocalRoleBinding&) const = default;
};
struct ManagedRoleBindings {
    std::optional<LocalRoleBinding> teacher;
    std::optional<LocalRoleBinding> technician;
    std::uint64_t generation{0};
    bool operator==(const ManagedRoleBindings&) const = default;
};
struct RoleGrants { bool teacher{false}; bool technician{false}; };
enum class AuthorizationStatus {
    authorized, unprovisioned, invalid_binding, unsupported_token,
    identity_unavailable, impersonation_failed, message_invalid,
    membership_unavailable, membership_incomplete, not_direct_member,
    session_unavailable, session_locked, session_inactive, evidence_expired,
    binding_changed, generation_changed, connection_busy, connection_invalid,
    outer_impersonation_present, entry_context_unavailable, foreign_evidence
};

// A trusted test backend is never selected by IPC input. Native construction
// uses real Windows APIs. Returned token/account/name storage must be owned.
class WindowsAuthorizationApi {
public:
    virtual ~WindowsAuthorizationApi() = default;
    virtual bool native_backend() const noexcept { return false; }
    virtual DWORD entry_thread_context() = 0;
    virtual ApiResult<bool> inspect_local_server_pipe(HANDLE) = 0;
    virtual ApiResult<std::vector<std::byte>> read_message(HANDLE, DWORD maximum) = 0;
    virtual ApiResult<bool> impersonate_pipe(HANDLE) = 0;
    virtual ApiResult<HANDLE> open_thread_token(DWORD access) = 0;
    virtual ApiResult<HANDLE> duplicate_query_token(HANDLE, DWORD access) = 0;
    virtual ApiResult<TokenFacts> token_facts(HANDLE) = 0;
    virtual ApiResult<TokenFacts> own_process_facts() = 0;
    virtual bool revert_to_self() noexcept = 0;
    virtual void close_handle(HANDLE) noexcept = 0;
    virtual void disconnect_pipe(HANDLE) noexcept = 0;
    virtual ApiResult<std::wstring> local_machine_name() = 0;
    virtual ApiResult<AccountFacts> account_from_sid(std::span<const std::byte>) = 0;
    virtual ApiResult<AccountFacts> account_from_name(const std::wstring&) = 0;
    virtual DirectLocalGroups direct_local_groups(const std::wstring& qualified_user,
        DWORD level, DWORD flags, DWORD preferred_bytes) = 0;
    virtual ApiResult<SessionFacts> session_facts(DWORD session_id) = 0;
    virtual Clock::time_point now() noexcept = 0;
};
class OwnedOperatorPipeConnection {
public:
    OwnedOperatorPipeConnection() noexcept;
    ~OwnedOperatorPipeConnection();
    OwnedOperatorPipeConnection(OwnedOperatorPipeConnection&&) noexcept;
    OwnedOperatorPipeConnection& operator=(OwnedOperatorPipeConnection&&) noexcept;
    OwnedOperatorPipeConnection(const OwnedOperatorPipeConnection&) = delete;
    OwnedOperatorPipeConnection& operator=(const OwnedOperatorPipeConnection&) = delete;
    [[nodiscard]] bool valid() const noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
    explicit OwnedOperatorPipeConnection(std::unique_ptr<State>) noexcept;
    friend class WindowsOperatorAuthorizationProvider;
};
class AuthorizedPipeMessage {
public:
    ~AuthorizedPipeMessage();
    AuthorizedPipeMessage(AuthorizedPipeMessage&&) noexcept;
    AuthorizedPipeMessage& operator=(AuthorizedPipeMessage&&) noexcept;
    AuthorizedPipeMessage(const AuthorizedPipeMessage&) = delete;
    AuthorizedPipeMessage& operator=(const AuthorizedPipeMessage&) = delete;
    [[nodiscard]] RoleGrants grants() const noexcept;
    [[nodiscard]] std::span<const std::byte> message() const noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
    explicit AuthorizedPipeMessage(std::unique_ptr<State>) noexcept;
    friend class WindowsOperatorAuthorizationProvider;
};
struct AuthorizationResult {
    AuthorizationStatus status{AuthorizationStatus::identity_unavailable};
    DWORD native_error{0};
    bool token_captured{false};
    bool reverted{false};
    RoleGrants role_grants{};
    std::unique_ptr<AuthorizedPipeMessage> evidence;
    [[nodiscard]] bool authorized() const noexcept;
};
struct OwnProcessObservation {
    bool token_query_succeeded{false};
    std::size_t enabled_group_count{0};
    std::size_t deny_only_group_count{0};
    bool session_query_succeeded{false};
    bool session_active{false};
    bool session_unlocked{false};
    bool restricted{false};
    bool direct_local_query_succeeded{false};
    std::size_t direct_local_group_count{0};
    RoleGrants managed_grants{}; // Observation cannot establish a managed role.
};
using TokenInformationQuery =
    BOOL (WINAPI*)(HANDLE, TOKEN_INFORMATION_CLASS, LPVOID, DWORD, PDWORD);
struct NativeTokenClassificationObservation {
    bool query_succeeded{false};
    DWORD native_error{0};
    bool ever_filtered{false};
    bool supported_shape{false};
};
// Read-only developer observation; never creates role/evidence/operation authority.
[[nodiscard]] NativeTokenClassificationObservation observe_owned_token_classification_read_only(
    HANDLE owned_token, TokenInformationQuery query = ::GetTokenInformation);

class WindowsOperatorAuthorizationProvider {
public:
    WindowsOperatorAuthorizationProvider();
    WindowsOperatorAuthorizationProvider(const WindowsOperatorAuthorizationProvider&)=delete;
    WindowsOperatorAuthorizationProvider& operator=(const WindowsOperatorAuthorizationProvider&)=delete;
    WindowsOperatorAuthorizationProvider(WindowsOperatorAuthorizationProvider&&)=delete;
    WindowsOperatorAuthorizationProvider& operator=(WindowsOperatorAuthorizationProvider&&)=delete;
    explicit WindowsOperatorAuthorizationProvider(
        std::shared_ptr<WindowsAuthorizationApi> trusted_test_api);
    // Transfer must be exclusive, with no duplicate/borrowed endpoint aliases.
    // This wrapper exposes no raw handle or alternative read method.
    [[nodiscard]] OwnedOperatorPipeConnection adopt_connected_operator_pipe(HANDLE&);
    [[nodiscard]] AuthorizationResult receive_authorized_pipe_message(
        OwnedOperatorPipeConnection&, const ManagedRoleBindings&);
    [[nodiscard]] AuthorizationResult revalidate_for_dispatch(
        const AuthorizedPipeMessage&, const ManagedRoleBindings&);
    [[nodiscard]] OwnProcessObservation observe_own_process_read_only();
private:
    std::shared_ptr<WindowsAuthorizationApi> api_;
    std::shared_ptr<const int> origin_;
    bool native_{false};
    AuthorizationResult validate(const AuthorizedPipeMessage::State&, const ManagedRoleBindings&);
};
}
