#include "nstu/windows_operator_authorization.hpp"
#include <lm.h>
#include <wtsapi32.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace nstu::server::authority {
namespace {
constexpr DWORD kMaximumMessage = 4096;
constexpr DWORD kPreferredGroupBytes = 32768;
constexpr std::size_t kMaximumBuffer = 1024 * 1024;
constexpr std::size_t kMaximumGroups = 4096;
constexpr auto kEvidenceLifetime = std::chrono::milliseconds(250);

template<class T> ApiResult<T> success(T value) { return {true, 0, std::move(value)}; }
template<class T> ApiResult<T> failure(DWORD error) { return {false, error, {}}; }
bool handle_valid(HANDLE value) noexcept { return value && value != INVALID_HANDLE_VALUE; }
bool valid_sid(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() < 8 || std::to_integer<unsigned>(bytes[0]) != SID_REVISION) return false;
    const auto count = std::to_integer<unsigned>(bytes[1]);
    if (count > SID_MAX_SUB_AUTHORITIES || bytes.size() != 8 + 4 * count) return false;
    return IsValidSid(const_cast<std::byte*>(bytes.data())) != FALSE;
}
bool equal_sid(std::span<const std::byte> a, std::span<const std::byte> b) noexcept {
    return valid_sid(a) && valid_sid(b) &&
        EqualSid(const_cast<std::byte*>(a.data()), const_cast<std::byte*>(b.data())) != FALSE;
}
bool name_equal(const std::wstring& a, const std::wstring& b) noexcept {
    if (a.size() > 1024 || b.size() > 1024) return false;
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()),
        b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}
bool simple_name(const std::wstring& name) {
    return !name.empty() && name.size() <= 256 &&
        name.find_first_of(L"\\/\0", 0, 3) == std::wstring::npos;
}
bool same_logon(const TokenFacts& a, const TokenFacts& b) noexcept {
    return equal_sid(a.user,b.user) && a.session_id==b.session_id &&
        a.authentication_id.LowPart==b.authentication_id.LowPart &&
        a.authentication_id.HighPart==b.authentication_id.HighPart;
}
bool supported(const TokenFacts& token) noexcept {
    return valid_sid(token.user) && token.type == TokenImpersonation &&
        token.level >= SecurityImpersonation && token.level <= SecurityDelegation &&
        !token.restricted && !token.app_container && !token.anonymous;
}
bool enabled_group(const TokenFacts& token, const SidBytes& sid) {
    return std::any_of(token.groups.begin(),token.groups.end(),[&](const auto& group) {
        return equal_sid(group.sid,sid) && (group.attributes & SE_GROUP_ENABLED) != 0 &&
            (group.attributes & SE_GROUP_USE_FOR_DENY_ONLY) == 0;
    });
}
AuthorizationResult denied(AuthorizationStatus status, DWORD error=0,
                            bool captured=false, bool reverted=false) {
    AuthorizationResult result;
    result.status=status; result.native_error=error;
    result.token_captured=captured; result.reverted=reverted;
    return result;
}
struct TokenOwner {
    std::shared_ptr<WindowsAuthorizationApi> api;
    HANDLE handle{nullptr};
    TokenOwner()=default;
    TokenOwner(std::shared_ptr<WindowsAuthorizationApi> backend,HANDLE value)
        :api(std::move(backend)),handle(value){}
    ~TokenOwner(){if(handle_valid(handle))api->close_handle(handle);}
    TokenOwner(TokenOwner&& other) noexcept
        :api(std::move(other.api)),handle(std::exchange(other.handle,nullptr)){}
    TokenOwner& operator=(TokenOwner&&)=delete;
    TokenOwner(const TokenOwner&)=delete;
};
struct ImpersonationGuard {
    WindowsAuthorizationApi& api;
    bool active{true};
    ~ImpersonationGuard(){finish();}
    void finish() noexcept {
        if(active){active=false;if(!api.revert_to_self())std::terminate();}
    }
};
struct NetBuffer {
    BYTE* value{nullptr};
    ~NetBuffer(){if(value)NetApiBufferFree(value);}
};

