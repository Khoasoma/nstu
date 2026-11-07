#include <windows.h>
#include <objbase.h>
#include <uiautomation.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#ifdef NDEBUG
#error Release checks must remain active
#endif
HRESULT probe_control(HWND, int, int);
HRESULT probe_latch(HWND, bool, bool);
unsigned probe_observe(HWND, int);
HRESULT probe_provider(HWND, int, REFIID, void**);
struct ProbeFaultOutcome {
    int status{};
    unsigned created{}, retired{}, remaining{};
    bool window_retired{}, registry_cleared{}, worker_retired{}, before_com_uninitialize{};
    long long elapsed_ms{};
};

enum class ProbeActorKind : unsigned { direct_client, os_value_client, own_input, fixture_control, provider_untagged };
enum class ProbeActorStage : unsigned {
    ready, call_begin, call_return, query_begin, query_return, baseline, latch_active,
    target_admitted, expiry, latch_release, after_pop, assertion_begin, assertion_return, epoch_end
};
enum class ProbeRemovalReason : unsigned { none, front_pop, prune_completed, notify_rollback, retirement_clear };
enum class ProbeEvidenceRead : unsigned { copied, busy, invalid };
struct ProbeFocusEvidence {
    bool valid{}, trace_complete{}, target_present{}, held{}, host_focused{};
    unsigned generation{}, target_token{}, ordinary_total{}, ordinary_pending{}, control_total{}, target_completion{};
    std::uint64_t event_sequence{}, pump_cycle{}, removal_sequence{}, post_pump_sequence{};
    std::uint64_t pump_first_front_pop_sequence{}, pump_last_front_pop_sequence{};
    ProbeRemovalReason removal_reason{ProbeRemovalReason::none};
    int semantic_focus{}; HRESULT target_result{E_PENDING};
    bool target_mutated{}, other_valid_mutation{}, unplanned_action{};
};
struct ProbeDiagnosticRecord {
    std::uint64_t sequence{}, epoch{}, pump{}, actor{}, candidate_os{};
    unsigned token{}, generation{}; DWORD caller{}, executing{};
    unsigned phase{}, stage{}, action{}, node{}, completion{};
    ProbeActorKind origin{ProbeActorKind::provider_untagged}; UINT input{};
    int before{}, after{}; bool held{}, host_focused{}, expired{};
    unsigned total{}, pending{}, controls{}, total_before{}, controls_before{}; HRESULT result{E_PENDING};
    std::uint64_t first_pop{}, last_pop{};
};
struct ProbeEpochSnapshot {
    ProbeFocusEvidence evidence; std::uint64_t epoch{}; unsigned count{};
    bool armed{}, disarm_confirmed{}; std::array<ProbeDiagnosticRecord,256> records{};
};
std::uint64_t probe_begin_focus_epoch(HWND);
void probe_actor_mark(HWND, std::uint64_t, std::uint64_t, ProbeActorKind, ProbeActorStage, HRESULT = E_PENDING);
ProbeEvidenceRead probe_try_epoch_snapshot(HWND, std::uint64_t, std::uint64_t, ProbeEpochSnapshot&, bool = false) noexcept;
std::string probe_format_epoch_snapshot(const ProbeEpochSnapshot&);

