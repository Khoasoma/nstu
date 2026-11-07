#include "nstu/windows_operator_authorization.hpp"
#include <sddl.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <atomic>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <random>
#include <set>
#include <stdexcept>
#include <thread>
#include <type_traits>
static_assert(!std::is_copy_constructible_v<nstu::server::authority::WindowsOperatorAuthorizationProvider>);
static_assert(!std::is_copy_constructible_v<nstu::server::authority::AuthorizedPipeMessage>);
static_assert(!std::is_copy_constructible_v<nstu::server::authority::OwnedOperatorPipeConnection>);

using namespace nstu::server::authority;
namespace {
std::string current_case;
void check(bool value, int line) { if (!value) { std::cerr << "Check line=" << line << "; native_error=" << GetLastError() << "\n"; throw std::runtime_error(current_case); } }
#define CHECK(value) check(static_cast<bool>(value), __LINE__)
template<class T> ApiResult<T> good(T value) { return {true, 0, std::move(value)}; }
template<class T> ApiResult<T> bad(DWORD error = ERROR_ACCESS_DENIED) { return {false, error, {}}; }
SidBytes sid(const wchar_t* text) {
    PSID raw = nullptr;
    CHECK(ConvertStringSidToSidW(text, &raw));
    const auto* first = reinterpret_cast<const std::byte*>(raw);
    SidBytes result(first, first + GetLengthSid(raw));
    LocalFree(raw);
    return result;
}
HANDLE fake_handle(std::uintptr_t number) { return reinterpret_cast<HANDLE>(number); }

struct FakeApi final : WindowsAuthorizationApi {
    TokenFacts facts;
    ManagedRoleBindings bindings;
    AccountFacts user;
    AccountFacts teacher;
    AccountFacts technician;
    DirectLocalGroups groups{0, 2, 2, 256, true, {L"Teachers", L"Technicians"}};
    SessionFacts session{true, true, true};
    Clock::time_point tick{std::chrono::seconds(1)};
    DWORD entry{ERROR_NO_TOKEN};
    bool pipe_valid{true}, impersonation_ok{true}, open_ok{true}, duplicate_ok{true};
    bool query_ok{true}, revert_ok{true}, machine_ok{true}, sid_lookup_ok{true};
    bool name_lookup_ok{true}, session_ok{true}, entered{false};
    DWORD read_error{0};
    int reads{0}, reverts{0}, queries{0}, direct_calls{0}, lookups{0}, sid_lookups{0};
    DWORD opened_access{0}, duplicated_access{0}, direct_level{99}, direct_flags{99};
    DWORD preferred{0};
    int delay_after{0};
    bool throw_query{false}, change_user_after{false}, change_alias_after{false};
    bool delay_original_close{false};
    std::function<void()> during_read;
    std::function<void()> during_membership;
    std::set<HANDLE> held;
    FakeApi() {
        facts.user = sid(L"S-1-5-21-1-2-3-1001");
        teacher = {sid(L"S-1-5-21-1-2-3-1002"), L"Teachers", L"LAB", SidTypeAlias};
        technician = {sid(L"S-1-5-21-1-2-3-1003"), L"Technicians", L"LAB", SidTypeAlias};
        facts.groups = {{teacher.sid, SE_GROUP_ENABLED}, {technician.sid, SE_GROUP_ENABLED}};
        facts.authentication_id.LowPart = 42;
        facts.session_id = 1;
        facts.type = TokenImpersonation;
        facts.level = SecurityImpersonation;
        user = {facts.user, L"Operator", L"LAB", SidTypeUser};
        bindings = {{LocalRoleBinding{L"LAB\\Teachers", teacher.sid}},
                    {LocalRoleBinding{L"LAB\\Technicians", technician.sid}}, 7};
    }
    DWORD entry_thread_context() override { return entry; }
    ApiResult<bool> inspect_local_server_pipe(HANDLE) override {
        return pipe_valid ? good(true) : bad<bool>();
    }
    ApiResult<std::vector<std::byte>> read_message(HANDLE, DWORD maximum) override {
        CHECK(maximum == 4096);
        ++reads;
        if (during_read) during_read();
        if (read_error) return bad<std::vector<std::byte>>(read_error);
        return good(std::vector<std::byte>{std::byte{1}});
    }
    ApiResult<bool> impersonate_pipe(HANDLE) override {
        if (!impersonation_ok) return bad<bool>();
        entered = true;
        return good(true);
    }
    ApiResult<HANDLE> open_thread_token(DWORD access) override {
        opened_access = access;
        if (!open_ok) return bad<HANDLE>();
        held.insert(fake_handle(2));
        return good(fake_handle(2));
    }
    ApiResult<HANDLE> duplicate_query_token(HANDLE, DWORD access) override {
        duplicated_access = access;
        if (!duplicate_ok) return bad<HANDLE>();
        held.insert(fake_handle(3));
        return good(fake_handle(3));
    }
    ApiResult<TokenFacts> token_facts(HANDLE) override {
        ++queries;
        if (throw_query) throw std::runtime_error("injected");
        return query_ok ? good(facts) : bad<TokenFacts>();
    }
    ApiResult<TokenFacts> own_process_facts() override { return good(facts); }
    bool revert_to_self() noexcept override { ++reverts; entered = false; return revert_ok; }
    void close_handle(HANDLE handle) noexcept override { if(delay_original_close && handle==fake_handle(2)) tick+=std::chrono::milliseconds(300); held.erase(handle); }
    void disconnect_pipe(HANDLE) noexcept override {}
    ApiResult<std::wstring> local_machine_name() override {
        return machine_ok ? good(std::wstring(L"LAB")) : bad<std::wstring>();
    }
    ApiResult<AccountFacts> account_from_sid(std::span<const std::byte>) override {
        ++sid_lookups;
        if (!sid_lookup_ok) return bad<AccountFacts>();
        auto result = user;
        if (change_user_after && sid_lookups > 1) result.sid = technician.sid;
        return good(result);
    }
    ApiResult<AccountFacts> account_from_name(const std::wstring& name) override {
        ++lookups;
        if (!name_lookup_ok) return bad<AccountFacts>();
        if (name == user.domain + L"\\" + user.name) return good(user);
        if (name == L"LAB\\Teachers") {
            auto result = teacher;
            if (change_alias_after && direct_calls) result.sid = technician.sid;
            return good(result);
        }
        if (name == L"LAB\\Technicians") return good(technician);
        return bad<AccountFacts>(ERROR_NONE_MAPPED);
    }
    DirectLocalGroups direct_local_groups(const std::wstring& name, DWORD level,
                                          DWORD flags, DWORD bytes) override {
        CHECK(name == user.domain + L"\\" + user.name);
        ++direct_calls; direct_level = level; direct_flags = flags; preferred = bytes;
        if(during_membership)during_membership();
        if (delay_after == 1) tick += std::chrono::milliseconds(300);
        return groups;
    }
    ApiResult<SessionFacts> session_facts(DWORD id) override {
        CHECK(id == facts.session_id);
        if (delay_after == 2) tick += std::chrono::milliseconds(300);
        return session_ok ? good(session) : bad<SessionFacts>();
    }
    Clock::time_point now() noexcept override { return tick; }
};
struct Fixture {
    std::shared_ptr<FakeApi> api = std::make_shared<FakeApi>();
    WindowsOperatorAuthorizationProvider provider{api};
    OwnedOperatorPipeConnection pipe;
    Fixture() {
        HANDLE handle = fake_handle(1);
        api->held.insert(handle);
        pipe = provider.adopt_connected_operator_pipe(handle);
        CHECK(handle == INVALID_HANDLE_VALUE);
        CHECK(pipe.valid());
    }
    AuthorizationResult receive() {
        return provider.receive_authorized_pipe_message(pipe, api->bindings);
    }
};
int cases = 0;
template<class Function> void run(const char* name, Function body) {
    current_case = name;
    body();
    ++cases;
}
void no_grant(const AuthorizationResult& result) {
    CHECK(!result.authorized()); CHECK(!result.evidence);
}
void unit_tests() {
    run("direct_user_and_dual_roles_injected", [] {
        Fixture f; auto r=f.receive(); CHECK(r.authorized());
        CHECK(r.evidence->grants().teacher && r.evidence->grants().technician);
        CHECK(f.api->direct_level==0 && f.api->direct_flags==0 && f.api->preferred==32768);
        CHECK(f.api->opened_access==(TOKEN_QUERY|TOKEN_DUPLICATE));
        CHECK(f.api->duplicated_access==TOKEN_QUERY);
        CHECK(!f.api->entered && f.api->reverts==1);
    });
    run("domain_user_direct_membership_injected", [] {
        Fixture f; f.api->user.domain=L"DOMAIN"; CHECK(f.receive().authorized());
    });
    run("absent_bindings_no_grant", [] {
        Fixture f; auto r=f.provider.receive_authorized_pipe_message(f.pipe,{});
        no_grant(r); CHECK(r.status==AuthorizationStatus::unprovisioned);
    });
    run("outer_context_and_unknown_entry_preserved", [] {
        for (DWORD entry : {DWORD{ERROR_SUCCESS}, DWORD{ERROR_ACCESS_DENIED}}) {
            Fixture f; f.api->entry=entry; no_grant(f.receive());
            CHECK(f.api->reads==0 && f.api->reverts==0);
        }
    });
    run("impersonation_and_token_failures_cleanup", [] {
        for(int mode=0;mode<4;++mode) {
            Fixture f;
            if(mode==0)f.api->impersonation_ok=false;
            if(mode==1)f.api->open_ok=false;
            if(mode==2)f.api->duplicate_ok=false;
            if(mode==3)f.api->query_ok=false;
            no_grant(f.receive()); CHECK(!f.api->entered);
            CHECK(!f.api->held.contains(fake_handle(2)) && !f.api->held.contains(fake_handle(3)));
        }
    });
    run("query_exception_reverts_and_closes", [] {
        Fixture f; f.api->throw_query=true; no_grant(f.receive());
        CHECK(!f.api->entered && f.api->reverts==1);
        CHECK(!f.api->held.contains(fake_handle(2)) && !f.api->held.contains(fake_handle(3)));
    });
    run("disabled_deny_only_and_missing_token_group", [] {
        for(DWORD attributes : {DWORD{0}, DWORD{SE_GROUP_USE_FOR_DENY_ONLY}, DWORD{SE_GROUP_USE_FOR_DENY_ONLY|SE_GROUP_ENABLED}}) {
            Fixture f; for(auto& g:f.api->facts.groups)g.attributes=attributes;
            no_grant(f.receive());
        }
        Fixture f; f.api->facts.groups.clear(); no_grant(f.receive());
    });
    run("unsupported_token_shapes", [] {
        for(int mode=0;mode<6;++mode) {
            Fixture f;
            if(mode==0)f.api->facts.anonymous=true;
            if(mode==1)f.api->facts.restricted=true;
            if(mode==2)f.api->facts.app_container=true;
            if(mode==3)f.api->facts.level=SecurityIdentification;
            if(mode==4)f.api->facts.level=SecurityAnonymous;
            if(mode==5)f.api->facts.type=TokenPrimary;
            no_grant(f.receive());
        }
    });
    run("binding_sid_type_domain_and_recreation", [] {
        for(int mode=0;mode<5;++mode) {
            Fixture f; f.api->bindings.technician.reset();
            if(mode==0)f.api->bindings.teacher->expected_sid={std::byte{1}};
            if(mode==1)f.api->teacher.type=SidTypeUser;
            if(mode==2)f.api->teacher.domain=L"DOMAIN";
            if(mode==3)f.api->bindings.teacher->qualified_local_alias=L"Teachers";
            if(mode==4)f.api->change_alias_after=true;
            no_grant(f.receive());
        }
    });
    run("user_roundtrip_and_query_errors", [] {
        for(int mode=0;mode<4;++mode) {
            Fixture f;
            if(mode==0)f.api->user.sid=f.api->teacher.sid;
            if(mode==1)f.api->user.type=SidTypeGroup;
            if(mode==2)f.api->sid_lookup_ok=false;
            if(mode==3)f.api->change_user_after=true;
            no_grant(f.receive());
        }
    });
    run("complete_nonresumable_query_required", [] {
        for(int mode=0;mode<7;++mode) {
            Fixture f;
            if(mode==0)f.api->groups.status=ERROR_MORE_DATA;
            if(mode==1)f.api->groups.status=ERROR_ACCESS_DENIED;
            if(mode==2)f.api->groups.total=3;
            if(mode==3)f.api->groups.structure_valid=false;
            if(mode==4)f.api->groups.buffer_bytes=1048577;
            if(mode==5)f.api->groups.entries=4097;
            if(mode==6)f.api->groups.aliases[0]=std::wstring(257,L'x');
            no_grant(f.receive()); CHECK(f.api->direct_calls==1);
        }
    });
    run("nested_only_membership_no_direct_grant", [] {
        Fixture f; f.api->groups={0,0,0,0,true,{}}; no_grant(f.receive());
    });
    run("session_zero_unknown_locked_inactive_and_error", [] {
        for(int mode=0;mode<5;++mode) {
            Fixture f;
            if(mode==0)f.api->facts.session_id=0;
            if(mode==1)f.api->session.known=false;
            if(mode==2)f.api->session.unlocked=false;
            if(mode==3)f.api->session.active=false;
            if(mode==4)f.api->session_ok=false;
            no_grant(f.receive());
        }
    });
    run("oversize_invalidates_tail", [] {
        Fixture f; f.api->read_error=ERROR_MORE_DATA;
        no_grant(f.receive()); CHECK(!f.pipe.valid());
        f.api->read_error=0; no_grant(f.receive()); CHECK(f.api->reads==1);
    });
    run("exclusive_same_endpoint_across_providers", [] {
        Fixture f; WindowsOperatorAuthorizationProvider other(f.api);
        f.api->during_read=[&] {
            auto other_result=other.receive_authorized_pipe_message(f.pipe,f.api->bindings);
            CHECK(other_result.status==AuthorizationStatus::connection_busy);
            no_grant(other_result);
        };
        CHECK(f.receive().authorized()); CHECK(f.api->reads==1);
    });
    run("concurrent_reader_lease", [] {
        Fixture f; WindowsOperatorAuthorizationProvider other(f.api);
        f.api->during_read=[&] {
            bool denied=false;
            std::thread competing([&] { auto r=other.receive_authorized_pipe_message(f.pipe,f.api->bindings); denied=r.status==AuthorizationStatus::connection_busy && !r.authorized(); });
            competing.join(); CHECK(denied);
        };
        CHECK(f.receive().authorized()); CHECK(f.api->reads==1);
    });
    run("session_locked_during_membership_denied", [] {
        Fixture f; f.api->during_membership=[&] {f.api->session.unlocked=false;};
        auto r=f.receive(); no_grant(r); CHECK(r.status==AuthorizationStatus::session_locked);
    });
    run("final_expiry_after_native_query", [] {
        for(int delay : {1,2}) {
            Fixture f; f.api->delay_after=delay;
            auto r=f.receive(); no_grant(r);
            CHECK(r.status==AuthorizationStatus::evidence_expired);
        }
    });
    run("original_token_cleanup_precedes_final_age_check", [] {
        Fixture f; f.api->delay_original_close=true; auto r=f.receive();
        no_grant(r); CHECK(r.status==AuthorizationStatus::evidence_expired);
    });
    run("revalidation_removal_generation_foreign_and_expiry", [] {
        Fixture f; auto r=f.receive(); CHECK(r.authorized());
        WindowsOperatorAuthorizationProvider foreign(f.api);
        no_grant(foreign.revalidate_for_dispatch(*r.evidence,f.api->bindings));
        WindowsOperatorAuthorizationProvider native;
        no_grant(native.revalidate_for_dispatch(*r.evidence,f.api->bindings));
        auto changed=f.api->bindings; ++changed.generation;
        no_grant(f.provider.revalidate_for_dispatch(*r.evidence,changed));
        f.api->groups={0,0,0,0,true,{}};
        no_grant(f.provider.revalidate_for_dispatch(*r.evidence,f.api->bindings));
        f.api->groups={0,2,2,256,true,{L"Teachers",L"Technicians"}};
        f.api->delay_after=1;
        auto delayed=f.provider.revalidate_for_dispatch(*r.evidence,f.api->bindings);
        no_grant(delayed); CHECK(delayed.status==AuthorizationStatus::evidence_expired);
        f.api->delay_after=0;
        no_grant(f.provider.revalidate_for_dispatch(*r.evidence,f.api->bindings));
    });
    run("evidence_and_backend_storage_lifetime", [] {
        auto api=std::make_shared<FakeApi>(); std::weak_ptr<FakeApi> weak=api;
        std::unique_ptr<AuthorizedPipeMessage> evidence;
        {
            WindowsOperatorAuthorizationProvider provider(api);
            HANDLE handle=fake_handle(1); api->held.insert(handle);
            auto pipe=provider.adopt_connected_operator_pipe(handle);
            auto r=provider.receive_authorized_pipe_message(pipe,api->bindings);
            CHECK(r.authorized()); evidence=std::move(r.evidence);
            api.reset(); CHECK(!weak.expired());
        }
        CHECK(!weak.expired()); evidence.reset(); CHECK(weak.expired());
    });
    std::cout<<"Injected provider cases passed: "<<cases
             <<"; synthetic grants only; real configured roles not established.\n";
}
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~Handle(){if(value!=INVALID_HANDLE_VALUE && value!=nullptr)CloseHandle(value);}
    Handle()=default;
    Handle(const Handle&)=delete;
    Handle& operator=(const Handle&)=delete;
};
std::wstring own_sid_text() {
    Handle token; CHECK(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token.value));
    DWORD size=0;
    GetTokenInformation(token.value,TokenUser,nullptr,0,&size);
    std::vector<std::byte> buffer(size);
    CHECK(GetTokenInformation(token.value,TokenUser,buffer.data(),size,&size));
    LPWSTR text=nullptr;
    CHECK(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid,&text));
    std::wstring result(text); LocalFree(text); return result;
}
struct NativePipe {
    Handle server,client;
    NativePipe() {
        std::random_device random;
        const auto name=L"\\\\.\\pipe\\nstu-auth01-"+std::to_wstring(GetCurrentProcessId())+
            L"-"+std::to_wstring(random())+L"-"+std::to_wstring(random());
        const auto descriptor_text=L"D:P(A;;0x0012019B;;;"+own_sid_text()+L")(A;;GA;;;SY)";
        PSECURITY_DESCRIPTOR descriptor=nullptr;
        CHECK(ConvertStringSecurityDescriptorToSecurityDescriptorW(
            descriptor_text.c_str(),SDDL_REVISION_1,&descriptor,nullptr));
        SECURITY_ATTRIBUTES attributes{sizeof(attributes),descriptor,FALSE};
        server.value=CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,
            1,8192,8192,0,&attributes);
        LocalFree(descriptor); CHECK(server.value!=INVALID_HANDLE_VALUE);
        client.value=CreateFileW(name.c_str(),FILE_READ_DATA|FILE_WRITE_DATA|SYNCHRONIZE,
            0,nullptr,OPEN_EXISTING,SECURITY_SQOS_PRESENT|SECURITY_IMPERSONATION,nullptr);
        CHECK(client.value!=INVALID_HANDLE_VALUE);
        CHECK(ConnectNamedPipe(server.value,nullptr)||GetLastError()==ERROR_PIPE_CONNECTED);
    }
    void send(std::size_t count) {
        std::vector<std::byte> bytes(count,std::byte{1}); DWORD written=0;
        CHECK(WriteFile(client.value,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr));
        CHECK(written==bytes.size());
    }
};
void native_tests() {
    WindowsOperatorAuthorizationProvider provider;
    const auto observation=provider.observe_own_process_read_only();
    CHECK(!observation.managed_grants.teacher && !observation.managed_grants.technician);
    CHECK(observation.token_query_succeeded);
    std::cout<<"Native own-process observation: token_query=ok; enabled_groups="
             <<observation.enabled_group_count<<"; deny_only_groups="
             <<observation.deny_only_group_count<<"; session_query="
             <<(observation.session_query_succeeded?"ok":"unavailable")
             <<"; direct_local_query="<<(observation.direct_local_query_succeeded?"ok":"unavailable")
             <<"; observed_direct_groups="<<observation.direct_local_group_count
             <<"; managed_config=absent; managed_grants=not-established; installed_broker=not-tested.\n";
    current_case="native_self_pipe_no_configured_role";
    NativePipe fixture; fixture.send(1);
    auto pipe=provider.adopt_connected_operator_pipe(fixture.server.value); CHECK(pipe.valid());
    auto result=provider.receive_authorized_pipe_message(pipe,{});
    no_grant(result); CHECK(result.token_captured && result.reverted);
    std::cout<<"Native no-role pipe result status="<<static_cast<int>(result.status)<<"; captured=1; reverted=1; no retained authority claimed.\n";
    Handle thread_token;
    CHECK(!OpenThreadToken(GetCurrentThread(),TOKEN_QUERY,TRUE,&thread_token.value));
    CHECK(GetLastError()==ERROR_NO_TOKEN);
    current_case="native_oversize_tail_invalidated";
    NativePipe oversize; oversize.send(5000);
    auto oversized=provider.adopt_connected_operator_pipe(oversize.server.value);
    auto rejected=provider.receive_authorized_pipe_message(oversized,{});
    no_grant(rejected); CHECK(!oversized.valid());
    no_grant(provider.receive_authorized_pipe_message(oversized,{}));
    std::cout<<"Native owned pipe capture/revert and oversize invalidation verified; zero managed role grants.\n";
}