ApiResult<std::vector<std::byte>> token_blob(HANDLE token,TOKEN_INFORMATION_CLASS kind,
    TokenInformationQuery query) {
    DWORD needed=0;
    if(query(token,kind,nullptr,0,&needed) ||
       GetLastError()!=ERROR_INSUFFICIENT_BUFFER || needed==0 || needed>kMaximumBuffer)
        return failure<std::vector<std::byte>>(ERROR_INVALID_DATA);
    std::vector<std::byte> bytes(needed);
    if(!query(token,kind,bytes.data(),needed,&needed))
        return failure<std::vector<std::byte>>(GetLastError());
    bytes.resize(needed);
    return success(std::move(bytes));
}
template<class T> ApiResult<T> token_value(HANDLE token,TOKEN_INFORMATION_CLASS kind,
    TokenInformationQuery query) {
    T value{};DWORD bytes=0;
    if(!query(token,kind,&value,sizeof(value),&bytes))
        return failure<T>(GetLastError());
    if(bytes!=sizeof(value))return failure<T>(ERROR_INVALID_DATA);
    return success(value);
}
ApiResult<bool> token_has_restrictions(HANDLE token,TokenInformationQuery query) {
    alignas(DWORD) std::array<std::byte,sizeof(DWORD)> field{};
    DWORD returned=0;
    if(!query(token,TokenHasRestrictions,field.data(),
              static_cast<DWORD>(field.size()),&returned))
        return failure<bool>(GetLastError());
    // Native Windows observation returns BOOLEAN1; Microsoft documents DWORD4.
    // Decode only these two validated prefixes, never unwritten trailing bytes.
    if(returned==sizeof(BYTE))return success(std::to_integer<BYTE>(field[0])!=0);
    if(returned==sizeof(DWORD)){
        DWORD value=0;
        std::memcpy(&value,field.data(),sizeof(value));
        return success(value!=0);
    }
    return failure<bool>(ERROR_INVALID_DATA);
}
SidBytes sid_in_blob(PSID sid,std::span<const std::byte> blob) {
    const auto start=reinterpret_cast<std::uintptr_t>(blob.data());
    const auto pointer=reinterpret_cast<std::uintptr_t>(sid);
    if(pointer<start || pointer-start>blob.size() || blob.size()-(pointer-start)<8)return {};
    auto remaining=blob.subspan(pointer-start);
    const auto count=std::to_integer<unsigned>(remaining[1]);
    const auto size=std::size_t{8}+std::size_t{4}*count;
    if(count>SID_MAX_SUB_AUTHORITIES || size>remaining.size())return {};
    SidBytes result(remaining.begin(),remaining.begin()+static_cast<std::ptrdiff_t>(size));
    return valid_sid(result)?result:SidBytes{};
}
class NativeApi final : public WindowsAuthorizationApi {
public:
    explicit NativeApi(TokenInformationQuery query = ::GetTokenInformation) : query_(query) {}
    bool native_backend() const noexcept override {return true;}
    DWORD entry_thread_context() override {
        HANDLE token=nullptr;
        if(OpenThreadToken(GetCurrentThread(),TOKEN_QUERY,TRUE,&token)){
            CloseHandle(token);return ERROR_SUCCESS;
        }
        return GetLastError();
    }
    ApiResult<bool> inspect_local_server_pipe(HANDLE pipe) override {
        DWORD flags=0,mode=0;
        if(!GetNamedPipeInfo(pipe,&flags,nullptr,nullptr,nullptr) ||
           !GetNamedPipeHandleStateW(pipe,&mode,nullptr,nullptr,nullptr,nullptr,0))
            return failure<bool>(GetLastError());
        if((flags&PIPE_SERVER_END)==0 || (flags&PIPE_TYPE_MESSAGE)==0 ||
           (mode&PIPE_READMODE_MESSAGE)==0)return failure<bool>(ERROR_INVALID_HANDLE);
        std::array<wchar_t,256> client{};
        // Local NPFS endpoints report ERROR_PIPE_LOCAL, rather than a name.
        // Any remote-computer success or other error is denied.
        if(!GetNamedPipeClientComputerNameW(pipe,client.data(),
                                           static_cast<ULONG>(sizeof(client))) &&
           GetLastError()==ERROR_PIPE_LOCAL) return success(true);
        return failure<bool>(ERROR_ACCESS_DENIED);
    }
    ApiResult<std::vector<std::byte>> read_message(HANDLE pipe,DWORD maximum) override {
        std::vector<std::byte> message(maximum);
        DWORD read=0;
        if(!ReadFile(pipe,message.data(),maximum,&read,nullptr))
            return failure<std::vector<std::byte>>(GetLastError());
        if(read==0 || read>maximum)return failure<std::vector<std::byte>>(ERROR_INVALID_DATA);
        message.resize(read);return success(std::move(message));
    }
    ApiResult<bool> impersonate_pipe(HANDLE pipe) override {
        if(!ImpersonateNamedPipeClient(pipe))return failure<bool>(GetLastError());
        return success(true);
    }
    ApiResult<HANDLE> open_thread_token(DWORD access) override {
        HANDLE token=nullptr;
        if(!OpenThreadToken(GetCurrentThread(),access,TRUE,&token))
            return failure<HANDLE>(GetLastError());
        return success(token);
    }
    ApiResult<HANDLE> duplicate_query_token(HANDLE token,DWORD access) override {
        HANDLE duplicated=nullptr;
        if(!DuplicateTokenEx(token,access,nullptr,SecurityImpersonation,
                             TokenImpersonation,&duplicated))
            return failure<HANDLE>(GetLastError());
        return success(duplicated);
    }
    ApiResult<TokenFacts> token_facts(HANDLE token) override {
        TokenFacts result;
        auto restrictions=token_has_restrictions(token,query_);
        if(!restrictions.ok)return failure<TokenFacts>(restrictions.error);
        result.restricted=restrictions.value;
        auto statistics=token_value<TOKEN_STATISTICS>(token,TokenStatistics,query_);
        auto session=token_value<DWORD>(token,TokenSessionId,query_);
        auto container=token_value<DWORD>(token,TokenIsAppContainer,query_);
        auto user=token_blob(token,TokenUser,query_);
        auto groups=token_blob(token,TokenGroups,query_);
        if(!statistics.ok || !session.ok || !container.ok || !user.ok || !groups.ok)
            return failure<TokenFacts>(ERROR_INVALID_DATA);
        result.type=statistics.value.TokenType;
        result.authentication_id=statistics.value.AuthenticationId;
        result.session_id=session.value;
        result.app_container=container.value!=0;
        if(result.type==TokenImpersonation){
            auto level=token_value<SECURITY_IMPERSONATION_LEVEL>(token,TokenImpersonationLevel,query_);
            if(!level.ok)return failure<TokenFacts>(level.error);
            result.level=level.value;
        }
        if(user.value.size()<sizeof(TOKEN_USER))return failure<TokenFacts>(ERROR_INVALID_DATA);
        result.user=sid_in_blob(reinterpret_cast<TOKEN_USER*>(user.value.data())->User.Sid,user.value);
        if(!valid_sid(result.user))return failure<TokenFacts>(ERROR_INVALID_DATA);
        std::array<std::byte,SECURITY_MAX_SID_SIZE> anonymous{};
        DWORD anonymous_size=static_cast<DWORD>(anonymous.size());
        if(!CreateWellKnownSid(WinAnonymousSid,nullptr,anonymous.data(),&anonymous_size))
            return failure<TokenFacts>(GetLastError());
        result.anonymous=equal_sid(result.user,std::span(anonymous).first(anonymous_size));
        if(groups.value.size()<offsetof(TOKEN_GROUPS,Groups))
            return failure<TokenFacts>(ERROR_INVALID_DATA);
        const auto* list=reinterpret_cast<TOKEN_GROUPS*>(groups.value.data());
        if(list->GroupCount>kMaximumGroups ||
           offsetof(TOKEN_GROUPS,Groups)+std::size_t{list->GroupCount}*sizeof(SID_AND_ATTRIBUTES)>
           groups.value.size())return failure<TokenFacts>(ERROR_INVALID_DATA);
        for(DWORD index=0;index<list->GroupCount;++index){
            auto group_sid=sid_in_blob(list->Groups[index].Sid,groups.value);
            if(!valid_sid(group_sid))return failure<TokenFacts>(ERROR_INVALID_DATA);
            result.groups.push_back({std::move(group_sid),list->Groups[index].Attributes});
        }
        return success(std::move(result));
    }
    ApiResult<TokenFacts> own_process_facts() override {
        HANDLE token=nullptr;
        if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))
            return failure<TokenFacts>(GetLastError());
        struct Close {HANDLE value;~Close(){CloseHandle(value);}} owner{token};
        return token_facts(token);
    }
    bool revert_to_self() noexcept override {return RevertToSelf()!=FALSE;}
    void close_handle(HANDLE handle) noexcept override {CloseHandle(handle);}
    void disconnect_pipe(HANDLE handle) noexcept override {DisconnectNamedPipe(handle);}
    ApiResult<std::wstring> local_machine_name() override {
        std::array<wchar_t,MAX_COMPUTERNAME_LENGTH+1> name{};
        DWORD length=static_cast<DWORD>(name.size());
        if(!GetComputerNameW(name.data(),&length))return failure<std::wstring>(GetLastError());
        return success(std::wstring(name.data(),length));
    }
    ApiResult<AccountFacts> account_from_sid(std::span<const std::byte> sid) override {
        if(!valid_sid(sid))return failure<AccountFacts>(ERROR_INVALID_SID);
        DWORD name_size=0,domain_size=0;SID_NAME_USE type=SidTypeUnknown;
        LookupAccountSidW(nullptr,const_cast<std::byte*>(sid.data()),nullptr,&name_size,
                          nullptr,&domain_size,&type);
        if(GetLastError()!=ERROR_INSUFFICIENT_BUFFER || name_size==0 || name_size>257 ||
           domain_size==0 || domain_size>257)return failure<AccountFacts>(ERROR_NONE_MAPPED);
        std::vector<wchar_t> name(name_size),domain(domain_size);
        if(!LookupAccountSidW(nullptr,const_cast<std::byte*>(sid.data()),name.data(),&name_size,
                             domain.data(),&domain_size,&type))
            return failure<AccountFacts>(GetLastError());
        return success(AccountFacts{SidBytes(sid.begin(),sid.end()),name.data(),domain.data(),type});
    }
    ApiResult<AccountFacts> account_from_name(const std::wstring& name) override {
        DWORD sid_size=0,domain_size=0;SID_NAME_USE type=SidTypeUnknown;
        LookupAccountNameW(nullptr,name.c_str(),nullptr,&sid_size,nullptr,&domain_size,&type);
        if(GetLastError()!=ERROR_INSUFFICIENT_BUFFER || sid_size<8 ||
           sid_size>SECURITY_MAX_SID_SIZE || domain_size==0 || domain_size>257)
            return failure<AccountFacts>(ERROR_NONE_MAPPED);
        SidBytes bytes(sid_size);std::vector<wchar_t> domain(domain_size);
        if(!LookupAccountNameW(nullptr,name.c_str(),bytes.data(),&sid_size,
                              domain.data(),&domain_size,&type))
            return failure<AccountFacts>(GetLastError());
        bytes.resize(sid_size);
        const auto separator=name.find(L'\\');
        if(separator==std::wstring::npos || !valid_sid(bytes))
            return failure<AccountFacts>(ERROR_INVALID_DATA);
        return success(AccountFacts{std::move(bytes),name.substr(separator+1),domain.data(),type});
    }
    DirectLocalGroups direct_local_groups(const std::wstring& user,DWORD level,
                                          DWORD flags,DWORD preferred) override {
        DirectLocalGroups result;NetBuffer buffer;
        result.status=NetUserGetLocalGroups(nullptr,user.c_str(),level,flags,&buffer.value,
                                            preferred,&result.entries,&result.total);
        if(result.status!=NERR_Success)return result; // No resume context; buffer freed.
        if(!buffer.value){
            result.structure_valid=result.entries==0 && result.total==0;return result;
        }
        DWORD size=0;
        if(NetApiBufferSize(buffer.value,&size)!=NERR_Success)return result;
        result.buffer_bytes=size;
        if(result.entries>kMaximumGroups || size>kMaximumBuffer ||
           std::size_t{result.entries}*sizeof(LOCALGROUP_USERS_INFO_0)>size)return result;
        const auto* list=reinterpret_cast<LOCALGROUP_USERS_INFO_0*>(buffer.value);
        const auto start=reinterpret_cast<std::uintptr_t>(buffer.value);
        for(DWORD index=0;index<result.entries;++index){
            const auto pointer=reinterpret_cast<std::uintptr_t>(list[index].lgrui0_name);
            if(pointer<start || pointer-start>=size || (pointer-start)%alignof(wchar_t)!=0)
                return result;
            const auto remaining=(size-(pointer-start))/sizeof(wchar_t);
            const auto maximum=std::min<std::size_t>(remaining,257);
            std::size_t length=0;
            while(length<maximum && list[index].lgrui0_name[length]!=L'\0')++length;
            if(length==maximum || length==0)return result;
            result.aliases.emplace_back(list[index].lgrui0_name,length);
        }
        result.structure_valid=true;return result;
    }
    ApiResult<SessionFacts> session_facts(DWORD session) override {
        LPWSTR data=nullptr;DWORD size=0;
        if(!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE,session,WTSSessionInfoEx,
                                        &data,&size))
            return failure<SessionFacts>(GetLastError());
        struct Free {LPWSTR value;~Free(){WTSFreeMemory(value);}} owner{data};
        if(size<sizeof(WTSINFOEXW))return failure<SessionFacts>(ERROR_INVALID_DATA);
        const auto* info=reinterpret_cast<const WTSINFOEXW*>(data);
        if(info->Level!=1 || info->Data.WTSInfoExLevel1.SessionId!=session)
            return failure<SessionFacts>(ERROR_INVALID_DATA);
        const auto& value=info->Data.WTSInfoExLevel1;
        const bool known=value.SessionFlags==WTS_SESSIONSTATE_LOCK ||
                         value.SessionFlags==WTS_SESSIONSTATE_UNLOCK;
        return success(SessionFacts{known,value.SessionState==WTSActive,
                                     value.SessionFlags==WTS_SESSIONSTATE_UNLOCK});
    }
    Clock::time_point now() noexcept override {return Clock::now();}