ProbeFaultOutcome probe_fault_scenario(int);
bool probe_regression(int);
void probe_mark_focus(HWND, int);
bool probe_focus_cancellation_proven(HWND);
std::string probe_focus_trace(HWND);
namespace {
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
constexpr HRESULT unavailable = static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE);
constexpr HRESULT invalid = static_cast<HRESULT>(UIA_E_INVALIDOPERATION);
constexpr HRESULT disabled = static_cast<HRESULT>(UIA_E_ELEMENTNOTENABLED);
constexpr HRESULT timeout = static_cast<HRESULT>(UIA_E_TIMEOUT);
struct Mta {
    HRESULT result{CoInitializeEx(nullptr, COINIT_MULTITHREADED)};
    ~Mta() { if (SUCCEEDED(result)) CoUninitialize(); }
};
void check(bool result, const char* message) { if (!result) throw std::runtime_error(message); }
void success(HRESULT result, const char* message) {
    if (FAILED(result)) { std::cerr << "HRESULT 0x" << std::hex << static_cast<unsigned long>(result) << std::dec << ": " << message << '\n'; throw std::runtime_error(message); }
}
template<class Predicate>
void wait_for(Predicate predicate, const char* message, int milliseconds = 2000) {
    const auto end = Clock::now() + std::chrono::milliseconds(milliseconds);
    while (!predicate()) { check(Clock::now() < end, message); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
}
std::wstring name(IUIAutomationElement* element) {
    BSTR text{}; success(element->get_CurrentName(&text), "read OS-client name");
    const std::wstring result(text ? text : L"", SysStringLen(text)); SysFreeString(text); return result;
}
std::wstring value(IValueProvider* provider) {
    BSTR text{}; success(provider->get_Value(&text), "read direct Value");
    const std::wstring result(text ? text : L"", SysStringLen(text)); SysFreeString(text); return result;
}
std::wstring value(IUIAutomationValuePattern* provider) {
    BSTR text{}; success(provider->get_CurrentValue(&text), "read OS-client Value");
    const std::wstring result(text ? text : L"", SysStringLen(text)); SysFreeString(text); return result;
}
std::vector<LONG> runtime_id(IUIAutomationElement* element) {
    SAFEARRAY* array{}; success(element->GetRuntimeId(&array), "OS runtime ID"); check(array != nullptr, "missing runtime ID");
    LONG first{}, last{}; success(SafeArrayGetLBound(array, 1, &first), "ID lower bound"); success(SafeArrayGetUBound(array, 1, &last), "ID upper bound");
    std::vector<LONG> result;
    for (LONG i = first; i <= last; ++i) { LONG entry{}; success(SafeArrayGetElement(array, &i, &entry), "ID entry"); result.push_back(entry); }
    SafeArrayDestroy(array); return result;
}
class Events final : public IUIAutomationEventHandler, public IUIAutomationPropertyChangedEventHandler {
public:
    std::atomic<unsigned> invokes{}, values{}, focus{};
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (iid == IID_IUnknown || iid == IID_IUIAutomationEventHandler) *result = static_cast<IUIAutomationEventHandler*>(this);
        else if (iid == IID_IUIAutomationPropertyChangedEventHandler) *result = static_cast<IUIAutomationPropertyChangedEventHandler*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto count = --references_; if (!count) { const std::unique_ptr<Events> retired(this); } return count;
    }
    HRESULT STDMETHODCALLTYPE HandleAutomationEvent(IUIAutomationElement*, EVENTID event) override {
        if (event == UIA_Invoke_InvokedEventId) ++invokes;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE HandlePropertyChangedEvent(IUIAutomationElement*, PROPERTYID property, VARIANT changed) override {
        if (property == UIA_ValueValuePropertyId && changed.vt == VT_BSTR) ++values;
        if (property == UIA_HasKeyboardFocusPropertyId) ++focus;
        return S_OK;
    }
private:
    std::atomic<ULONG> references_{1};
};
struct Subscriptions {
    ComPtr<IUIAutomation> client; ComPtr<IUIAutomationElement> root; ComPtr<Events> events;
    bool invokes{}, properties{};
    HRESULT remove() noexcept {
        HRESULT result = S_OK;
        if (properties) { const auto hr = client->RemovePropertyChangedEventHandler(root.Get(), events.Get()); if (FAILED(hr)) result = hr; properties = false; }
        if (invokes) { const auto hr = client->RemoveAutomationEventHandler(UIA_Invoke_InvokedEventId, root.Get(), events.Get()); if (FAILED(hr)) result = hr; invokes = false; }
        return result;
    }
    ~Subscriptions() { const auto hr = remove(); if (FAILED(hr)) std::cerr << "FAIL: originating-MTA subscription cleanup\n"; }
};
struct Completed { HRESULT result{}; long long elapsed{}; };
std::future<Completed> pending_value(ComPtr<IValueProvider> provider, std::wstring text) {
    return std::async(std::launch::async, [provider = std::move(provider), text = std::move(text)] {
        Mta mta; success(mta.result, "secondary MTA");
        const auto start = Clock::now(); const auto result = provider->SetValue(text.c_str());
        return Completed{result, std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count()};
    });
}

class ActorScope {
public:
    ActorScope(HWND window, std::uint64_t epoch, std::uint64_t actor, ProbeActorKind kind,
               ProbeActorStage begin = ProbeActorStage::call_begin)
        : window_(window), epoch_(epoch), actor_(actor), kind_(kind), begin_(begin) {
        probe_actor_mark(window_,epoch_,actor_,kind_,begin_);
    }
    ~ActorScope() { probe_actor_mark(window_,epoch_,actor_,kind_,
        begin_==ProbeActorStage::query_begin ? ProbeActorStage::query_return : ProbeActorStage::call_return,result_); }
    void result(HRESULT value) noexcept { result_=value; }
private:
    HWND window_; std::uint64_t epoch_,actor_; ProbeActorKind kind_; ProbeActorStage begin_; HRESULT result_{E_FAIL};
};

bool valid_identity(const ProbeEpochSnapshot&,std::uint64_t,unsigned) noexcept;
bool valid_final(const ProbeEpochSnapshot&,std::uint64_t,unsigned) noexcept;
bool canceled_front_pop(const ProbeFocusEvidence&) noexcept;
bool isolated_final(const ProbeEpochSnapshot&,std::uint64_t,unsigned) noexcept;
enum class OsValueEvidence { consistent_success, consistent_timeout, missing, ambiguous, mismatch };
OsValueEvidence adjudicate_os_value(const ProbeEpochSnapshot&,std::uint64_t,HRESULT) noexcept;

bool valid_identity(const ProbeEpochSnapshot& s,std::uint64_t epoch,unsigned generation) noexcept {
    return s.epoch==epoch && s.evidence.valid && s.evidence.generation==generation && s.evidence.trace_complete;
}
bool valid_final(const ProbeEpochSnapshot& s,std::uint64_t epoch,unsigned generation) noexcept {
    return valid_identity(s,epoch,generation) && s.disarm_confirmed && !s.armed;
}
bool canceled_front_pop(const ProbeFocusEvidence& e) noexcept {
    return e.target_token!=0 && !e.target_present && !e.target_mutated && e.target_completion==2 &&
        e.target_result==timeout && e.removal_reason==ProbeRemovalReason::front_pop &&
        e.post_pump_sequence>e.removal_sequence && e.pump_first_front_pop_sequence<=e.removal_sequence &&
        e.pump_last_front_pop_sequence>=e.removal_sequence;
}
bool isolated_final(const ProbeEpochSnapshot& s,std::uint64_t epoch,unsigned generation) noexcept {
    if(!valid_final(s,epoch,generation) || !canceled_front_pop(s.evidence) || s.evidence.held ||
       s.evidence.semantic_focus!=1 || !s.evidence.host_focused ||
       s.evidence.other_valid_mutation || s.evidence.unplanned_action)return false;
    std::uint64_t begin{},end{};
    for(unsigned i=0;i<s.count;++i) {
        const auto& r=s.records[i];
        if(r.epoch!=epoch || r.generation!=generation)return false;
        if(r.phase==0 && r.stage==static_cast<unsigned>(ProbeActorStage::latch_active))begin=r.sequence;
        if(r.phase==0 && r.stage==static_cast<unsigned>(ProbeActorStage::assertion_return))end=r.sequence;
    }
    if(!begin || end<=begin)return false;
    for(unsigned i=0;i<s.count;++i) {
        const auto& r=s.records[i];
        if(r.sequence>begin && r.sequence<=end && r.phase==12 && !r.host_focused)return false;
    }
    return true;
}
OsValueEvidence adjudicate_os_value(const ProbeEpochSnapshot& s,std::uint64_t os_actor,HRESULT actual) noexcept {
    std::uint64_t begin{},end{};unsigned begins{},ends{};HRESULT returned=E_PENDING;
    for(unsigned i=0;i<s.count;++i) {
        const auto& r=s.records[i];
        if(r.epoch!=s.epoch || r.generation!=s.evidence.generation)return OsValueEvidence::mismatch;
        if(r.phase!=0 || r.actor!=os_actor || r.origin!=ProbeActorKind::os_value_client)continue;
        if(r.stage==static_cast<unsigned>(ProbeActorStage::call_begin)){begin=r.sequence;++begins;}
        if(r.stage==static_cast<unsigned>(ProbeActorStage::call_return)){end=r.sequence;returned=r.result;++ends;}
    }
    if(!begins || !ends)return OsValueEvidence::missing;
    if(begins!=1 || ends!=1)return OsValueEvidence::ambiguous;
    if(begin>=end || returned!=actual || (actual!=S_OK && actual!=timeout))return OsValueEvidence::mismatch;
    unsigned token{},submits{};const ProbeDiagnosticRecord* submission=nullptr;
    for(unsigned i=0;i<s.count;++i) {
        const auto& r=s.records[i];
        if(r.phase==1 && r.action==1 && r.node==2 && r.candidate_os==os_actor && r.sequence>begin && r.sequence<end) {
            if(token && token!=r.token)return OsValueEvidence::ambiguous;
            token=r.token;submission=&r;++submits;
        }
    }
    if(!token || !submission)return OsValueEvidence::missing;
    if(submits!=1)return OsValueEvidence::ambiguous;
    unsigned queued{},completed{},canceled{},mutations{};std::uint64_t admitted{},mutation{},completion{};
    DWORD completed_on{};bool incompatible=false;
    for(unsigned i=0;i<s.count;++i) {
        const auto& r=s.records[i];if(r.token!=token)continue;
        if(r.action!=1 || r.node!=2 || r.candidate_os!=os_actor){incompatible=true;continue;}
        if(r.phase>=1 && r.phase<=6 && (r.sequence<=begin || r.sequence>=end))incompatible=true;
        if(r.phase==2){++queued;admitted=r.sequence;}
        if(r.phase==3)incompatible=true;
        if(r.phase==4) {
            ++completed;completion=r.sequence;completed_on=r.executing;
            if(r.completion!=1 || r.result!=S_OK || r.expired)incompatible=true;
        }
        if(r.phase==5) {
            ++canceled;completion=r.sequence;
            if(r.completion!=2 || r.result!=timeout)incompatible=true;
        }
        if(r.phase==6){++mutations;mutation=r.sequence;if(r.result!=S_OK || r.expired)incompatible=true;}
    }
    if(incompatible || queued>1 || completed>1 || canceled>1 || mutations>1)return OsValueEvidence::mismatch;
    if(actual==S_OK) {
        if(canceled)return OsValueEvidence::mismatch;
        if(!completed || !mutations)return OsValueEvidence::missing;
        if(!(submission->sequence<mutation && mutation<completion))return OsValueEvidence::mismatch;
        if(queued && !(submission->sequence<admitted && admitted<mutation))return OsValueEvidence::mismatch;
        if(!queued && (submission->caller!=submission->executing || completed_on!=submission->executing))return OsValueEvidence::mismatch;
        return OsValueEvidence::consistent_success;
    }
    if(mutations || completed)return OsValueEvidence::mismatch;
    if(!queued || !canceled)return OsValueEvidence::missing;
    if(!(submission->sequence<admitted && admitted<completion))return OsValueEvidence::mismatch;
    return OsValueEvidence::consistent_timeout;
}
const char* os_value_label(OsValueEvidence result) noexcept {
    switch(result) {
    case OsValueEvidence::consistent_success:return "consistent-success";
    case OsValueEvidence::consistent_timeout:return "consistent-timeout";
    case OsValueEvidence::missing:return "missing";
    case OsValueEvidence::ambiguous:return "ambiguous";
    case OsValueEvidence::mismatch:return "mismatch";
    }
    return "mismatch";
}

void fixed_evidence_contracts() {
    ProbeEpochSnapshot sample;
    sample.epoch=7;sample.evidence.valid=true;sample.evidence.generation=9;
    sample.evidence.trace_complete=true;sample.disarm_confirmed=true;sample.armed=false;
    check(valid_final(sample,7,9),"complete matching final evidence accepted");
    check(!valid_final(sample,8,9),"copied wrong epoch rejected");
    check(!valid_final(sample,7,10),"copied wrong generation rejected");
    sample.evidence.valid=false;
    check(!valid_final(sample,7,9),"closed final evidence rejected");
    sample.evidence.valid=true;sample.evidence.trace_complete=false;
    check(!valid_final(sample,7,9),"epoch-end overflow rejected");
    sample.evidence.trace_complete=true;sample.disarm_confirmed=false;
    check(!valid_final(sample,7,9),"unconfirmed disarm rejected");
    sample.disarm_confirmed=true;sample.armed=true;
    check(!valid_final(sample,7,9),"still-armed final evidence rejected");
    sample.armed=false;
    auto& e=sample.evidence;e.target_token=3;e.target_completion=2;e.target_result=timeout;
    e.removal_reason=ProbeRemovalReason::front_pop;e.removal_sequence=10;e.post_pump_sequence=11;
    e.pump_first_front_pop_sequence=10;e.pump_last_front_pop_sequence=10;
    check(canceled_front_pop(e),"complete canceled target pop accepted");
    e.target_present=true;check(!canceled_front_pop(e),"resident canceled target rejected");e.target_present=false;
    e.pump_last_front_pop_sequence=9;check(!canceled_front_pop(e),"missing covering pump range rejected");e.pump_last_front_pop_sequence=10;
    for(auto reason:{ProbeRemovalReason::prune_completed,ProbeRemovalReason::notify_rollback,ProbeRemovalReason::retirement_clear}) {
        e.removal_reason=reason;check(!canceled_front_pop(e),"other target removal rejected");
    }
    e.removal_reason=ProbeRemovalReason::front_pop;e.target_mutated=true;
    check(!canceled_front_pop(e),"canceled target mutation rejected");e.target_mutated=false;
    e.semantic_focus=1;e.host_focused=false;
    check(!isolated_final(sample,7,9),"semantic command without host focus rejected");e.host_focused=true;
    auto add=[&](unsigned phase,unsigned stage,std::uint64_t sequence)->ProbeDiagnosticRecord& {
        auto& r=sample.records[sample.count++];r={};r.epoch=7;r.generation=9;r.phase=phase;r.stage=stage;r.sequence=sequence;return r;
    };
    add(0,static_cast<unsigned>(ProbeActorStage::latch_active),1);
    add(0,static_cast<unsigned>(ProbeActorStage::assertion_return),20);
    check(isolated_final(sample,7,9),"complete isolated final evidence accepted");
    auto earlier=sample;
    add(12,0,15).host_focused=false;add(12,0,16).host_focused=true;
    check(isolated_final(earlier,7,9)&&!isolated_final(sample,7,9),"host loss added after terminal read rejected even if restored");
    sample=earlier;sample.evidence.unplanned_action=true;
    check(!isolated_final(sample,7,9),"final interval contamination rejected");
    sample=earlier;sample.evidence.trace_complete=false;
    check(!isolated_final(sample,7,9),"newly lost final records rejected");
    sample=earlier;sample.count=0;
    auto& begin=add(0,static_cast<unsigned>(ProbeActorStage::call_begin),1);begin.actor=4;begin.origin=ProbeActorKind::os_value_client;
    auto& submit=add(1,0,2);submit.token=50;submit.action=1;submit.node=2;submit.candidate_os=4;submit.caller=submit.executing=8;
    auto& mutation=add(6,0,3);mutation.token=50;mutation.action=1;mutation.node=2;mutation.candidate_os=4;mutation.executing=8;mutation.result=S_OK;
    auto& complete=add(4,0,4);complete.token=50;complete.action=1;complete.node=2;complete.candidate_os=4;complete.executing=8;complete.completion=1;complete.result=S_OK;
    auto& end=add(0,static_cast<unsigned>(ProbeActorStage::call_return),5);end.actor=4;end.origin=ProbeActorKind::os_value_client;end.result=S_OK;
    check(adjudicate_os_value(sample,4,S_OK)==OsValueEvidence::consistent_success,"actual complete direct Value success accepted");
    auto success_sample=sample;
    auto& queued_success=add(2,0,3);queued_success.token=50;queued_success.action=1;queued_success.node=2;queued_success.candidate_os=4;
    sample.records[2].sequence=4;sample.records[3].sequence=5;sample.records[4].sequence=6;
    check(adjudicate_os_value(sample,4,S_OK)==OsValueEvidence::consistent_success,"actual complete queued Value success accepted");
    sample=success_sample;sample.records[2].expired=true;
    check(adjudicate_os_value(sample,4,S_OK)==OsValueEvidence::mismatch,"expired successful Value execution rejected");
    sample=success_sample;
    sample.records[3].phase=99;
    check(adjudicate_os_value(sample,4,S_OK)==OsValueEvidence::missing,"OS success without Value completion rejected");
    sample=success_sample;
    check(adjudicate_os_value(sample,4,timeout)==OsValueEvidence::mismatch,"actual returned HRESULT mismatch rejected");
    auto& duplicate=add(1,0,3);duplicate.token=51;duplicate.action=1;duplicate.node=2;duplicate.candidate_os=4;
    check(adjudicate_os_value(sample,4,S_OK)==OsValueEvidence::ambiguous,"incompatible Value candidates rejected");
    sample=success_sample;sample.records[2].phase=2;sample.records[2].result=S_OK;
    sample.records[3].phase=5;sample.records[3].completion=2;sample.records[3].result=timeout;sample.records[4].result=timeout;
    check(adjudicate_os_value(sample,4,timeout)==OsValueEvidence::consistent_timeout,"complete queued Value timeout accepted");
    auto& bad=add(6,0,3);bad.token=50;bad.action=1;bad.node=2;bad.candidate_os=4;bad.result=S_OK;
    check(adjudicate_os_value(sample,4,timeout)==OsValueEvidence::mismatch,"Value timeout with mutation rejected");
    sample=success_sample;sample.records[1].action=2;
    check(adjudicate_os_value(sample,4,S_OK)==OsValueEvidence::missing,"focus-only OS result is not Value proof");
}

ProbeEpochSnapshot terminal_pop(HWND window, std::uint64_t epoch, std::uint64_t actor, unsigned generation,
                                ProbeEpochSnapshot& snapshot) {
    const auto deadline=Clock::now()+std::chrono::seconds(2);
    bool complete=false;
    while(Clock::now()<deadline) {
        ProbeEpochSnapshot temporary;
        if(probe_try_epoch_snapshot(window,epoch,actor,temporary)==ProbeEvidenceRead::copied) {
            check(valid_identity(temporary,epoch,generation),"matching terminal identity and generation");
            snapshot=temporary;
            const auto& e=snapshot.evidence;
            check(e.valid && e.trace_complete,"valid complete diagnostic epoch");
            if(e.removal_reason!=ProbeRemovalReason::none && e.removal_reason!=ProbeRemovalReason::front_pop)
                throw std::runtime_error("target removed by another path, not a pump front-pop");
            if(e.removal_reason==ProbeRemovalReason::front_pop && e.post_pump_sequence>e.removal_sequence &&
               e.pump_first_front_pop_sequence<=e.removal_sequence && e.pump_last_front_pop_sequence>=e.removal_sequence &&
               !e.target_present) { complete=true; break; }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(complete,"actual target pop and covering completed UI pump pass");
    check(snapshot.evidence.target_completion==2 && snapshot.evidence.target_result==timeout,
          "target was canceled with original timeout before physical pop");
    check(!snapshot.evidence.target_mutated,"exact canceled target never mutated");
    return snapshot;
}
void finalize_fixture(HWND window,std::uint64_t epoch,unsigned generation,ProbeEpochSnapshot& retained,const char* message) {
    ProbeEpochSnapshot temporary;
    const auto read=probe_try_epoch_snapshot(window,epoch,1,temporary,true);
    const bool identity=read==ProbeEvidenceRead::copied && valid_identity(temporary,epoch,generation);
    if(identity)retained=temporary; // invalid/closed/lost final copies never replace last-valid bytes
    check(identity && valid_final(temporary,epoch,generation),message);
    check(canceled_front_pop(temporary.evidence),"final exact canceled target terminal invariants");
}
std::future<Completed> tagged_focus(HWND window, ComPtr<IRawElementProviderFragment> provider, std::uint64_t epoch,
                                   std::uint64_t actor) {
    return std::async(std::launch::async,[window,provider=std::move(provider),epoch,actor]{
        Mta mta; success(mta.result,"tagged secondary MTA");
        ActorScope tag(window,epoch,actor,ProbeActorKind::direct_client);
        const auto start=Clock::now(); const auto result=provider->SetFocus(); tag.result(result);
        return Completed{result,std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-start).count()};
    });
}

std::future<Completed> pending_focus(ComPtr<IRawElementProviderFragment> provider) {
    return std::async(std::launch::async, [provider = std::move(provider)] {
        Mta mta; success(mta.result, "focus MTA");
        const auto start = Clock::now(); const auto result = provider->SetFocus();
        return Completed{result, std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count()};
    });
}
void bounded(Completed result, HRESULT expected, const char* message) {
    std::cout << message << ": HRESULT=0x" << std::hex << static_cast<unsigned long>(result.result) << std::dec << ", elapsed_ms=" << result.elapsed << '\n';
    check(result.result == expected && result.elapsed <= 1750, message);
}

struct FocusFailureContext {
    HWND window{};std::uint64_t epoch{};unsigned expected_generation{};
    bool generation_pinned{},last_valid_available{};
    bool returned{},released{},interval_failed{},collector_exercised{},final_copy{},disarmed{};
    HRESULT actual_result{E_PENDING};long long actual_elapsed{};
    const char* stage{"before-focus-interval"};
    const char* collection_disposition{"not-exercised"};const char* finalization_disposition{"not-attempted"};
    ProbeEvidenceRead last_read{ProbeEvidenceRead::invalid};
    std::exception_ptr primary;ProbeEpochSnapshot snapshot;
    void completed(Completed value) noexcept {returned=true;actual_result=value.result;actual_elapsed=value.elapsed;}
    bool adopt(const ProbeEpochSnapshot& temporary) noexcept {
        if(!generation_pinned && temporary.epoch==epoch && temporary.evidence.valid && temporary.evidence.trace_complete) {
            expected_generation=temporary.evidence.generation;generation_pinned=true;
        }
        if(!generation_pinned || !valid_identity(temporary,epoch,expected_generation))return false;
        snapshot=temporary;last_valid_available=true;return true;
    }
    bool retain_read(ProbeEvidenceRead read,const ProbeEpochSnapshot& temporary) noexcept {
        last_read=read;
        return read==ProbeEvidenceRead::copied && adopt(temporary);
    }
    void observe() noexcept {
        ProbeEpochSnapshot temporary;
        retain_read(probe_try_epoch_snapshot(window,epoch,1,temporary),temporary);
    }
    void finalize(Clock::time_point end) noexcept {
        if(Clock::now()>=end){finalization_disposition="budget-exhausted";return;}
        ProbeEpochSnapshot temporary;last_read=probe_try_epoch_snapshot(window,epoch,1,temporary,true);
        final_copy=last_read==ProbeEvidenceRead::copied;
        disarmed=final_copy && temporary.epoch==epoch && temporary.disarm_confirmed && !temporary.armed;
        if(last_read==ProbeEvidenceRead::busy){finalization_disposition="busy";return;}
        if(!final_copy || temporary.epoch!=epoch || !temporary.evidence.valid ||
           (generation_pinned && temporary.evidence.generation!=expected_generation)) {
            finalization_disposition="invalid-identity-or-generation";return;
        }
        if(!temporary.evidence.trace_complete){finalization_disposition="lost-trace";return;}
        if(!adopt(temporary) || !valid_final(temporary,epoch,expected_generation)) {
            finalization_disposition="unconfirmed-disarm";return;
        }
        finalization_disposition="confirmed";
    }
    void freeze_success() {
        finalize(Clock::now()+std::chrono::seconds(2));
        check(finalization_disposition==std::string_view("confirmed") &&
              valid_final(snapshot,epoch,expected_generation),"final complete diagnostic copy/disarm");
        check(canceled_front_pop(snapshot.evidence),"final canceled target front-pop and covering pump");
        collection_disposition="success-terminal-copy";
    }
    void collect_failure() noexcept {
        collector_exercised=true;
        const auto end=Clock::now()+std::chrono::seconds(2);
        try {
            if(!returned || actual_result!=timeout || !released) {
                collection_disposition=!returned?"incomplete-before-target-completion":
                    actual_result!=timeout?"incomplete-unexpected-target-result":"incomplete-latch-not-released";
                finalize(end);return;
            }
            bool terminal=false,saw_usable=false;
            while(Clock::now()<end) {
                ProbeEpochSnapshot temporary;last_read=probe_try_epoch_snapshot(window,epoch,1,temporary);
                if(last_read==ProbeEvidenceRead::invalid){collection_disposition="incomplete-invalid-identity";terminal=true;break;}
                if(last_read==ProbeEvidenceRead::copied) {
                    const auto& e=temporary.evidence;
                    if(temporary.epoch!=epoch || !e.valid || (generation_pinned && e.generation!=expected_generation)) {
                        collection_disposition="incomplete-closed-or-generation";terminal=true;break;
                    }
                    if(!e.trace_complete){collection_disposition="incomplete-lost-trace";terminal=true;break;}
                    if(!adopt(temporary)){collection_disposition="incomplete-invalid-identity";terminal=true;break;}
                    saw_usable=true;
                    if(!e.target_token){collection_disposition="incomplete-no-admitted-target";terminal=true;break;}
                    if(e.held || e.target_completion!=2 || e.target_result!=timeout) {
                        collection_disposition="incomplete-target-not-canceled-released";terminal=true;break;
                    }
                    if(e.removal_reason!=ProbeRemovalReason::none && e.removal_reason!=ProbeRemovalReason::front_pop) {
                        collection_disposition=e.removal_reason==ProbeRemovalReason::prune_completed?
                            "terminal-prune-completed":e.removal_reason==ProbeRemovalReason::notify_rollback?
                            "terminal-notification-rollback":"terminal-retirement-clear";
                        terminal=true;break;
                    }
                    if(e.target_mutated) {
                        collection_disposition="incomplete-canceled-target-mutated";terminal=true;break;
                    }
                    if(canceled_front_pop(e)) {
                        collection_disposition="terminal-front-pop-post-pump";terminal=true;break;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if(!terminal)collection_disposition=!saw_usable && last_read==ProbeEvidenceRead::busy?
                "incomplete-persistent-observer-busy":last_valid_available && snapshot.evidence.removal_sequence?
                "incomplete-post-pump-deadline":"incomplete-removal-deadline";
            finalize(end); // same nonrenewable budget; preserve collection disposition and last-valid bytes
        }catch(...){collection_disposition="incomplete-secondary-diagnostic-exception";finalization_disposition="secondary-exception";}
    }
    void emit() const noexcept {
        try {
            std::cerr<<"FAILURE-SCOPE interval_failed="<<interval_failed<<" collector_exercised="<<collector_exercised
                <<" stage="<<stage<<" original_hresult="<<actual_result<<" original_elapsed_ms="<<actual_elapsed
                <<" collection_disposition="<<collection_disposition<<" finalization_disposition="<<finalization_disposition
                <<" last_read="<<static_cast<unsigned>(last_read)<<" last_valid_available="<<last_valid_available
                <<" expected_generation="<<expected_generation<<" final_copy="<<final_copy<<" disarmed="<<disarmed<<'\n'
                <<probe_format_epoch_snapshot(snapshot);
        }catch(...){std::cerr<<"INCOMPLETE secondary diagnostic output; primary failure preserved\n";}
    }
};
void retained_evidence_contracts() {
    FocusFailureContext context;context.epoch=7;
    ProbeEpochSnapshot valid;valid.epoch=7;valid.evidence.valid=true;valid.evidence.trace_complete=true;
    valid.evidence.generation=9;valid.evidence.target_token=42;
    check(context.adopt(valid),"last valid snapshot adopted");
    auto bad=valid;bad.epoch=8;
    check(!context.retain_read(ProbeEvidenceRead::busy,bad) && context.snapshot.evidence.target_token==42 &&
          context.last_valid_available && context.last_read==ProbeEvidenceRead::busy,"busy read preserves last valid bytes");
    check(!context.retain_read(ProbeEvidenceRead::invalid,bad) && context.snapshot.epoch==7 &&
          context.last_read==ProbeEvidenceRead::invalid,"invalid read preserves last valid bytes");
    check(!context.retain_read(ProbeEvidenceRead::copied,bad) && context.snapshot.epoch==7,"invalid copy preserves last valid bytes");
    bad=valid;bad.evidence.generation=10;
    check(!context.adopt(bad) && context.expected_generation==9,"pinned generation preserved");
    bad=valid;bad.evidence.trace_complete=false;
    check(!context.adopt(bad) && context.snapshot.evidence.trace_complete,"overflow copy preserves last valid bytes");
    bad=valid;bad.evidence.valid=false;
    check(!context.adopt(bad) && context.snapshot.evidence.valid,"closed copy preserves last valid bytes");
    context.collection_disposition="incomplete-post-pump-deadline";
    context.finalize(Clock::time_point::min()); // no bridge call when budget already exhausted
    check(context.collection_disposition==std::string_view("incomplete-post-pump-deadline") &&
          context.finalization_disposition==std::string_view("budget-exhausted"),"collection and finalization reasons preserved independently");
}

void tests(HWND window, FocusFailureContext& diagnosis, bool controlled_failure_capture) {
    const auto overall = Clock::now();
    Subscriptions subscriptions;
    success(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(subscriptions.client.GetAddressOf())), "create OS UIA client");
    auto& client = subscriptions.client;
    success(client->ElementFromHandle(window, &subscriptions.root), "OS ElementFromHandle own HWND");
    auto& root = subscriptions.root;
    check(name(root.Get()) == L"NSTU UI Automation feasibility", "OS custom provider name (original RED assertion)");
    ComPtr<IUIAutomationCondition> condition; success(client->CreateTrueCondition(&condition), "own subtree condition");
    ComPtr<IUIAutomationElementArray> children; success(root->FindAll(TreeScope_Children, condition.Get(), &children), "own subtree children only");
    int length{}; success(children->get_Length(&length), "child count");
    std::cout << "OS raw child count=" << length << '\n';
    ComPtr<IUIAutomationElement> command, edit;
    // HWND caption proxies can coexist; select ONLY explicit own automation IDs.
    for (int i = 0; i < length; ++i) {
        ComPtr<IUIAutomationElement> child; success(children->GetElement(i, &child), "own child");
        BSTR id{}; success(child->get_CurrentAutomationId(&id), "own automation ID");
        const std::wstring text(id ? id : L"", SysStringLen(id)); SysFreeString(id);
        if (text == L"probe.command") command = child;
        if (text == L"probe.value") edit = child;
    }
    check(command && edit, "own command/value nodes exposed to OS");
    check(name(command.Get()) == L"Lệnh thử an toàn" && name(edit.Get()) == L"Giá trị tiếng Việt", "Vietnamese OS names");
    CONTROLTYPEID type{}; success(command->get_CurrentControlType(&type), "command role"); check(type == UIA_ButtonControlTypeId, "button role");
    success(edit->get_CurrentControlType(&type), "value role"); check(type == UIA_EditControlTypeId, "edit role");
    const auto command_id = runtime_id(command.Get()), edit_id = runtime_id(edit.Get());
    check(command_id != edit_id && edit_id == runtime_id(edit.Get()), "stable distinct OS IDs");
    ComPtr<IUIAutomationTreeWalker> walker; success(client->get_RawViewWalker(&walker), "raw walker");
    ComPtr<IUIAutomationElement> parent, sibling;
    success(walker->GetParentElement(edit.Get(), &parent), "own child parent"); BOOL same{};
    success(client->CompareElements(parent.Get(), root.Get(), &same), "compare own parent"); check(same, "own root parent identity");
    success(walker->GetNextSiblingElement(command.Get(), &sibling), "own sibling");
    success(client->CompareElements(sibling.Get(), edit.Get(), &same), "compare own sibling"); check(same, "command then value sibling");
    ComPtr<IUIAutomationInvokePattern> os_invoke; ComPtr<IUIAutomationValuePattern> os_value;
    success(command->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&os_invoke)), "OS Invoke pattern");
    success(edit->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&os_value)), "OS Value pattern");
    ComPtr<IUnknown> absent;
    const auto absent_result = edit->GetCurrentPattern(UIA_TextPatternId, &absent);
    std::cout << "OS unsupported TextPattern HRESULT=0x" << std::hex << static_cast<unsigned long>(absent_result) << std::dec << '\n';
    check(!absent, "TextPattern honestly absent");
    auto sink = std::make_unique<Events>(); subscriptions.events.Attach(sink.release());
    success(client->AddAutomationEventHandler(UIA_Invoke_InvokedEventId, root.Get(), TreeScope_Subtree, nullptr, subscriptions.events.Get()), "own subtree Invoke subscription"); subscriptions.invokes = true;
    std::array<PROPERTYID, 2> properties{UIA_ValueValuePropertyId, UIA_HasKeyboardFocusPropertyId};
    success(client->AddPropertyChangedEventHandlerNativeArray(root.Get(), TreeScope_Subtree, nullptr,
        subscriptions.events.Get(), properties.data(), static_cast<int>(properties.size())), "own subtree property subscription"); subscriptions.properties = true;
    success(os_invoke->Invoke(), "OS harmless Invoke"); wait_for([&] { return subscriptions.events->invokes.load() > 0; }, "observed OS Invoke event");
    const std::array<std::wstring, 4> samples{L"Tiếng Việt ế ợ", L"Tie\u0302\u0301ng Vie\u0323\u0302t", L"Code \U0001F642", L""};
    for (const auto& text : samples) {
        auto allocated = SysAllocStringLen(text.data(), static_cast<UINT>(text.size()));
        check(allocated != nullptr, "allocate OS client BSTR");
        HRESULT result{};
        {
            ActorScope os_call(window,diagnosis.epoch,10,ProbeActorKind::os_value_client);
            result = os_value->SetValue(allocated);os_call.result(result);
        }
        SysFreeString(allocated);
        success(result, "OS Unicode SetValue");
        {
            ActorScope os_query(window,diagnosis.epoch,11,ProbeActorKind::os_value_client,ProbeActorStage::query_begin);
            check(value(os_value.Get()) == text, "OS exact UTF16 round trip");os_query.result(S_OK);
        }
    }
    wait_for([&] { return subscriptions.events->values.load() > 0; }, "observed OS Value property event");
    std::cout << "PASS OS: provider names/roles/IDs/navigation/Invoke/Value/Unicode/events\n";
    // Direct-provider tests supplement OS client results, never substitute for AT.
    ComPtr<IRawElementProviderSimple> raw_root, raw_edit;
    ComPtr<IRawElementProviderFragmentRoot> fragment_root;
    ComPtr<IRawElementProviderFragment> fragment_edit;
    ComPtr<IValueProvider> direct_value; ComPtr<IInvokeProvider> direct_invoke;
    success(probe_provider(window, 0, IID_PPV_ARGS(&raw_root)), "retain direct root");
    success(probe_provider(window, 2, IID_PPV_ARGS(&raw_edit)), "retain direct child");
    success(probe_provider(window, 0, IID_PPV_ARGS(&fragment_root)), "direct fragment root");
    success(probe_provider(window, 2, IID_PPV_ARGS(&fragment_edit)), "direct fragment edit");
    success(probe_provider(window, 2, IID_PPV_ARGS(&direct_value)), "direct Value");
    success(probe_provider(window, 1, IID_PPV_ARGS(&direct_invoke)), "direct Invoke");
    ProviderOptions options{}; success(raw_root->get_ProviderOptions(&options), "provider MTA options");
    check(options == static_cast<ProviderOptions>(ProviderOptions_ServerSideProvider | ProviderOptions_UseComThreading), "explicit supported provider COM model");
    ComPtr<IRawElementProviderFragment> root_fragment; success(raw_root.As(&root_fragment), "root fragment");
    SAFEARRAY* no_id{}; success(root_fragment->GetRuntimeId(&no_id), "hosted root ID"); check(!no_id, "hosted root ID null");
    UiaRect bounds{}; success(fragment_edit->get_BoundingRectangle(&bounds), "direct bounds");
    check(bounds.width > 0 && bounds.height > 0, "nonempty bounds");
    RECT os_bounds{}; success(edit->get_CurrentBoundingRectangle(&os_bounds), "OS bounds");
    check(os_bounds.left == bounds.left && os_bounds.top == bounds.top, "OS/direct screen bounds agree");
    ComPtr<IRawElementProviderFragment> hit;
    success(fragment_root->ElementProviderFromPoint(bounds.left + 2, bounds.top + 2, &hit), "own fragment hit test");
    ComPtr<IUnknown> hit_id, edit_identity; success(hit.As(&hit_id), "hit identity"); success(fragment_edit.As(&edit_identity), "edit identity");
    check(hit_id.Get() == edit_identity.Get(), "hit selects exact value node");
    success(probe_control(window, 3, 0), "own UI thread release input focus");
    ComPtr<IRawElementProviderFragment> focus;
    success(fragment_root->GetFocus(&focus), "fragment no input focus"); check(!focus, "actual host focus absent => no fragment focus");
    check(fragment_edit->SetFocus() == invalid, "direct SetFocus rejects host not focused");
    success(probe_control(window, 3, 1), "own UI thread focus only own HWND");
    success(fragment_edit->SetFocus(), "direct internal focus application");
    success(fragment_root->GetFocus(&focus), "actual focused fragment");
    ComPtr<IUnknown> focused_identity; success(focus.As(&focused_identity), "focused identity");
    check(focused_identity.Get() == edit_identity.Get(), "own input focus/internal node agree");
    BOOL focused{}; success(edit->get_CurrentHasKeyboardFocus(&focused), "OS edit focus property"); check(focused, "OS focus property actual");
    ComPtr<IRawElementProviderFragment> fragment_command;
    success(probe_provider(window, 1, IID_PPV_ARGS(&fragment_command)), "direct command focus");
    success(fragment_command->SetFocus(), "focus transfer to command");
    success(command->get_CurrentHasKeyboardFocus(&focused), "OS command focus"); check(focused, "command focus transfer actual");
    success(probe_control(window, 3, 0), "own focus loss"); focus.Reset();
    success(fragment_root->GetFocus(&focus), "focus loss query"); check(!focus, "focus loss does not retain requested ID as real focus");
    try {
    diagnosis.stage="baseline-before-latch";
    success(probe_control(window, 3, 1), "restore own host for focus race");
    check(probe_observe(window, 5) == 1, "command identity established before cancellation latch");
    probe_mark_focus(window, 1);
    success(probe_latch(window, true, false), "hold focus timeout");
    diagnosis.stage="target-completion";
    auto target=tagged_focus(window,fragment_edit,diagnosis.epoch,1);
    if(controlled_failure_capture) {
        wait_for([&] {
            ProbeEpochSnapshot temporary;
            diagnosis.last_read=probe_try_epoch_snapshot(window,diagnosis.epoch,1,temporary);
            return diagnosis.last_read==ProbeEvidenceRead::copied && diagnosis.adopt(temporary) &&
                temporary.evidence.target_token!=0 && temporary.evidence.target_present && temporary.evidence.held &&
                temporary.evidence.target_completion==0;
        },"controlled failure exact held target admitted",500);
        ActorScope competitor(window,diagnosis.epoch,2,ProbeActorKind::fixture_control);
        const auto result=probe_control(window,8,2);competitor.result(result);
        success(result,"controlled failure legitimate competitor");
    }
    const auto timed_focus=target.get();
    diagnosis.completed(timed_focus);
    bounded(timed_focus, timeout, "direct focus timeout cancellation");
    probe_mark_focus(window, 2);
    diagnosis.stage="latch-release";
    success(probe_latch(window, false, false), "release timed-out focus");
    diagnosis.released=true;
    diagnosis.stage="original-scalar-assertion";
    probe_actor_mark(window,diagnosis.epoch,1,ProbeActorKind::fixture_control,ProbeActorStage::assertion_begin);
    check(probe_observe(window, 5) == 1, "timed-out focus never changed internal command identity");
    check(probe_focus_cancellation_proven(window), "same canceled focus token has no mutation trace between causal latch markers");
    success(edit->get_CurrentHasKeyboardFocus(&focused), "edit focus absent after cancellation");
    check(!focused, "timed-out focus never applied to edit even if host loses focus");
    probe_actor_mark(window,diagnosis.epoch,1,ProbeActorKind::fixture_control,ProbeActorStage::assertion_return);
    diagnosis.stage="post-assertion-actual-pop";
    diagnosis.observe();
    check(diagnosis.generation_pinned,"ordinary diagnostic generation pinned");
    diagnosis.adopt(terminal_pop(window,diagnosis.epoch,1,diagnosis.expected_generation,diagnosis.snapshot));
    diagnosis.freeze_success();
    } catch(...) {
        diagnosis.interval_failed=true;diagnosis.primary=std::current_exception();
        diagnosis.collect_failure();
        throw; // exact original exception; retained COM owners unwind only now
    }
    success(probe_latch(window, true, false), "hold focus loss race");
    auto focus_race = pending_focus(fragment_edit);
    wait_for([&] { return probe_observe(window, 1) == 1; }, "focus request pending", 500);
    success(probe_control(window, 3, 0), "lose actual own host focus before dispatch");
    success(probe_latch(window, false, false), "release focus loss race");
    bounded(focus_race.get(), invalid, "direct focus loss race failure");
    success(probe_control(window, 6, 0), "own UI-thread direct dispatch without self wait");
    check(value(direct_value.Get()) == L"own UI direct dispatch", "own UI direct path actually applied");
    check(direct_value->SetValue(nullptr) == E_INVALIDARG, "null value rejection");
    const std::wstring malformed(1, static_cast<wchar_t>(0xD800));
    check(direct_value->SetValue(malformed.c_str()) == E_INVALIDARG, "unpaired UTF16 rejection");
    check(direct_value->SetValue(std::wstring(1025, L'a').c_str()) == E_INVALIDARG, "value bound rejection");
    check(direct_value->SetValue(L"line\nbreak") == E_INVALIDARG, "single-line contract");
    success(direct_value->SetValue(std::wstring(1024, L'a').c_str()), "exact text limit");
    success(direct_value->SetValue(L"unchanged baseline"), "race baseline");
    success(probe_control(window, 1, 1), "read-only state");
    check(direct_value->SetValue(L"must reject") == invalid, "read-only immediate failure");
    BOOL read_only{}; success(os_value->get_CurrentIsReadOnly(&read_only), "OS read-only state"); check(read_only, "OS read-only property truthful");
    success(probe_control(window, 1, 0), "restore editable");
    success(probe_control(window, 2, 0), "disabled state"); check(direct_invoke->Invoke() == disabled, "disabled immediate Invoke rejection");
    success(command->get_CurrentIsEnabled(&focused), "OS enabled state"); check(!focused, "OS disabled property truthful");
    success(probe_control(window, 2, 1), "restore enabled");
    success(probe_latch(window, false, true), "notification fault injection");
    check(FAILED(direct_value->SetValue(L"post failure")), "post failure cannot return success");
    check(value(direct_value.Get()) == L"unchanged baseline", "post failure no mutation");
    success(probe_latch(window, true, false), "hold dispatch for timeout");
    bounded(pending_value(direct_value, L"expired mutation").get(), timeout, "direct timeout cancellation");
    success(probe_latch(window, false, false), "release after timeout");
    wait_for([&] { return probe_observe(window, 1) == 0; }, "expired request retired");
    check(value(direct_value.Get()) == L"unchanged baseline", "canceled timeout never applies later");
    success(probe_latch(window, true, false), "hold read-only race");
    auto read_only_race = pending_value(direct_value, L"race must not apply");
    wait_for([&] { return probe_observe(window, 1) == 1; }, "Value pending before read-only change", 500);
    success(probe_control(window, 1, 1), "read-only after acceptance");
    success(probe_latch(window, false, false), "dispatch revalidation");
    bounded(read_only_race.get(), invalid, "direct read-only race failure");
    check(value(direct_value.Get()) == L"unchanged baseline", "read-only race retains value");
    success(probe_control(window, 1, 0), "restore read-only race");
    success(probe_latch(window, true, false), "hold disabled Value race");
    auto disabled_value_race = pending_value(direct_value, L"disabled pending value");
    wait_for([&] { return probe_observe(window, 1) == 1; }, "disabled Value request pending", 500);
    success(probe_control(window, 7, 0), "disable Value after acceptance");
    success(probe_latch(window, false, false), "release disabled Value");
    bounded(disabled_value_race.get(), disabled, "direct disabled Value race failure");
    check(value(direct_value.Get()) == L"unchanged baseline", "disabled Value race retains value");
    success(probe_control(window, 7, 1), "restore Value enabled state");
    success(probe_latch(window, true, false), "hold generation race");
    auto generation_race = pending_value(direct_value, L"retired generation");
    wait_for([&] { return probe_observe(window, 1) == 1; }, "generation request pending", 500);
    success(probe_control(window, 4, 0), "retire pending request generation");
    success(probe_latch(window, false, false), "release generation race");
    bounded(generation_race.get(), unavailable, "direct stale generation failure");
    check(edit_id == runtime_id(edit.Get()), "semantic runtime identity survives canceled action");
    success(probe_latch(window, true, false), "hold asynchronous command");
    const auto invoke_start = Clock::now(); const auto count = probe_observe(window, 2);
    success(direct_invoke->Invoke(), "async command accepted");
    const auto invoke_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - invoke_start).count();
    std::cout << "direct Invoke elapsed_ms=" << invoke_elapsed << '\n'; check(invoke_elapsed <= 250, "Invoke returns immediately");
    check(probe_observe(window, 2) == count, "held command not applied before dispatch");
    success(probe_control(window, 2, 0), "disable queued command");
    success(probe_latch(window, false, false), "release disabled command");
    wait_for([&] { return probe_observe(window, 1) == 0; }, "disabled command retired");
    check(probe_observe(window, 2) == count, "queued Invoke rechecks enabled guard");
    success(probe_control(window, 2, 1), "restore command guard");
    success(probe_latch(window, true, false), "hold full queue");
    for (int i = 0; i < 32; ++i) success(direct_invoke->Invoke(), "fill bounded queue");
    check(direct_invoke->Invoke() == E_PENDING, "33rd command rejected");
    check(direct_value->SetValue(L"full queue") == E_PENDING, "Value queue rejection is failure");
    success(probe_latch(window, false, false), "drain full queue");
    wait_for([&] { return probe_observe(window, 2) == count + 32; }, "real UI-thread command application");
    // Queries on another MTA are limited to retained own provider interfaces.
    auto query = std::async(std::launch::async, [raw_edit] {
        Mta mta; success(mta.result, "query MTA");
        for (int i = 0; i < 100; ++i) { VARIANT property{}; success(raw_edit->GetPropertyValue(UIA_NamePropertyId, &property), "concurrent own snapshot"); VariantClear(&property); }
    });
    success(direct_value->SetValue(L"concurrent value"), "concurrent UI mutation"); query.get();
    check(Clock::now() - overall < std::chrono::seconds(60), "overall test deadline before lifecycle");
    std::cout << "PASS direct contracts: bounds/hit/focus/guards/queue/cancellation/concurrent snapshots\n";
    success(probe_latch(window, true, false), "hold destruction Value wait");
    auto closing_race = pending_value(direct_value, L"destroyed pending value");
    wait_for([&] { return probe_observe(window, 1) == 1; }, "destruction Value pending", 500);
    success(probe_control(window, 5, 0), "request own HWND destruction");
    bounded(closing_race.get(), unavailable, "direct destruction wakes Value wait");
    wait_for([&] { return probe_observe(window, 3) && probe_observe(window, 4); }, "own HWND invalidation and non-input-sync disconnect");
    VARIANT retired_property{};
    check(raw_root->GetPropertyValue(UIA_NamePropertyId, &retired_property) == unavailable, "retained root unavailable");
    check(raw_edit->GetPropertyValue(UIA_NamePropertyId, &retired_property) == unavailable, "retained child unavailable");
    ComPtr<IUnknown> retired_pattern;
    check(raw_edit->GetPatternProvider(UIA_ValuePatternId, &retired_pattern) == unavailable, "retained pattern unavailable");
    check(direct_value->SetValue(L"after close") == unavailable, "retained action unavailable");
    check(fragment_edit->SetFocus() == unavailable && direct_invoke->Invoke() == unavailable, "retained focus/invoke unavailable");
    BSTR retired_name{}; check(FAILED(root->get_CurrentName(&retired_name)), "OS root invalid after real destruction"); SysFreeString(retired_name);
    BSTR retired_value{}; check(FAILED(os_value->get_CurrentValue(&retired_value)), "OS value invalid after real destruction"); SysFreeString(retired_value);
    success(subscriptions.remove(), "originating MTA removes exact own subscriptions");
    check(Clock::now() - overall < std::chrono::seconds(60), "overall 60-second worker bound");
    std::cout << "PASS lifecycle: real own HWND destroyed, pending Value failed, providers disconnected, retained OS/direct interfaces rejected, subscriptions removed\n";
}
}
int run_focus_evidence_contract_tests() {
    try {
        fixed_evidence_contracts();retained_evidence_contracts();
        std::cout<<"FIXED-DATA contracts PASS; pure adjudication only; Windows locking and historical cause unqualified.\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<"FAIL fixed-data contracts: "<<error.what()<<'\n';return 1;}
}
int run_probe_tests(HWND window, bool controlled_failure_capture) {
    Mta mta;
    if (FAILED(mta.result)) { std::cerr << "FAIL: initial MTA\n"; return 2; }
    FocusFailureContext diagnosis;diagnosis.window=window;
    diagnosis.epoch=probe_begin_focus_epoch(window);diagnosis.observe();
    try {
        tests(window,diagnosis,controlled_failure_capture);
        std::cout<<"DIAGNOSTIC ordinary collector_exercised=0; failure branch unqualified; historical cause UNKNOWN.\n"
                 <<probe_format_epoch_snapshot(diagnosis.snapshot);
        std::cout << "PASS: standalone UIA probe only; actual NSTU / AT / TextPattern / IME / DPI remain NOT TESTED\n"; return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        diagnosis.emit(); // frozen/incomplete interval snapshot precedes original cleanup
        if(controlled_failure_capture) {
            bool competitor_valid=false;std::uint64_t cancellation{};
            const auto& s=diagnosis.snapshot;
            for(unsigned i=0;i<s.count;++i)if(s.records[i].token==s.evidence.target_token && s.records[i].phase==5)cancellation=s.records[i].sequence;
            for(unsigned i=0;i<s.count;++i) {
                const auto& r=s.records[i];
                if(r.epoch==diagnosis.epoch && r.generation==diagnosis.expected_generation &&
                   r.actor==2 && r.origin==ProbeActorKind::fixture_control && r.token!=s.evidence.target_token &&
                   r.action==2 && r.phase==6 && r.held && !r.expired && r.result==S_OK &&
                   r.before==1 && r.after==2 && r.sequence<cancellation) {
                    for(unsigned j=0;j<s.count;++j) {
                        const auto& completion=s.records[j];
                        if(completion.token==r.token && completion.actor==2 && completion.phase==4 &&
                           completion.epoch==diagnosis.epoch && completion.generation==diagnosis.expected_generation &&
                           completion.completion==1 && completion.result==S_OK && completion.held && !completion.expired &&
                           completion.sequence>r.sequence && completion.sequence<cancellation)competitor_valid=true;
                    }
                }
            }
            const bool complete=std::string_view(error.what())=="timed-out focus never changed internal command identity" &&
                diagnosis.interval_failed && diagnosis.collector_exercised && diagnosis.returned && diagnosis.released &&
                diagnosis.actual_result==timeout && diagnosis.actual_elapsed<=1750 &&
                diagnosis.collection_disposition==std::string_view("terminal-front-pop-post-pump") &&
                diagnosis.finalization_disposition==std::string_view("confirmed") &&
                valid_final(s,diagnosis.epoch,diagnosis.expected_generation) && canceled_front_pop(s.evidence) &&
                competitor_valid && !s.evidence.held && s.evidence.semantic_focus==2;
            std::cerr<<"CONTROLLED-FAILURE adjudication="<<(complete?"EXPECTED_CONTROLLED_ERROR":"INCONCLUSIVE")
                <<" valid_competitor="<<competitor_valid<<" original_nonzero_preserved=1; historical cause UNKNOWN.\n";
        }
        std::cerr << "Own failure context: internal_focus=" << probe_observe(window, 5)
            << ", closing=" << probe_observe(window, 3) << ", pending=" << probe_observe(window, 1) << '\n';
        std::cerr << probe_focus_trace(window);
        if (!probe_observe(window, 3)) {
            const auto latch_cleanup=probe_latch(window,false,false),close_cleanup=probe_control(window,5,0);
            if(FAILED(latch_cleanup)||FAILED(close_cleanup))std::cerr<<"SECONDARY cleanup failure; primary remains nonzero\n";
        }
        return 1;
    }
}
int run_fault_tests() {
    try {
        for (int fault = 1; fault <= 11; ++fault) {
            const auto result = probe_fault_scenario(fault);
            std::cout << "Fault " << fault << ": status=" << result.status << ", created=" << result.created
                << ", retired=" << result.retired << ", registry_clear=" << result.registry_cleared
                << ", elapsed_ms=" << result.elapsed_ms << '\n';
            check(result.status != 0, "injected host failure must report failure");
            check(result.created == 5 && result.retired == 5 && result.remaining == 0, "all five fault-scenario providers retired");
            check(result.window_retired && result.registry_cleared && result.worker_retired && result.before_com_uninitialize,
                "fault shutdown clears window, registry, worker and COM refs before apartment exit");
            check(result.elapsed_ms <= 5000, "fault shutdown five-second bound");
        }
        std::cout << "PASS: eleven real own-host lifecycle fault paths retired before apartment exit\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
int run_fault_worker(HWND window) {
    Mta mta;
    if (FAILED(mta.result)) return 2;
    try {
        Subscriptions subscriptions;
        success(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(subscriptions.client.GetAddressOf())), "fault actor OS client");
        success(subscriptions.client->ElementFromHandle(window, &subscriptions.root), "fault actor exact own HWND");
        check(name(subscriptions.root.Get()) == L"NSTU UI Automation feasibility", "fault actor actual OS provider");
        auto sink = std::make_unique<Events>(); subscriptions.events.Attach(sink.release());
        std::array<PROPERTYID, 1> properties{UIA_ValueValuePropertyId};
        success(subscriptions.client->AddPropertyChangedEventHandlerNativeArray(subscriptions.root.Get(), TreeScope_Subtree,
            nullptr, subscriptions.events.Get(), properties.data(), 1), "fault actor scoped MTA subscription");
        subscriptions.properties = true;
        ComPtr<IValueProvider> value_provider;
        success(probe_provider(window, 2, IID_PPV_ARGS(&value_provider)), "fault actor own Value");
        const auto start = Clock::now();
        const auto result = value_provider->SetValue(L"host failure must cancel this value");
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
        check(result == unavailable && elapsed <= 1750, "fault shutdown aborts actual Value wait in bound");
        success(subscriptions.remove(), "fault actor originating MTA unsubscribe");
        std::cout << "Fault actor: pending Value aborted, MTA unsubscribed, elapsed_ms=" << elapsed << '\n';
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL fault actor: " << error.what() << '\n'; return 1; }
}
int run_regression_tests(int scenario) {
    try {
        const auto label = scenario == 1 ? "fresh worker budget after simulated 61-second idle" :
            scenario == 2 ? "aborted State construction closes every own acquired event" :
            scenario == 3 ? "own state query proceeds while diagnostic sink is withheld" :
            "immutable cached metadata and delayed provider/State completed retirement";
        check(probe_regression(scenario), label);
        std::cout << "PASS regression: " << label << '\n'; return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL regression: " << error.what() << '\n'; return 1; }
}
int run_competing_focus(HWND window) {
    Mta mta;
    if (FAILED(mta.result)) return 2;
    const auto epoch=probe_begin_focus_epoch(window);
    unsigned generation{};ProbeEpochSnapshot retained;
    try {
        ComPtr<IRawElementProviderFragment> command, edit;
        success(probe_provider(window, 1, IID_PPV_ARGS(&command)), "controlled own command provider");
        success(probe_provider(window, 2, IID_PPV_ARGS(&edit)), "controlled own edit provider");
        success(probe_control(window, 3, 1), "controlled actual own host focus");
        success(command->SetFocus(), "controlled command baseline");
        check(probe_observe(window, 5) == 1, "controlled baseline command1");
        check(probe_try_epoch_snapshot(window,epoch,1,retained)==ProbeEvidenceRead::copied &&
              valid_identity(retained,epoch,retained.evidence.generation),"competitor valid baseline generation");
        generation=retained.evidence.generation;
        probe_mark_focus(window, 1);
        success(probe_latch(window, true, false), "controlled focus latch");
        auto canceled = tagged_focus(window,edit,epoch,1);
        wait_for([&] { return probe_observe(window, 1) == 1; }, "controlled focus request queued", 500);
        { ActorScope competing(window,epoch,2,ProbeActorKind::fixture_control);
          const auto result=probe_control(window,8,2);competing.result(result);
          success(result, "competing valid UI-thread direct edit focus while held"); }
        check(probe_observe(window, 5) == 2, "competing valid action applied before cancellation");
        bounded(canceled.get(), timeout, "controlled held request timeout");
        probe_mark_focus(window, 2);
        success(probe_latch(window, false, false), "controlled resume dispatch");
        wait_for([&] { return probe_observe(window, 1) == 0; }, "controlled canceled request drained");
        const auto terminal=terminal_pop(window,epoch,1,generation,retained);
        check(probe_observe(window, 5) == 2, "valid competitor focus remains after canceled request drains");
        check(probe_focus_cancellation_proven(window), "controlled expired token never mutates focus");
        std::cout << "CONTROLLED scalar fixture assumption fails: baseline1 -> valid competitor2; canceled token has no mutation. Historical cause NOT ESTABLISHED.\n"
            << probe_focus_trace(window);
        finalize_fixture(window,epoch,generation,retained,"competitor final copy/disarm");
        check(terminal.evidence.other_valid_mutation,"distinct valid competing token observed");
        check(retained.evidence.other_valid_mutation,"final valid competing token retained");
        std::cout<<probe_format_epoch_snapshot(retained);
        success(probe_control(window, 5, 0), "controlled own host cleanup");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL controlled focus: " << error.what() << '\n' << probe_focus_trace(window)
                  <<probe_format_epoch_snapshot(retained);
        if (!probe_observe(window, 3)) { probe_latch(window, false, false); probe_control(window, 5, 0); }
        return 1;
    }
}

int run_isolated_focus(HWND window) {
    Mta mta; if(FAILED(mta.result))return 2;
    ProbeEpochSnapshot retained;
    try {
        const auto epoch=probe_begin_focus_epoch(window); check(epoch!=0,"isolated diagnostic epoch");
        ComPtr<IRawElementProviderFragment> command,edit;
        success(probe_provider(window,1,IID_PPV_ARGS(&command)),"isolated own command");
        success(probe_provider(window,2,IID_PPV_ARGS(&edit)),"isolated own edit");
        success(probe_control(window,3,1),"isolated own host focus");
        success(command->SetFocus(),"isolated baseline command");
        ProbeEpochSnapshot initial;
        check(probe_try_epoch_snapshot(window,epoch,1,initial)==ProbeEvidenceRead::copied &&
              initial.evidence.ordinary_total==0,"isolated physical queue empty at baseline");
        const auto generation=initial.evidence.generation;
        check(valid_identity(initial,epoch,generation) && initial.evidence.semantic_focus==1 &&
              initial.evidence.host_focused && initial.armed,"isolated valid focused baseline");
        retained=initial;
        check(probe_observe(window,5)==1,"command identity established before cancellation latch");
        probe_mark_focus(window,1);
        success(probe_latch(window,true,false),"hold isolated focus timeout");
        ProbeEpochSnapshot latch;
        check(probe_try_epoch_snapshot(window,epoch,1,latch)==ProbeEvidenceRead::copied &&
              valid_identity(latch,epoch,generation) && latch.armed && latch.evidence.held &&
              latch.evidence.ordinary_total==0 && latch.evidence.semantic_focus==1 && latch.evidence.host_focused,
              "isolated valid focused empty latch");
        retained=latch;
        auto pending=tagged_focus(window,edit,epoch,1);
        wait_for([&]{ProbeEpochSnapshot s; return probe_try_epoch_snapshot(window,epoch,1,s)==ProbeEvidenceRead::copied &&
                     valid_identity(s,epoch,generation) && s.evidence.target_present && s.evidence.ordinary_pending==1;
        },"exact isolated target admitted",500);
        bounded(pending.get(),timeout,"isolated direct focus timeout cancellation");
        probe_mark_focus(window,2);
        success(probe_latch(window,false,false),"release isolated timed-out focus");
        retained=terminal_pop(window,epoch,1,generation,retained);
        probe_actor_mark(window,epoch,1,ProbeActorKind::fixture_control,ProbeActorStage::assertion_begin);
        check(probe_observe(window,5)==1,"timed-out focus never changed internal command identity");
        probe_actor_mark(window,epoch,1,ProbeActorKind::fixture_control,ProbeActorStage::assertion_return);
        finalize_fixture(window,epoch,generation,retained,"isolated final copy/disarm");
        const auto& snapshot=retained;
        check(!snapshot.evidence.other_valid_mutation,"isolated fixture has no competing focus mutation");
        check(!snapshot.evidence.unplanned_action,"isolated fixture no unplanned action or input");
        check(isolated_final(snapshot,epoch,generation),"isolated complete focused uncontaminated final interval");
        std::cout<<"ISOLATED original stable-focus assertion retained; actual-pop proof only; history UNKNOWN.\n"
                 <<probe_format_epoch_snapshot(retained);
        success(probe_control(window,5,0),"isolated own cleanup");
        return 0;
    } catch(const std::exception& error) {
        std::cerr<<"FAIL isolated: "<<error.what()<<'\n'<<probe_format_epoch_snapshot(retained);
        if(!probe_observe(window,3)){probe_latch(window,false,false);probe_control(window,5,0);}
        return 1;
    }
}


int run_os_interleave(HWND window) {
    Mta mta;if(FAILED(mta.result))return 2;
    const auto epoch=probe_begin_focus_epoch(window);
    unsigned generation{};bool generation_pinned=false;ProbeEpochSnapshot retained;
    std::atomic<bool> ready{},go{},done{};std::atomic<HRESULT> os_result{E_PENDING};
    try {
        std::jthread os_actor([&](std::stop_token stop){
            Mta apartment;
            try{
                success(apartment.result,"OS interleave MTA");
                ComPtr<IUIAutomation> client;success(CoCreateInstance(CLSID_CUIAutomation,nullptr,CLSCTX_INPROC_SERVER,
                    IID_PPV_ARGS(&client)),"controlled real OS client");
                ComPtr<IUIAutomationElement> root;success(client->ElementFromHandle(window,&root),"OS exact own HWND");
                ComPtr<IUIAutomationCondition> condition;success(client->CreateTrueCondition(&condition),"OS own children condition");
                ComPtr<IUIAutomationElementArray> children;success(root->FindAll(TreeScope_Children,condition.Get(),&children),"OS own children only");
                int count=0;success(children->get_Length(&count),"OS own child count");
                ComPtr<IUIAutomationValuePattern> value_pattern;
                for(int i=0;i<count;++i){
                    ComPtr<IUIAutomationElement> child;success(children->GetElement(i,&child),"OS own child");
                    BSTR id=nullptr;success(child->get_CurrentAutomationId(&id),"OS own ID");
                    const std::wstring identity(id?id:L"",SysStringLen(id));SysFreeString(id);
                    if(identity==L"probe.value")success(child->GetCurrentPatternAs(UIA_ValuePatternId,IID_PPV_ARGS(&value_pattern)),"OS Value pattern");
                }
                check(value_pattern!=nullptr,"OS exact own edit pattern");
                probe_actor_mark(window,epoch,3,ProbeActorKind::os_value_client,ProbeActorStage::ready);
                ready=true;
                while(!go.load()&&!stop.stop_requested())std::this_thread::sleep_for(std::chrono::milliseconds(1));
                if(go.load()){
                    ActorScope call(window,epoch,3,ProbeActorKind::os_value_client);
                    struct OwnedBstr {
                        BSTR value{SysAllocString(L"controlled OS client interleave")};
                        ~OwnedBstr(){SysFreeString(value);}
                    } text;
                    check(text.value!=nullptr,"OS actor owns actual BSTR");
                    const auto hr=value_pattern->SetValue(text.value);
                    call.result(hr);os_result=hr;
                }
                value_pattern.Reset();children.Reset();condition.Reset();root.Reset();client.Reset();
            }catch(...){os_result=E_FAIL;ready=true;}
            done=true; // originating MTA RAII retires at lambda return; join before fixture cleanup
        });
        wait_for([&]{return ready.load();},"controlled OS actor prepared",2000);
        check(!done.load(),"controlled OS setup actually ready");
        ComPtr<IRawElementProviderFragment> command,edit;
        success(probe_provider(window,1,IID_PPV_ARGS(&command)),"OS-trial own command");
        success(probe_provider(window,2,IID_PPV_ARGS(&edit)),"OS-trial own edit");
        success(probe_control(window,3,1),"OS-trial own host focus");
        success(command->SetFocus(),"OS-trial baseline command");
        check(probe_observe(window,6)==0&&probe_observe(window,5)==1,"OS trial physical queue and command baseline");
        check(probe_try_epoch_snapshot(window,epoch,1,retained)==ProbeEvidenceRead::copied &&
              valid_identity(retained,epoch,retained.evidence.generation),"OS valid baseline generation");
        generation=retained.evidence.generation;generation_pinned=true;
        probe_mark_focus(window,1);success(probe_latch(window,true,false),"OS trial latch");
        auto target=tagged_focus(window,edit,epoch,1);
        wait_for([&]{ProbeEpochSnapshot s;return probe_try_epoch_snapshot(window,epoch,1,s)==ProbeEvidenceRead::copied &&
            valid_identity(s,epoch,generation) && s.evidence.target_present;},"OS trial exact target admitted",500);
        go=true;
        bounded(target.get(),timeout,"OS trial canceled direct target");
        probe_mark_focus(window,2);success(probe_latch(window,false,false),"OS trial release");
        wait_for([&]{return done.load();},"OS actor actual return",2000);
        os_actor.join();
        check(os_result.load()==S_OK||os_result.load()==timeout,"OS real Value returns actual success or queued timeout");
        terminal_pop(window,epoch,1,generation,retained);
        finalize_fixture(window,epoch,generation,retained,"OS trial final copy/disarm");
        const auto adjudication=adjudicate_os_value(retained,3,os_result.load());
        const bool consistent=(os_result.load()==S_OK && adjudication==OsValueEvidence::consistent_success) ||
                              (os_result.load()==timeout && adjudication==OsValueEvidence::consistent_timeout);
        std::cout<<"CONTROLLED OS Value result="<<os_result.load()<<" Value_evidence="<<os_value_label(adjudication)
            <<" separate_focus_mutation="<<retained.evidence.other_valid_mutation
            <<"; OS association="<<(consistent?"candidate interval consistent":"inconclusive candidate interval")
            <<"; historical cause UNKNOWN.\n"<<probe_format_epoch_snapshot(retained);
        check(consistent,"OS actual Value token/result evidence inconclusive");
        success(probe_control(window,5,0),"OS trial own cleanup");
        return 0;
    }catch(const std::exception& error){
        std::cerr<<"FAIL controlled OS: "<<error.what()<<'\n';
        ProbeEpochSnapshot temporary;
        if(probe_try_epoch_snapshot(window,epoch,1,temporary,true)==ProbeEvidenceRead::copied &&
           generation_pinned && valid_identity(temporary,epoch,generation))retained=temporary;
        std::cerr<<probe_format_epoch_snapshot(retained);
        if(!probe_observe(window,3)){probe_latch(window,false,false);probe_control(window,5,0);}
        return 1;
    }
}

int run_retirement_child_tests() {
    try {
      for (const auto child_mode : {L"--retirement-child", L"--retirement-release-child",
          L"--retirement-failure-child", L"--retirement-creation-child"}) {
        std::array<wchar_t, 32768> executable{};
        const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        check(length != 0 && length < executable.size(), "exact own executable path");
        const std::wstring path(executable.data(), length);
        const auto log_path = path + L".own-retirement-" + std::to_wstring(GetCurrentProcessId()) + child_mode + L".log";
        SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        struct Handle { HANDLE value{}; ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); } };
        Handle output{CreateFileW(log_path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
            &attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
        check(output.value != INVALID_HANDLE_VALUE, "own child bounded file output");
        Handle input{CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        check(input.value != INVALID_HANDLE_VALUE, "own child NUL input");
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE; startup.hStdInput = input.value; startup.hStdOutput = startup.hStdError = output.value;
        PROCESS_INFORMATION process{};
        auto command = L"\"" + path + L"\" " + child_mode;
        const auto start = Clock::now();
        check(CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
            nullptr, nullptr, &startup, &process) != FALSE, "launch only exact own child");
        Handle child{process.hProcess}, child_thread{process.hThread};
        const auto wait = WaitForSingleObject(child.value, 8500);
        if (wait != WAIT_OBJECT_0) {
            TerminateProcess(child.value, 99); // exact child handle, never process search
            check(WaitForSingleObject(child.value, 2000) == WAIT_OBJECT_0, "own child controller cleanup bound");
            throw std::runtime_error("own retirement child containment absent (controller timeout)");
        }
        DWORD exit{}; check(GetExitCodeProcess(child.value, &exit) != FALSE, "exact own child exit");
        Handle reader{CreateFileW(log_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr)};
        check(reader.value != INVALID_HANDLE_VALUE, "own asynchronous output reader");
        Handle read_event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        check(read_event.value != nullptr, "own output completion event");
        OVERLAPPED read_operation{}; read_operation.hEvent = read_event.value;
        std::string log(65537, '\0'); DWORD read{};
        const bool read_complete = ReadFile(reader.value, log.data(), static_cast<DWORD>(log.size()), &read, &read_operation) != FALSE;
        if (!read_complete && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(read_event.value, 1000) != WAIT_OBJECT_0) {
                CancelIoEx(reader.value, &read_operation);
                if (WaitForSingleObject(read_event.value, 1000) != WAIT_OBJECT_0) std::_Exit(4); // retain no pending buffer lifetime
                throw std::runtime_error("own output drain exceeded bound");
            }
        } else check(read_complete, "own output read start");
        check(GetOverlappedResult(reader.value, &read_operation, &read, FALSE) && read <= 65536,
            "bounded own child output size and completed drain");
        log.resize(read);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
        std::cout << "Own retirement child: exit=" << exit << ", elapsed_ms=" << elapsed << '\n' << log;
        check(exit == 3 && elapsed <= 8000, "own watchdog exact exit3 within 5-second budget plus scheduler allowance");
        check(log.find("OWN LOCAL") != std::string::npos && log.find("hook entered") != std::string::npos &&
            log.find("UNSAFE normal apartment teardown") == std::string::npos, "hook reached and apartment success path not reached");
        if (std::wstring_view(child_mode) == L"--retirement-release-child")
            check(log.find("live_states=1, live_providers=1") != std::string::npos,
                "blocked cached-host Release does not report State or final provider retirement early");
      }
        std::cout << "PASS own process containment; no blocked OS COM or hard realtime claim\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL containment: " << error.what() << '\n'; return 1; }
}