struct DerivedTokenFixture {
    Handle base, filtered, impersonation;
    DerivedTokenFixture() {
        CHECK(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY|TOKEN_DUPLICATE,&base.value));
        CHECK(CreateRestrictedToken(base.value,DISABLE_MAX_PRIVILEGE,0,nullptr,0,nullptr,
                                    0,nullptr,&filtered.value));
        CHECK(DuplicateTokenEx(filtered.value,TOKEN_QUERY,nullptr,SecurityImpersonation,
                               TokenImpersonation,&impersonation.value));
    }
};
void native_owned_restrictions() {
    current_case="derived_privilege_filtered_without_restricting_sids";
    DerivedTokenFixture fixture;
    alignas(DWORD) std::array<std::byte,sizeof(DWORD)> field{}; DWORD returned=0;
    CHECK(GetTokenInformation(fixture.impersonation.value,TokenHasRestrictions,
                              field.data(),static_cast<DWORD>(field.size()),&returned));
    CHECK(returned==sizeof(BYTE) || returned==sizeof(DWORD));
    DWORD filtered=0;
    if(returned==sizeof(BYTE))filtered=std::to_integer<BYTE>(field[0]);
    else std::memcpy(&filtered,field.data(),sizeof(filtered));
    CHECK(filtered!=0);
    std::cout<<"Owned native restriction returned_bytes="<<returned<<"; nonzero=1; no token application.\n";
    TOKEN_TYPE type=TokenPrimary;
    CHECK(GetTokenInformation(fixture.impersonation.value,TokenType,&type,sizeof(type),&returned));
    CHECK(type==TokenImpersonation);
    SetLastError(ERROR_SUCCESS);
    CHECK(!IsTokenRestricted(fixture.impersonation.value));
    CHECK(GetLastError()==ERROR_SUCCESS);
    DWORD needed=0;
    CHECK(!GetTokenInformation(fixture.impersonation.value,TokenRestrictedSids,nullptr,0,&needed));
    CHECK(GetLastError()==ERROR_INSUFFICIENT_BUFFER);
    std::vector<std::byte> buffer(needed);
    CHECK(GetTokenInformation(fixture.impersonation.value,TokenRestrictedSids,
                              buffer.data(),needed,&needed));
    CHECK(reinterpret_cast<TOKEN_GROUPS*>(buffer.data())->GroupCount==0);
    const auto observed=observe_owned_token_classification_read_only(fixture.impersonation.value);
    std::cout<<"Native owned-derived observation: independent_has_restrictions=1; "
             <<"restricting_sids=0; adapter_query="<<observed.query_succeeded
             <<"; adapter_error="<<observed.native_error
             <<"; adapter_ever_filtered="<<observed.ever_filtered
             <<"; supported_shape="<<observed.supported_shape
             <<"; managed_grants=not-tested; token_application=none.\n";
    CHECK(observed.query_succeeded && observed.ever_filtered && !observed.supported_shape);
}
int restriction_query_calls=0;
BOOL WINAPI denied_restriction_query(HANDLE token,TOKEN_INFORMATION_CLASS kind,
                                     LPVOID data,DWORD bytes,PDWORD returned) {
    if(kind==TokenHasRestrictions){
        ++restriction_query_calls;
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return GetTokenInformation(token,kind,data,bytes,returned);
}
BOOL WINAPI short_restriction_query(HANDLE token,TOKEN_INFORMATION_CLASS kind,
                                    LPVOID data,DWORD bytes,PDWORD returned) {
    if(kind==TokenHasRestrictions){
        ++restriction_query_calls;
        if(data && bytes>=sizeof(DWORD))*static_cast<DWORD*>(data)=0;
        if(returned)*returned=sizeof(DWORD)-1;
        SetLastError(ERROR_SUCCESS);
        return TRUE;
    }
    return GetTokenInformation(token,kind,data,bytes,returned);
}

DWORD injected_restriction_length=3;
BOOL WINAPI invalid_restriction_length(HANDLE token,TOKEN_INFORMATION_CLASS kind,
                                       LPVOID data,DWORD bytes,PDWORD returned) {
    if(kind==TokenHasRestrictions){
        ++restriction_query_calls;
        if(data && bytes>=sizeof(DWORD))std::memset(data,0,sizeof(DWORD));
        if(returned)*returned=injected_restriction_length;
        SetLastError(ERROR_SUCCESS);
        return TRUE;
    }
    return GetTokenInformation(token,kind,data,bytes,returned);
}
BOOL WINAPI documentary_dword_restriction(HANDLE token,TOKEN_INFORMATION_CLASS kind,
                                          LPVOID data,DWORD bytes,PDWORD returned) {
    if(kind==TokenHasRestrictions){
        ++restriction_query_calls;
        if(!data || bytes<sizeof(DWORD)){SetLastError(ERROR_INSUFFICIENT_BUFFER);return FALSE;}
        const DWORD filtered=1;
        std::memcpy(data,&filtered,sizeof(filtered));
        if(returned)*returned=sizeof(DWORD);
        SetLastError(ERROR_SUCCESS);
        return TRUE;
    }
    return GetTokenInformation(token,kind,data,bytes,returned);
}
BOOL WINAPI byte_prefix_restriction(HANDLE token,TOKEN_INFORMATION_CLASS kind,
                                    LPVOID data,DWORD bytes,PDWORD returned) {
    if(kind==TokenHasRestrictions){
        ++restriction_query_calls;
        if(!data || bytes<sizeof(DWORD)){SetLastError(ERROR_INSUFFICIENT_BUFFER);return FALSE;}
        std::memset(data,0x55,sizeof(DWORD)); // Unwritten tail must not be decoded.
        *static_cast<BYTE*>(data)=0;
        if(returned)*returned=sizeof(BYTE);
        SetLastError(ERROR_SUCCESS);
        return TRUE;
    }
    return GetTokenInformation(token,kind,data,bytes,returned);
}

void native_query_errors() {
    DerivedTokenFixture fixture;
    current_case="actual_native_hasrestrictions_access_denied";
    restriction_query_calls=0;
    const auto denied=observe_owned_token_classification_read_only(
        fixture.impersonation.value,denied_restriction_query);
    std::cout<<"Injected native-adapter query error: calls="<<restriction_query_calls
             <<"; query_succeeded="<<denied.query_succeeded
             <<"; error="<<denied.native_error<<"; supported_shape="<<denied.supported_shape
             <<"; managed_grants=not-tested.\n";
    CHECK(restriction_query_calls==1 && !denied.query_succeeded &&
          denied.native_error==ERROR_ACCESS_DENIED && !denied.supported_shape);
    current_case="actual_native_restriction_size_error";
    restriction_query_calls=0;
    const auto short_size=observe_owned_token_classification_read_only(
        fixture.impersonation.value,short_restriction_query);
    CHECK(restriction_query_calls==1 && !short_size.query_succeeded &&
          short_size.native_error==ERROR_INVALID_DATA && !short_size.supported_shape);
    for(DWORD length : {DWORD{0},DWORD{2},DWORD{5}}){
        injected_restriction_length=length; restriction_query_calls=0;
        const auto invalid=observe_owned_token_classification_read_only(fixture.impersonation.value,invalid_restriction_length);
        CHECK(restriction_query_calls==1 && !invalid.query_succeeded && invalid.native_error==ERROR_INVALID_DATA && !invalid.supported_shape);
    }
    current_case="documentary_dword4_injected_compatibility"; restriction_query_calls=0;
    const auto documentary=observe_owned_token_classification_read_only(fixture.impersonation.value,documentary_dword_restriction);
    CHECK(restriction_query_calls==1 && documentary.query_succeeded && documentary.ever_filtered && !documentary.supported_shape);
    current_case="validated_byte_prefix_ignores_unwritten_tail"; restriction_query_calls=0;
    const auto prefix=observe_owned_token_classification_read_only(fixture.impersonation.value,byte_prefix_restriction);
    CHECK(restriction_query_calls==1 && prefix.query_succeeded && !prefix.ever_filtered);
    std::cout<<"Injected documentaryDWORD4 and BYTE1 validated-prefix checks passed; no nativeDWORD4 or role grant claim.\n";
    const auto null_query=observe_owned_token_classification_read_only(
        fixture.impersonation.value,nullptr);
    CHECK(!null_query.query_succeeded && null_query.native_error==ERROR_INVALID_PARAMETER);
    std::cout<<"Injected native-adapter short-size/null query failures denied; no authority evidence.\n";
}

void fatal_child() {
    std::set_terminate([]{std::_Exit(86);});
    Fixture f; f.api->revert_ok=false;
    static_cast<void>(f.receive());
    std::_Exit(87);
}
void verify_fatal_child() {
    current_case="fatal_revert_child_nonreturn";
    std::vector<wchar_t> path(32768);
    const auto length=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
    CHECK(length>0 && length<path.size());
    auto command=L"\""+std::wstring(path.data(),length)+L"\" --fatal-revert-child";
    STARTUPINFOW startup{}; startup.cb=sizeof(startup);
    PROCESS_INFORMATION process{};
    CHECK(CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,
                         nullptr,nullptr,&startup,&process));
    Handle process_owner,thread_owner;
    process_owner.value=process.hProcess; thread_owner.value=process.hThread;
    const auto waited=WaitForSingleObject(process_owner.value,5000);
    if(waited!=WAIT_OBJECT_0)TerminateProcess(process_owner.value,88);
    CHECK(waited==WAIT_OBJECT_0);
    DWORD code=0; CHECK(GetExitCodeProcess(process_owner.value,&code)); CHECK(code==86);
}
}
int main(int argc,char** argv) {
    try {
        if(argc!=2)return 2;
        const std::string mode(argv[1]);
        if(mode=="--fatal-revert-child")fatal_child();
        else if(mode=="--unit"){unit_tests();verify_fatal_child();}
        else if(mode=="--native-read-only")native_tests();
        else if(mode=="--native-owned-restrictions")native_owned_restrictions();
        else if(mode=="--native-query-errors")native_query_errors();
        else return 2;
        return 0;
    } catch(const std::exception&) {
        std::cerr<<"AUTH01 qualification failed: "<<current_case<<"\n";
        return 1;
    }
}