private:
    TokenInformationQuery query_;
};
std::shared_ptr<WindowsAuthorizationApi> native_api() {
    static auto instance=std::make_shared<NativeApi>();
    return instance;
}
}

NativeTokenClassificationObservation observe_owned_token_classification_read_only(
    HANDLE owned_token,TokenInformationQuery query) {
    NativeTokenClassificationObservation result;
    if(!query || !handle_valid(owned_token)){result.native_error=ERROR_INVALID_PARAMETER;return result;}
    try {
        NativeApi adapter(query);
        const auto facts=adapter.token_facts(owned_token);
        result.query_succeeded=facts.ok;
        result.native_error=facts.error;
        if(facts.ok){result.ever_filtered=facts.value.restricted;result.supported_shape=supported(facts.value);}
    } catch(...) {result.native_error=ERROR_INVALID_DATA;}
    return result;
}

struct OwnedOperatorPipeConnection::State {
    std::shared_ptr<WindowsAuthorizationApi> api;
    HANDLE handle{nullptr};
    std::atomic_flag busy=ATOMIC_FLAG_INIT;
    std::atomic<bool> invalid{false};
    ~State(){if(handle_valid(handle))api->close_handle(handle);}
    void invalidate() noexcept {
        invalid.store(true);
        if(handle_valid(handle)){
            api->disconnect_pipe(handle);
            api->close_handle(std::exchange(handle,nullptr));
        }
    }
};
struct AuthorizedPipeMessage::State {
    TokenOwner token;
    TokenFacts initial;
    ManagedRoleBindings bindings;
    Clock::time_point captured_at;
    std::vector<std::byte> bytes;
    RoleGrants granted;
    std::shared_ptr<const int> origin;
    bool native{false};
};
OwnedOperatorPipeConnection::OwnedOperatorPipeConnection() noexcept=default;
OwnedOperatorPipeConnection::~OwnedOperatorPipeConnection()=default;
OwnedOperatorPipeConnection::OwnedOperatorPipeConnection(OwnedOperatorPipeConnection&&) noexcept=default;
OwnedOperatorPipeConnection& OwnedOperatorPipeConnection::operator=(OwnedOperatorPipeConnection&&) noexcept=default;
OwnedOperatorPipeConnection::OwnedOperatorPipeConnection(std::unique_ptr<State> state) noexcept
    :state_(std::move(state)){}
