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
#include <map>
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
    bool kernel_allow{true},kernel_ok{true};
    std::optional<bool> teacher_kernel,technician_kernel;
    int kernel_calls{0};
    std::function<void()> during_read;
    std::function<void()> during_membership;
    std::function<void()> during_kernel;
    std::function<void()> during_session;
    int session_calls{0};
    std::optional<AccountFacts> sid_override;
    std::map<std::wstring,ApiResult<AccountFacts>> name_overrides;
    std::vector<std::array<DWORD,3>> direct_query_arguments;
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
        if(sid_override)result=*sid_override;
        if (change_user_after && sid_lookups > 1) result.sid = technician.sid;
        return good(result);
    }
    ApiResult<AccountFacts> account_from_name(const std::wstring& name) override {
        ++lookups;
        if (!name_lookup_ok) return bad<AccountFacts>();
        if(const auto found=name_overrides.find(name);found!=name_overrides.end())return found->second;
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
        direct_query_arguments.push_back({level,flags,bytes});
        if(during_membership)during_membership();
        if (delay_after == 1) tick += std::chrono::milliseconds(300);
        return groups;
    }
    ApiResult<SessionFacts> session_facts(DWORD id) override {
        CHECK(id == facts.session_id);
        ++session_calls;
        if(during_session)during_session();
        if (delay_after == 2) tick += std::chrono::milliseconds(300);
        return session_ok ? good(session) : bad<SessionFacts>();
    }
    ApiResult<bool> role_access_check(HANDLE token,std::span<const std::byte> role) override {
        CHECK(token==fake_handle(3) && held.contains(token));
        CHECK(SidBytes(role.begin(),role.end())==teacher.sid || SidBytes(role.begin(),role.end())==technician.sid);
        ++kernel_calls;
        if(during_kernel)during_kernel();
        if(delay_after==3)tick+=std::chrono::milliseconds(300);
        const auto per_role=SidBytes(role.begin(),role.end())==teacher.sid?teacher_kernel:technician_kernel;
        return kernel_ok?good(per_role.value_or(kernel_allow)):bad<bool>();
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
    run("post_kernel_single_teacher_alias_recreated_denied", [] {
        Fixture f;
        f.api->bindings.technician.reset();
        f.api->during_kernel = [&] { f.api->teacher.sid = f.api->technician.sid; };
        auto r = f.receive();
        no_grant(r);
        CHECK(r.status == AuthorizationStatus::binding_changed);
    });
    run("post_kernel_principal_remap_denied", [] {
        Fixture f;
        f.api->during_kernel = [&] { f.api->user.sid = f.api->teacher.sid; };
        auto r = f.receive();
        no_grant(r);
        CHECK(r.status == AuthorizationStatus::identity_unavailable);
    });
    run("post_kernel_direct_membership_removal_denied", [] {
        Fixture f;
        f.api->during_kernel = [&] { f.api->groups = {0,0,0,0,true,{}}; };
        auto r = f.receive();
        no_grant(r);
        CHECK(r.status == AuthorizationStatus::not_direct_member);
    });
    run("post_kernel_membership_result_errors_denied", [] {
        for(int mode=0;mode!=10;++mode){
            Fixture f;
            f.api->during_kernel=[&] {
                if(mode==0)f.api->groups.status=ERROR_MORE_DATA;
                if(mode==1)f.api->groups.status=ERROR_ACCESS_DENIED;
                if(mode==2)f.api->groups.structure_valid=false;
                if(mode==3)f.api->groups.total=3;
                if(mode==4)f.api->groups.aliases.pop_back();
                if(mode==5){
                    f.api->groups.aliases.clear();
                    for(int index=0;index!=4097;++index)
                        f.api->groups.aliases.push_back(L"Group"+std::to_wstring(index));
                    f.api->groups.entries=4097;f.api->groups.total=4097;
                }
                if(mode==6)f.api->groups.buffer_bytes=1048577;
                if(mode==7)f.api->groups.aliases[0]=L"invalid\\name";
                if(mode==8)f.api->groups.aliases[0]=std::wstring(257,L'x');
                if(mode==9)f.api->groups.aliases={L"Teachers",L"teachers"};
            };
            auto r=f.receive();no_grant(r);
            CHECK(r.status==(mode==1?AuthorizationStatus::membership_unavailable:
                                     AuthorizationStatus::membership_incomplete));
            CHECK(r.native_error==(mode==0?DWORD{ERROR_MORE_DATA}:
                                   mode==1?DWORD{ERROR_ACCESS_DENIED}:DWORD{0}));
            CHECK(f.api->direct_calls==2 && f.api->session_calls==1);
            CHECK(f.api->tick==Clock::time_point(std::chrono::seconds(1)));
            CHECK(f.api->session.known && f.api->session.active && f.api->session.unlocked);
            CHECK(f.api->bindings.generation==7);
        }
    });
    run("post_kernel_identity_and_alias_query_errors_denied", [] {
        for(int mode=0;mode!=16;++mode){
            Fixture f;f.api->bindings.technician.reset();
            f.api->during_kernel=[&] {
                if(mode==0)f.api->sid_lookup_ok=false;
                if(mode==1)f.api->name_lookup_ok=false;
                if(mode>=2 && mode<=5){
                    auto changed=f.api->user;
                    if(mode==2)changed.type=SidTypeAlias;
                    if(mode==3)changed.sid=f.api->technician.sid;
                    if(mode==4)changed.name=L"Remapped";
                    if(mode==5)changed.domain=L"OTHER";
                    f.api->sid_override=changed;
                }
                if(mode>=6 && mode<=9){
                    auto changed=f.api->user;
                    if(mode==6)changed.type=SidTypeAlias;
                    if(mode==7)changed.sid=f.api->technician.sid;
                    if(mode==8)changed.name=L"Remapped";
                    if(mode==9)changed.domain=L"OTHER";
                    f.api->name_overrides[L"LAB\\Operator"]=good(changed);
                }
                if(mode==10)f.api->name_overrides[L"LAB\\Teachers"]=bad<AccountFacts>();
                if(mode==11)f.api->teacher.type=SidTypeUser;
                if(mode==12)f.api->teacher.sid=f.api->technician.sid;
                if(mode==13)f.api->teacher.name=L"Recreated";
                if(mode==14)f.api->teacher.domain=L"OTHER";
                if(mode==15)f.api->name_overrides[L"LAB\\Operator"]=bad<AccountFacts>(ERROR_NONE_MAPPED);
            };
            auto r=f.receive();no_grant(r);
            CHECK(r.status==(mode>=10 && mode<=14?AuthorizationStatus::binding_changed:
                                                   AuthorizationStatus::identity_unavailable));
            if(mode==0 || mode==1 || mode==10)CHECK(r.native_error==ERROR_ACCESS_DENIED);
            if(mode==15)CHECK(r.native_error==ERROR_NONE_MAPPED);
            CHECK(f.api->direct_calls==2 && f.api->kernel_calls==1);
        }
        for(bool alias_change:{false,true}){
            Fixture f;f.api->bindings.technician.reset();
            f.api->during_membership=[&] {
                if(f.api->direct_calls!=2)return;
                if(alias_change)f.api->teacher.sid=f.api->technician.sid;
                else f.api->user.sid=f.api->teacher.sid;
            };
            auto r=f.receive();no_grant(r);
            CHECK(r.status==(alias_change?AuthorizationStatus::binding_changed:AuthorizationStatus::identity_unavailable));
        }
    });
    run("second_membership_query_final_guards_and_original_age", [] {
        for(int mode=0;mode!=6;++mode){
            Fixture f;
            f.api->during_membership=[&] {
                if(f.api->direct_calls!=2)return;
                if(mode==0)f.api->tick+=std::chrono::milliseconds(250);
                if(mode==1)f.api->tick-=std::chrono::milliseconds(1);
                if(mode==2)f.api->session.unlocked=false;
                if(mode==3)++f.api->bindings.generation;
                if(mode==4)f.api->session.known=false;
                if(mode==5)f.api->session.active=false;
            };
            auto r=f.receive();no_grant(r);
            const auto expected=mode<2?AuthorizationStatus::evidence_expired:
                mode==2?AuthorizationStatus::session_locked:
                mode==3?AuthorizationStatus::generation_changed:
                mode==4?AuthorizationStatus::session_unavailable:AuthorizationStatus::session_inactive;
            CHECK(r.status==expected && f.api->direct_calls==2);
        }
        Fixture within_age;
        within_age.api->during_membership=[&] {
            if(within_age.api->direct_calls==2)within_age.api->tick+=std::chrono::milliseconds(249);
        };
        auto within=within_age.receive();CHECK(within.authorized());
        within_age.api->tick+=std::chrono::milliseconds(1);
        auto expired=within_age.provider.revalidate_for_dispatch(*within.evidence,within_age.api->bindings);
        no_grant(expired);CHECK(expired.status==AuthorizationStatus::evidence_expired);
        CHECK(within_age.api->direct_calls==2);
        Fixture changed_binding;
        changed_binding.api->during_session=[&] {
            if(changed_binding.api->session_calls==2)
                changed_binding.api->bindings.teacher->qualified_local_alias=L"LAB\\RenamedTeachers";
        };
        auto changed=changed_binding.receive();no_grant(changed);
        CHECK(changed.status==AuthorizationStatus::binding_changed);
        CHECK(changed_binding.api->bindings.generation==7 && changed_binding.api->direct_calls==2);
        Fixture session_error;
        session_error.api->during_membership=[&] {if(session_error.api->direct_calls==2)session_error.api->session_ok=false;};
        auto unknown=session_error.receive();no_grant(unknown);
        CHECK(unknown.status==AuthorizationStatus::session_unavailable && unknown.native_error==ERROR_ACCESS_DENIED);
    });
    run("post_kernel_snapshot_positive_and_call_budget_controls", [] {
        for(bool single:{false,true}){
            Fixture f;if(single)f.api->bindings.technician.reset();
            auto r=f.receive();CHECK(r.authorized());
            CHECK(f.api->direct_calls==2 && f.api->sid_lookups==3);
            CHECK(f.api->lookups==(single?6:9) && f.api->kernel_calls==(single?1:2));
            CHECK(f.api->session_calls==2 && f.api->direct_query_arguments.size()==2);
            for(const auto& args:f.api->direct_query_arguments)
                CHECK((args==std::array<DWORD,3>{0,0,32768}));
            const int post_calls=(f.api->direct_calls-1)+(f.api->sid_lookups-2)+
                                 (f.api->lookups-(single?4:6));
            CHECK(post_calls==(single?4:5));
        }
        Fixture partial;partial.api->technician_kernel=false;
        auto teacher_only=partial.receive();CHECK(teacher_only.authorized());
        CHECK(teacher_only.role_grants.teacher && !teacher_only.role_grants.technician);
        CHECK(partial.api->direct_calls==2 && partial.api->kernel_calls==2);
        Fixture removed;removed.api->during_kernel=[&] {removed.api->groups={0,1,1,128,true,{L"Technicians"}};};
        auto lost_teacher=removed.receive();no_grant(lost_teacher);
        CHECK(lost_teacher.status==AuthorizationStatus::not_direct_member && removed.api->kernel_calls==2);
        Fixture added;added.api->groups={0,1,1,128,true,{L"Teachers"}};
        added.api->during_kernel=[&] {added.api->groups={0,2,2,256,true,{L"Teachers",L"Technicians"}};};
        auto unchanged=added.receive();CHECK(unchanged.authorized());
        CHECK(unchanged.role_grants.teacher && !unchanged.role_grants.technician);
        CHECK(added.api->kernel_calls==1 && added.api->direct_calls==2);
        Fixture denied;denied.api->kernel_allow=false;auto none=denied.receive();no_grant(none);
        CHECK(none.status==AuthorizationStatus::kernel_access_denied);
        CHECK(denied.api->direct_calls==1 && denied.api->kernel_calls==2);
        Fixture kernel_error;kernel_error.api->kernel_ok=false;auto error=kernel_error.receive();no_grant(error);
        CHECK(error.status==AuthorizationStatus::kernel_access_unavailable && kernel_error.api->direct_calls==1);
        std::cout<<"Post-kernel prospective grant calls: dual5/single4; direct queries2; original kernel decisions retained.\n";
    });
    run("dispatch_post_kernel_changes_deny_without_recapture", [] {
        for(int mode=0;mode!=3;++mode){
            Fixture f;if(mode==2)f.api->bindings.technician.reset();
            auto captured=f.receive();CHECK(captured.authorized());
            const int reads=f.api->reads;
            f.api->during_kernel=[&] {
                if(mode==0)f.api->groups={0,0,0,0,true,{}};
                if(mode==1)f.api->user.sid=f.api->teacher.sid;
                if(mode==2)f.api->teacher.sid=f.api->technician.sid;
            };
            auto r=f.provider.revalidate_for_dispatch(*captured.evidence,f.api->bindings);no_grant(r);
            CHECK(r.status==(mode==0?AuthorizationStatus::not_direct_member:
                             mode==1?AuthorizationStatus::identity_unavailable:AuthorizationStatus::binding_changed));
            CHECK(f.api->reads==reads && f.api->direct_calls==4 && f.api->reverts==1);
        }
    });
    run("filter_history_is_not_a_role_veto", [] {
        Fixture f; f.api->facts.ever_filtered=true;
        auto r=f.receive(); CHECK(r.authorized());
    });
    run("role_kernel_allow_deny_error_and_current_restrictions", [] {
        Fixture f; f.api->facts.ever_filtered=true; f.api->facts.restricting_sids={f.api->teacher.sid};
        f.api->technician_kernel=false;
        auto allowed=f.receive(); CHECK(allowed.authorized());
        CHECK(allowed.role_grants.teacher && !allowed.role_grants.technician && f.api->kernel_calls==2);
        f.api->kernel_allow=false; no_grant(f.receive());
        f.api->kernel_allow=true; f.api->kernel_ok=false; no_grant(f.receive());
    });
    run("kernel_check_late_expiry_lock_and_malformed_current_sid", [] {
        Fixture expired; expired.api->delay_after=3; auto r=expired.receive();
        no_grant(r); CHECK(r.status==AuthorizationStatus::evidence_expired);
        Fixture locked; locked.api->during_kernel=[&] {locked.api->session.unlocked=false;};
        no_grant(locked.receive());
        Fixture malformed; malformed.api->facts.restricting_sids={SidBytes{std::byte{1}}};
        no_grant(malformed.receive()); CHECK(malformed.api->kernel_calls==0);
    });
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
    run("unsupported_identity_shapes_or_current_kernel_denial", [] {
        for(int mode=0;mode<6;++mode) {
            Fixture f;
            if(mode==0)f.api->facts.anonymous=true;
            if(mode==1){f.api->facts.ever_filtered=true;f.api->facts.restricting_sids={f.api->teacher.sid};f.api->kernel_allow=false;}
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
    DWORD restricting_count=0;
    CHECK(buffer.size()>=sizeof(restricting_count));
    std::memcpy(&restricting_count,buffer.data(),sizeof(restricting_count));
    std::cout<<"Native restricting-SID layout: returned_bytes="<<needed<<"; count="<<restricting_count<<"; prefix="<<sizeof(DWORD)<<"; nonempty_array_offset="<<offsetof(TOKEN_GROUPS,Groups)<<"\n";
    CHECK(restricting_count==0);
    const auto observed=observe_owned_token_classification_read_only(fixture.impersonation.value);
    std::cout<<"Native owned-derived observation: independent_has_restrictions=1; "
             <<"restricting_sids=0; adapter_query="<<observed.query_succeeded
             <<"; adapter_error="<<observed.native_error
             <<"; adapter_ever_filtered="<<observed.ever_filtered
             <<"; identity_shape_valid="<<observed.identity_shape_valid
             <<"; managed_grants=not-tested; token_application=none.\n";
    CHECK(observed.query_succeeded && observed.ever_filtered && observed.identity_shape_valid && observed.restricting_sid_count==0);
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

BOOL WINAPI empty_restriction_prefix(HANDLE,TOKEN_INFORMATION_CLASS,LPVOID,DWORD,PDWORD);
BOOL WINAPI restriction_query_error(HANDLE,TOKEN_INFORMATION_CLASS,LPVOID,DWORD,PDWORD);
BOOL WINAPI malformed_restriction_array(HANDLE,TOKEN_INFORMATION_CLASS,LPVOID,DWORD,PDWORD);
BOOL WINAPI overlapping_restriction_sid(HANDLE,TOKEN_INFORMATION_CLASS,LPVOID,DWORD,PDWORD);
void native_query_errors() {
    DerivedTokenFixture fixture;
    current_case="actual_native_hasrestrictions_access_denied";
    restriction_query_calls=0;
    const auto denied=observe_owned_token_classification_read_only(
        fixture.impersonation.value,denied_restriction_query);
    std::cout<<"Injected native-adapter query error: calls="<<restriction_query_calls
             <<"; query_succeeded="<<denied.query_succeeded
             <<"; error="<<denied.native_error<<"; identity_shape_valid="<<denied.identity_shape_valid
             <<"; managed_grants=not-tested.\n";
    CHECK(restriction_query_calls==1 && !denied.query_succeeded &&
          denied.native_error==ERROR_ACCESS_DENIED && !denied.identity_shape_valid);
    current_case="actual_native_restriction_size_error";
    restriction_query_calls=0;
    const auto short_size=observe_owned_token_classification_read_only(
        fixture.impersonation.value,short_restriction_query);
    CHECK(restriction_query_calls==1 && !short_size.query_succeeded &&
          short_size.native_error==ERROR_INVALID_DATA && !short_size.identity_shape_valid);
    for(DWORD length : {DWORD{0},DWORD{2},DWORD{5}}){
        injected_restriction_length=length; restriction_query_calls=0;
        const auto invalid=observe_owned_token_classification_read_only(fixture.impersonation.value,invalid_restriction_length);
        CHECK(restriction_query_calls==1 && !invalid.query_succeeded && invalid.native_error==ERROR_INVALID_DATA && !invalid.identity_shape_valid);
    }
    current_case="documentary_dword4_injected_compatibility"; restriction_query_calls=0;
    const auto documentary=observe_owned_token_classification_read_only(fixture.impersonation.value,documentary_dword_restriction);
    CHECK(restriction_query_calls==1 && documentary.query_succeeded && documentary.ever_filtered && documentary.identity_shape_valid);
    current_case="validated_byte_prefix_ignores_unwritten_tail"; restriction_query_calls=0;
    const auto prefix=observe_owned_token_classification_read_only(fixture.impersonation.value,byte_prefix_restriction);
    CHECK(restriction_query_calls==1 && prefix.query_succeeded && !prefix.ever_filtered);
    std::cout<<"Injected documentaryDWORD4 and BYTE1 validated-prefix checks passed; no nativeDWORD4 or role grant claim.\n";
    const auto null_query=observe_owned_token_classification_read_only(
        fixture.impersonation.value,nullptr);
    CHECK(!null_query.query_succeeded && null_query.native_error==ERROR_INVALID_PARAMETER);
    const auto empty_prefix=observe_owned_token_classification_read_only(fixture.impersonation.value,empty_restriction_prefix);
    CHECK(empty_prefix.query_succeeded && empty_prefix.restricting_sid_count==0 && empty_prefix.restricting_sid_bytes==sizeof(DWORD));
    const auto restricting_error=observe_owned_token_classification_read_only(fixture.impersonation.value,restriction_query_error);
    CHECK(!restricting_error.query_succeeded && restricting_error.native_error==ERROR_ACCESS_DENIED);
    const auto malformed=observe_owned_token_classification_read_only(fixture.impersonation.value,malformed_restriction_array);
    CHECK(!malformed.query_succeeded && malformed.native_error==ERROR_INVALID_DATA);
    const auto overlap=observe_owned_token_classification_read_only(fixture.impersonation.value,overlapping_restriction_sid);
    CHECK(!overlap.query_succeeded && overlap.native_error==ERROR_INVALID_DATA);
    std::cout<<"Injected native-adapter short-size/null/current-restriction failures denied; no authority evidence.\n";
}


struct CurrentRestrictionFixture {
    Handle base, restricted, impersonation;
    explicit CurrentRestrictionFixture(SidBytes restriction) {
        CHECK(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY|TOKEN_DUPLICATE,&base.value));
        SID_AND_ATTRIBUTES entry{restriction.data(),0};
        CHECK(CreateRestrictedToken(base.value,0,0,nullptr,0,nullptr,1,&entry,&restricted.value));
        CHECK(DuplicateTokenEx(restricted.value,TOKEN_QUERY,nullptr,SecurityImpersonation,
                               TokenImpersonation,&impersonation.value));
    }
};
BOOL WINAPI denied_kernel_query(PSECURITY_DESCRIPTOR,HANDLE,DWORD,PGENERIC_MAPPING,
    PPRIVILEGE_SET,LPDWORD,LPDWORD,LPBOOL) {
    SetLastError(ERROR_ACCESS_DENIED);return FALSE;
}
BOOL WINAPI oversized_kernel_privileges(PSECURITY_DESCRIPTOR,HANDLE,DWORD,PGENERIC_MAPPING,
    PPRIVILEGE_SET,LPDWORD size,LPDWORD,LPBOOL) {
    *size=4097;SetLastError(ERROR_INSUFFICIENT_BUFFER);return FALSE;
}
BOOL WINAPI malformed_kernel_grant(PSECURITY_DESCRIPTOR,HANDLE,DWORD,PGENERIC_MAPPING,
    PPRIVILEGE_SET privileges,LPDWORD size,LPDWORD granted,LPBOOL access) {
    privileges->PrivilegeCount=0;*size=sizeof(PRIVILEGE_SET);*granted=0;*access=TRUE;
    return TRUE;
}
BOOL WINAPI privilege_backed_kernel_grant(PSECURITY_DESCRIPTOR,HANDLE,DWORD,PGENERIC_MAPPING,
    PPRIVILEGE_SET privileges,LPDWORD size,LPDWORD granted,LPBOOL access) {
    privileges->PrivilegeCount=1;*size=sizeof(PRIVILEGE_SET);*granted=1;*access=TRUE;
    return TRUE;
}
int kernel_resize_calls=0;
BOOL WINAPI resized_kernel_query(PSECURITY_DESCRIPTOR descriptor,HANDLE token,DWORD desired,
    PGENERIC_MAPPING mapping,PPRIVILEGE_SET privileges,LPDWORD size,LPDWORD granted,LPBOOL access) {
    ++kernel_resize_calls;
    if(kernel_resize_calls==1){
        *size=sizeof(PRIVILEGE_SET)+sizeof(LUID_AND_ATTRIBUTES);
        SetLastError(ERROR_INSUFFICIENT_BUFFER);return FALSE;
    }
    return AccessCheck(descriptor,token,desired,mapping,privileges,size,granted,access);
}
void native_kernel_tests() {
    const auto everyone=sid(L"S-1-1-0"); // In-memory non-role surrogate only.
    current_case="native_privilege_filtered_kernel_surrogate";
    DerivedTokenFixture privilege_only;
    auto allowed=observe_owned_kernel_access_read_only(privilege_only.impersonation.value,everyone);
    std::cout<<"Native virtual resource: privilege-only history; api="<<allowed.ok
             <<"; allowed="<<allowed.value<<"; error="<<allowed.error<<"; managed_role=not-tested.\n";
    CHECK(allowed.ok && allowed.value);
    current_case="native_restricting_sid_second_pass_denied";
    CurrentRestrictionFixture mismatched(sid(L"S-1-5-12"));
    auto mismatched_facts=observe_owned_token_classification_read_only(mismatched.impersonation.value);
    CHECK(mismatched_facts.query_succeeded && mismatched_facts.restricting_sid_count==1);
    auto denied=observe_owned_kernel_access_read_only(mismatched.impersonation.value,everyone);
    std::cout<<"Native current restriction: count="<<mismatched_facts.restricting_sid_count
             <<"; returned_bytes="<<mismatched_facts.restricting_sid_bytes
             <<"; api="<<denied.ok<<"; allowed="<<denied.value
             <<"; first-pass surrogate not a role.\n";
    CHECK(denied.ok && !denied.value);
    current_case="native_restricting_sid_both_passes_allow";
    CurrentRestrictionFixture matching(everyone);
    auto matching_facts=observe_owned_token_classification_read_only(matching.impersonation.value);
    CHECK(matching_facts.query_succeeded && matching_facts.restricting_sid_count==1);
    auto both=observe_owned_kernel_access_read_only(matching.impersonation.value,everyone);
    CHECK(both.ok && both.value);
    std::cout<<"Native both-check virtual resource allowed; restricting_count=1; no configured role/evidence.\n";
    current_case="native_kernel_errors_and_outputs_fail_closed";
    for(auto query : {denied_kernel_query,oversized_kernel_privileges,
                       malformed_kernel_grant,privilege_backed_kernel_grant}) {
        const auto failed=observe_owned_kernel_access_read_only(
            privilege_only.impersonation.value,everyone,query);
        CHECK(!failed.ok && !failed.value);
    }
    kernel_resize_calls=0;
    auto resized=observe_owned_kernel_access_read_only(
        privilege_only.impersonation.value,everyone,resized_kernel_query);
    CHECK(resized.ok && resized.value && kernel_resize_calls==2);
    CHECK(!observe_owned_kernel_access_read_only(privilege_only.impersonation.value,
                                                 SidBytes{std::byte{1}}).ok);
    std::cout<<"Native adapter error/descriptor/output/one-resize checks passed; surrogate only, no token application.\n";
}
BOOL WINAPI empty_restriction_prefix(HANDLE token,TOKEN_INFORMATION_CLASS kind,
                                    LPVOID data,DWORD bytes,PDWORD returned) {
    if(kind!=TokenRestrictedSids)return GetTokenInformation(token,kind,data,bytes,returned);
    if(!data || bytes<sizeof(DWORD)){
        *returned=sizeof(DWORD);SetLastError(ERROR_INSUFFICIENT_BUFFER);return FALSE;
    }
    const DWORD count=0;std::memcpy(data,&count,sizeof(count));*returned=sizeof(DWORD);return TRUE;
}
BOOL WINAPI restriction_query_error(HANDLE token,TOKEN_INFORMATION_CLASS kind,
                                   LPVOID data,DWORD bytes,PDWORD returned) {
    if(kind!=TokenRestrictedSids)return GetTokenInformation(token,kind,data,bytes,returned);
    SetLastError(ERROR_ACCESS_DENIED);return FALSE;
}
BOOL WINAPI malformed_restriction_array(HANDLE token,TOKEN_INFORMATION_CLASS kind,
                                       LPVOID data,DWORD bytes,PDWORD returned) {
    if(kind!=TokenRestrictedSids)return GetTokenInformation(token,kind,data,bytes,returned);
    if(!data || bytes<sizeof(DWORD)){
        *returned=sizeof(DWORD);SetLastError(ERROR_INSUFFICIENT_BUFFER);return FALSE;
    }
    const DWORD count=1;std::memcpy(data,&count,sizeof(count));*returned=sizeof(DWORD);return TRUE;
}

BOOL WINAPI overlapping_restriction_sid(HANDLE token,TOKEN_INFORMATION_CLASS kind,
    LPVOID data,DWORD bytes,PDWORD returned) {
    if(kind!=TokenRestrictedSids)return GetTokenInformation(token,kind,data,bytes,returned);
    constexpr DWORD size=offsetof(TOKEN_GROUPS,Groups)+sizeof(SID_AND_ATTRIBUTES);
    if(!data || bytes<size){*returned=size;SetLastError(ERROR_INSUFFICIENT_BUFFER);return FALSE;}
    std::memset(data,0,size);const DWORD count=1;std::memcpy(data,&count,sizeof(count));
    SID_AND_ATTRIBUTES item{data,0};
    std::memcpy(static_cast<std::byte*>(data)+offsetof(TOKEN_GROUPS,Groups),&item,sizeof(item));
    *returned=size;return TRUE;
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
        else if(mode=="--native-kernel-role-access")native_kernel_tests();
        else return 2;
        return 0;
    } catch(const std::exception&) {
        std::cerr<<"AUTH01 qualification failed: "<<current_case<<"\n";
        return 1;
    }
}