bool OwnedOperatorPipeConnection::valid() const noexcept {
    return state_ && !state_->invalid.load();
}
AuthorizedPipeMessage::AuthorizedPipeMessage(std::unique_ptr<State> state) noexcept
    :state_(std::move(state)){}
AuthorizedPipeMessage::~AuthorizedPipeMessage()=default;
AuthorizedPipeMessage::AuthorizedPipeMessage(AuthorizedPipeMessage&&) noexcept=default;
AuthorizedPipeMessage& AuthorizedPipeMessage::operator=(AuthorizedPipeMessage&&) noexcept=default;
RoleGrants AuthorizedPipeMessage::grants() const noexcept {return state_?state_->granted:RoleGrants{};}
std::span<const std::byte> AuthorizedPipeMessage::message() const noexcept {
    return state_?std::span<const std::byte>(state_->bytes):std::span<const std::byte>{};
}
bool AuthorizationResult::authorized() const noexcept {
    return status==AuthorizationStatus::authorized && (role_grants.teacher || role_grants.technician);
}
WindowsOperatorAuthorizationProvider::WindowsOperatorAuthorizationProvider()
    :api_(native_api()),origin_(std::make_shared<const int>(0)),native_(true){}
WindowsOperatorAuthorizationProvider::WindowsOperatorAuthorizationProvider(
    std::shared_ptr<WindowsAuthorizationApi> api)
    :api_(std::move(api)),origin_(std::make_shared<const int>(0)) {
    if(!api_ || api_->native_backend())
        throw std::invalid_argument("injection constructor requires a test backend");
}
OwnedOperatorPipeConnection WindowsOperatorAuthorizationProvider::adopt_connected_operator_pipe(
    HANDLE& handle) {
    auto state=std::make_unique<OwnedOperatorPipeConnection::State>();
    state->api=api_;state->handle=std::exchange(handle,INVALID_HANDLE_VALUE);
    try {
        auto result=api_->inspect_local_server_pipe(state->handle);
        if(!handle_valid(state->handle) || !result.ok || !result.value)state->invalidate();
    } catch(...) {state->invalidate();}
    return OwnedOperatorPipeConnection(std::move(state));
}
AuthorizationResult WindowsOperatorAuthorizationProvider::receive_authorized_pipe_message(
    OwnedOperatorPipeConnection& connection,const ManagedRoleBindings& bindings) {
    if(!connection.state_ || connection.state_->invalid.load())
        return denied(AuthorizationStatus::connection_invalid);
    auto& pipe=*connection.state_;
    if(pipe.busy.test_and_set())return denied(AuthorizationStatus::connection_busy);
    struct Release {std::atomic_flag& flag;~Release(){flag.clear();}} lease{pipe.busy};
    if(pipe.invalid.load() || pipe.api.get()!=api_.get())
        return denied(AuthorizationStatus::connection_invalid);
    try {
        const auto entry=api_->entry_thread_context();
        if(entry!=ERROR_NO_TOKEN)return denied(entry==ERROR_SUCCESS?
            AuthorizationStatus::outer_impersonation_present:
            AuthorizationStatus::entry_context_unavailable,entry);
        auto message=api_->read_message(pipe.handle,kMaximumMessage);
        if(!message.ok || message.value.empty() || message.value.size()>kMaximumMessage){
            pipe.invalidate();return denied(AuthorizationStatus::message_invalid,message.error);
        }
        const auto captured_at=api_->now();
        auto impersonation=api_->impersonate_pipe(pipe.handle);
        if(!impersonation.ok || !impersonation.value)
            return denied(AuthorizationStatus::impersonation_failed,impersonation.error);
        ImpersonationGuard impersonation_guard{*api_};
        auto original=api_->open_thread_token(TOKEN_QUERY|TOKEN_DUPLICATE);
        if(!original.ok || !handle_valid(original.value)){
            impersonation_guard.finish();
            return denied(AuthorizationStatus::identity_unavailable,original.error,false,true);
        }
        TokenOwner original_owner(api_,original.value);
        auto original_facts=api_->token_facts(original.value);
        if(!original_facts.ok || !supported(original_facts.value)){
            impersonation_guard.finish();
            return denied(AuthorizationStatus::unsupported_token,original_facts.error,true,true);
        }
        auto duplicate=api_->duplicate_query_token(original.value,TOKEN_QUERY);
        if(!duplicate.ok || !handle_valid(duplicate.value)){
            impersonation_guard.finish();
            return denied(AuthorizationStatus::identity_unavailable,duplicate.error,true,true);
        }
        TokenOwner duplicate_owner(api_,duplicate.value);
        impersonation_guard.finish();
        api_->close_handle(std::exchange(original_owner.handle,nullptr));
        auto state=std::make_unique<AuthorizedPipeMessage::State>();
        state->token.api=api_;
        state->token.handle=std::exchange(duplicate_owner.handle,nullptr);
        state->initial=std::move(original_facts.value);
        state->captured_at=captured_at;
        state->bindings=bindings;
        state->bytes=std::move(message.value);
        state->origin=origin_;state->native=native_;
        auto result=validate(*state,bindings);
        result.token_captured=true;result.reverted=true;
        if(result.authorized()){
            state->granted=result.role_grants;
            AuthorizedPipeMessage evidence(std::move(state));
            result.evidence=std::make_unique<AuthorizedPipeMessage>(std::move(evidence));
            const auto returned_at=api_->now();
            if(returned_at<captured_at || returned_at-captured_at>=kEvidenceLifetime)
                return denied(AuthorizationStatus::evidence_expired,0,true,true);
        }
        return result;
    } catch(...) {
        return denied(AuthorizationStatus::identity_unavailable,ERROR_INVALID_DATA);
    }
}
AuthorizationResult WindowsOperatorAuthorizationProvider::validate(
    const AuthorizedPipeMessage::State& evidence,const ManagedRoleBindings& bindings) {
    auto expired=[&] {
        const auto now=api_->now();
        return now<evidence.captured_at || now-evidence.captured_at>=kEvidenceLifetime;
    };
    if(expired())return denied(AuthorizationStatus::evidence_expired);
    if(bindings.generation!=evidence.bindings.generation)
        return denied(AuthorizationStatus::generation_changed);
    if(bindings!=evidence.bindings)return denied(AuthorizationStatus::binding_changed);
    auto facts=api_->token_facts(evidence.token.handle);
    if(!facts.ok || !supported(facts.value) || !same_logon(evidence.initial,facts.value))
        return denied(AuthorizationStatus::unsupported_token,facts.error);
    if(facts.value.session_id==0)return denied(AuthorizationStatus::session_inactive);
    auto session=api_->session_facts(facts.value.session_id);
    if(!session.ok || !session.value.known)
        return denied(AuthorizationStatus::session_unavailable,session.error);
    if(!session.value.active)return denied(AuthorizationStatus::session_inactive);
    if(!session.value.unlocked)return denied(AuthorizationStatus::session_locked);
    if(!bindings.teacher && !bindings.technician)
        return denied(expired()?AuthorizationStatus::evidence_expired:AuthorizationStatus::unprovisioned);
    auto machine=api_->local_machine_name();
    if(!machine.ok || !simple_name(machine.value))
        return denied(AuthorizationStatus::invalid_binding,machine.error);
    std::array<const std::optional<LocalRoleBinding>*,2> roles{&bindings.teacher,&bindings.technician};
    std::array<std::wstring,2> aliases;
    for(std::size_t index=0;index<roles.size();++index){
        if(!*roles[index])continue;
        const auto& binding=**roles[index];
        const auto split=binding.qualified_local_alias.find(L'\\');
        if(!valid_sid(binding.expected_sid) || split==std::wstring::npos ||
           !name_equal(binding.qualified_local_alias.substr(0,split),machine.value))
            return denied(AuthorizationStatus::invalid_binding);
        aliases[index]=binding.qualified_local_alias.substr(split+1);
        if(!simple_name(aliases[index]))return denied(AuthorizationStatus::invalid_binding);
        auto account=api_->account_from_name(binding.qualified_local_alias);
        if(!account.ok || account.value.type!=SidTypeAlias ||
           !name_equal(account.value.domain,machine.value) ||
           !name_equal(account.value.name,aliases[index]) ||
           !equal_sid(account.value.sid,binding.expected_sid))
            return denied(AuthorizationStatus::binding_changed,account.error);
    }
    if(bindings.teacher && bindings.technician &&
       (equal_sid(bindings.teacher->expected_sid,bindings.technician->expected_sid) ||
        name_equal(aliases[0],aliases[1])))return denied(AuthorizationStatus::invalid_binding);
    auto principal=api_->account_from_sid(facts.value.user);
    if(!principal.ok || principal.value.type!=SidTypeUser ||
       !simple_name(principal.value.name) || !simple_name(principal.value.domain) ||
       !equal_sid(principal.value.sid,facts.value.user))
        return denied(AuthorizationStatus::identity_unavailable,principal.error);
    const auto qualified=principal.value.domain+L"\\"+principal.value.name;
    auto check_principal=[&] {
        auto mapped=api_->account_from_name(qualified);
        return mapped.ok && mapped.value.type==SidTypeUser &&
            equal_sid(mapped.value.sid,facts.value.user) &&
            name_equal(mapped.value.name,principal.value.name) &&
            name_equal(mapped.value.domain,principal.value.domain);
    };
    if(!check_principal())return denied(AuthorizationStatus::identity_unavailable);
    auto groups=api_->direct_local_groups(qualified,0,0,kPreferredGroupBytes);
    if(groups.status!=NERR_Success)return denied(
        groups.status==ERROR_MORE_DATA?AuthorizationStatus::membership_incomplete:
        AuthorizationStatus::membership_unavailable,groups.status);
    if(!groups.structure_valid || groups.entries!=groups.total ||
       groups.entries!=groups.aliases.size() || groups.entries>kMaximumGroups ||
       groups.buffer_bytes>kMaximumBuffer)
        return denied(AuthorizationStatus::membership_incomplete);
    for(std::size_t i=0;i<groups.aliases.size();++i){
        if(!simple_name(groups.aliases[i]))return denied(AuthorizationStatus::membership_incomplete);
        for(std::size_t j=0;j<i;++j)
            if(name_equal(groups.aliases[i],groups.aliases[j]))
                return denied(AuthorizationStatus::membership_incomplete);
    }
    auto principal_after=api_->account_from_sid(facts.value.user);
    if(!principal_after.ok || principal_after.value.type!=SidTypeUser ||
       !equal_sid(principal_after.value.sid,facts.value.user) ||
       !name_equal(principal_after.value.name,principal.value.name) ||
       !name_equal(principal_after.value.domain,principal.value.domain) || !check_principal())
        return denied(AuthorizationStatus::identity_unavailable);
    RoleGrants granted;
    for(std::size_t index=0;index<roles.size();++index){
        if(!*roles[index])continue;
        const auto& binding=**roles[index];
        auto account=api_->account_from_name(binding.qualified_local_alias);
        if(!account.ok || account.value.type!=SidTypeAlias ||
           !name_equal(account.value.domain,machine.value) ||
           !name_equal(account.value.name,aliases[index]) ||
           !equal_sid(account.value.sid,binding.expected_sid))
            return denied(AuthorizationStatus::binding_changed,account.error);
        const bool direct=std::any_of(groups.aliases.begin(),groups.aliases.end(),
            [&](const auto& alias){return name_equal(alias,aliases[index]);});
        const bool allowed=direct && enabled_group(facts.value,binding.expected_sid);
        if(index==0)granted.teacher=allowed;else granted.technician=allowed;
    }
    // Membership lookup may have overlapped a session lock/disconnect.
    // Recheck local WTS state at acceptance, then check final evidence age.
    auto final_session=api_->session_facts(facts.value.session_id);
    if(!final_session.ok || !final_session.value.known)
        return denied(AuthorizationStatus::session_unavailable,final_session.error);
    if(!final_session.value.active)return denied(AuthorizationStatus::session_inactive);
    if(!final_session.value.unlocked)return denied(AuthorizationStatus::session_locked);
    // This final check is deliberately after every possibly blocking query.
    if(expired())return denied(AuthorizationStatus::evidence_expired);
    if(bindings.generation!=evidence.bindings.generation)
        return denied(AuthorizationStatus::generation_changed);
    if(!granted.teacher && !granted.technician)
        return denied(AuthorizationStatus::not_direct_member);
    AuthorizationResult result;
    result.status=AuthorizationStatus::authorized;
    result.role_grants=granted;
    return result;
}
AuthorizationResult WindowsOperatorAuthorizationProvider::revalidate_for_dispatch(
    const AuthorizedPipeMessage& evidence,const ManagedRoleBindings& bindings) {
    if(!evidence.state_ || evidence.state_->origin!=origin_ ||
       evidence.state_->native!=native_ || evidence.state_->token.api.get()!=api_.get())
        return denied(AuthorizationStatus::foreign_evidence);
    try {
        const auto entry=api_->entry_thread_context();
        if(entry!=ERROR_NO_TOKEN)return denied(entry==ERROR_SUCCESS?
            AuthorizationStatus::outer_impersonation_present:
            AuthorizationStatus::entry_context_unavailable,entry);
        return validate(*evidence.state_,bindings);
    } catch(...) {return denied(AuthorizationStatus::identity_unavailable,ERROR_INVALID_DATA);}
}
OwnProcessObservation WindowsOperatorAuthorizationProvider::observe_own_process_read_only() {
    OwnProcessObservation result;
    try {
        auto facts=api_->own_process_facts();
        result.token_query_succeeded=facts.ok;
        if(!facts.ok)return result;
        result.restricted=facts.value.restricted;
        for(const auto& group:facts.value.groups){
            if((group.attributes&SE_GROUP_ENABLED)!=0)++result.enabled_group_count;
            if((group.attributes&SE_GROUP_USE_FOR_DENY_ONLY)!=0)++result.deny_only_group_count;
        }
        auto session=api_->session_facts(facts.value.session_id);
        result.session_query_succeeded=session.ok && session.value.known;
        if(result.session_query_succeeded){
            result.session_active=session.value.active;
            result.session_unlocked=session.value.unlocked;
        }
        auto principal=api_->account_from_sid(facts.value.user);
        if(principal.ok && principal.value.type==SidTypeUser &&
           simple_name(principal.value.domain) && simple_name(principal.value.name)){
            const auto qualified=principal.value.domain+L"\\"+principal.value.name;
            auto roundtrip=api_->account_from_name(qualified);
            if(roundtrip.ok && roundtrip.value.type==SidTypeUser &&
               equal_sid(roundtrip.value.sid,facts.value.user)){
                auto groups=api_->direct_local_groups(qualified,0,0,kPreferredGroupBytes);
                result.direct_local_query_succeeded=groups.status==NERR_Success &&
                    groups.structure_valid && groups.entries==groups.total &&
                    groups.entries==groups.aliases.size() && groups.entries<=kMaximumGroups &&
                    groups.buffer_bytes<=kMaximumBuffer;
                if(result.direct_local_query_succeeded)result.direct_local_group_count=groups.entries;
            }
        }
    } catch(...) {}
    return result; // Managed grants are never populated by this observation.
}
}
