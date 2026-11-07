// Own-HWND UIA provider. Official contracts and process-containment limits: README.md.
#include <windows.h>
#include <objbase.h>
#include <uiautomation.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <stdexcept>
#include <thread>
#include <utility>
int run_probe_tests(HWND, bool = false);
int run_focus_evidence_contract_tests();
int run_fault_tests();
int run_fault_worker(HWND);
int run_regression_tests(int);
int run_retirement_child_tests();
int run_competing_focus(HWND);
int run_isolated_focus(HWND);
int run_os_interleave(HWND);
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

namespace {
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
constexpr UINT dispatch_message = WM_APP + 1, cleanup_message = WM_APP + 2;
constexpr UINT start_message = WM_APP + 3, done_message = WM_APP + 4;
constexpr int nodes = 5;
constexpr HRESULT unavailable = static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE);
constexpr HRESULT disabled = static_cast<HRESULT>(UIA_E_ELEMENTNOTENABLED);
constexpr HRESULT invalid = static_cast<HRESULT>(UIA_E_INVALIDOPERATION);
constexpr HRESULT timeout = static_cast<HRESULT>(UIA_E_TIMEOUT);
class Provider;
enum class Action { invoke, value, focus, control };
enum class Completion { pending, complete, canceled };
std::atomic<unsigned> request_tokens{};
thread_local UINT own_input_origin{};
struct DiagnosticActor { std::uint64_t epoch{}, actor{}; ProbeActorKind kind{ProbeActorKind::provider_untagged}; };
thread_local DiagnosticActor diagnostic_actor;
thread_local std::array<DiagnosticActor,8> diagnostic_actor_stack{};
thread_local unsigned diagnostic_actor_depth{}, diagnostic_actor_overflow{};
std::atomic<std::uint64_t> diagnostic_epochs{};
struct NestedDiagnosticActor {
    DiagnosticActor before;
    NestedDiagnosticActor(std::uint64_t epoch,std::uint64_t actor,ProbeActorKind kind) noexcept
        : before(diagnostic_actor) { diagnostic_actor={epoch,actor,kind}; }
    ~NestedDiagnosticActor(){diagnostic_actor=before;}
};
struct Request {
    Action action{}; int node{}, control{}, argument{}; unsigned generation{};
    std::wstring text; Clock::time_point deadline{};
    Completion completion{Completion::pending}; HRESULT result{E_PENDING};
    unsigned token{++request_tokens}; DWORD caller{}; UINT input_origin{};
    std::uint64_t diagnostic_epoch{}, actor{}, candidate_os{};
    ProbeActorKind actor_kind{ProbeActorKind::provider_untagged};
};
struct FocusTrace {
    ULONGLONG tick{}; unsigned token{}; DWORD caller{}, executing{}; UINT input{};
    int phase{}, node{}, before{}, after{}; bool held{}, expired{}; HRESULT result{};
};
std::atomic<unsigned> live_states{}, live_providers{};
std::atomic<unsigned> wake_created{}, wake_closed{};
std::atomic<long long> simulated_idle_ms{};
Clock::time_point worker_now() { return Clock::now() + std::chrono::milliseconds(simulated_idle_ms.load()); }
HANDLE acquire_wake() { auto event = CreateEventW(nullptr, FALSE, FALSE, nullptr); if (event) ++wake_created; return event; }
void close_wake(HANDLE event) { if (event && CloseHandle(event)) ++wake_closed; }
struct OwnedWake {
    HANDLE value{};
    explicit OwnedWake(bool fail = false) : value(fail ? nullptr : acquire_wake()) {}
    ~OwnedWake() { reset(); }
    OwnedWake(const OwnedWake&) = delete;
    OwnedWake& operator=(const OwnedWake&) = delete;
    void reset() noexcept { close_wake(std::exchange(value, nullptr)); }
};
struct RetirementControl {
    std::atomic<ULONGLONG> deadline{};
    std::atomic<bool> complete{};
    void arm() noexcept {
        ULONGLONG unarmed = 0;
        deadline.compare_exchange_strong(unarmed, GetTickCount64() + 5000);
    }
};
static_assert(std::atomic<ULONGLONG>::is_always_lock_free && std::atomic<bool>::is_always_lock_free);
class RetirementWatchdog {
public:
    std::shared_ptr<RetirementControl> control;
    explicit RetirementWatchdog(bool fail_creation = false) noexcept {
        try {
            if (fail_creation) throw std::runtime_error("controlled own watchdog creation failure");
            control = std::make_shared<RetirementControl>();
            thread_ = std::jthread([independent = control] {
                while (!independent->complete.load()) {
                    const auto deadline = independent->deadline.load();
                    if (deadline && GetTickCount64() >= deadline) std::_Exit(3);
                    Sleep(5); // independent atomics only: no provider lock, COM, pump or I/O
                }
            });
        } catch (...) { std::_Exit(3); } // no diagnostics or unsafe apartment teardown
    }
    void finish() noexcept { control->complete = true; }
    ~RetirementWatchdog() { finish(); } // jthread joins only the independent polling thread
private:
    std::jthread thread_;
};
std::wstring initial_value(int construction_fault) {
    if (construction_fault == 1) throw std::bad_alloc(); // owned constructor-failure seam
    return L"Tiếng Việt";
}
std::atomic<bool> diagnostic_entered{}, diagnostic_release{true};
struct State {
    std::mutex mutex; std::condition_variable changed;
    HWND hwnd{}, identity{}; // identity comparison only after close
    DWORD ui_thread{}; unsigned generation{}, count{};
    bool closing{}, disconnected{}, held{}, fail_post{}, read_only{}, host_focused{}, testing{};
    bool enabled{true}, value_enabled{true}, visible{}; int focused{1}; HRESULT disconnect_result{E_PENDING};
    OwnedWake wake_owner;
    const HANDLE wake{wake_owner.value}; // borrowed immutable alias, exactly one close owner
    std::shared_ptr<RetirementControl> retirement;
    bool ready{}, ever_ready{}, start_requested{}, notification_failed{}, worker_finished{true}, cleanup_attempted{};
    bool window_retired{true}, map_cleared{true};
    int worker_result{}, fault{};
    explicit State(int construction_fault = 0) : wake_owner(construction_fault == 3), value(initial_value(construction_fault)) {
        if (!wake || construction_fault == 2) throw std::runtime_error("controlled/actual own event acquisition failure");
        ++live_states;
    }
    ~State() {
        if (fault == 13) {
            std::cout << "OWN LOCAL retirement hook entered: live_states=" << live_states.load()
                << ", live_providers=" << live_providers.load() << '\n' << std::flush;
            Sleep(INFINITE); // owned hook before cached-host final Release
        }
        host_provider.Reset(); // may block: completed-state count changes only afterwards
        wake_owner.reset();
        --live_states;
    }
    std::wstring value{L"Tiếng Việt"};
    std::wstring status{L"Probe only. NSTU / Narrator / NVDA acceptance: NOT TESTED."};
    std::array<RECT, nodes> bounds{};
    ComPtr<IRawElementProviderSimple> host_provider; // immutable HWND host identity, not a live-HWND lookup
    std::array<Provider*, nodes> providers{}; // app owns refs; no state cycle
    std::deque<std::shared_ptr<Request>> requests, controls;
    std::array<FocusTrace, 64> focus_trace{}; unsigned focus_trace_count{};
    std::uint64_t diagnostic_epoch{}, diagnostic_sequence{}, pump_cycle{}, first_front_pop{}, last_front_pop{};
    std::uint64_t active_os_actor{}, recent_os_actor{};
    bool diagnostic_armed{}, diagnostic_lost{};
    unsigned diagnostic_count{};
    std::array<ProbeDiagnosticRecord,256> diagnostic_records{};
};
std::mutex registry_mutex; std::shared_ptr<State> registry;
std::atomic<unsigned> generation_counter{1};
std::atomic<unsigned> providers_created{}, providers_retired{};
enum class Notice { start, done, cleanup };
void diagnostic_sink(Notice) {
    diagnostic_entered = true;
    while (!diagnostic_release.load()) Sleep(1); // local controlled sink, no foreign pipe
}
void critical_notice(const std::shared_ptr<State>& state, Notice notice) {
    HWND window{}; DWORD thread{}; unsigned generation{}; int fault{};
    { std::lock_guard lock(state->mutex); window = state->hwnd; thread = state->ui_thread;
      generation = state->generation; fault = state->fault; }
    const bool drop = (notice == Notice::start && fault == 6) ||
        (notice == Notice::done && (fault == 7 || fault == 10)) ||
        (notice == Notice::cleanup && (fault == 8 || fault == 10));
    const UINT message = notice == Notice::start ? start_message : notice == Notice::done ? done_message : cleanup_message;
    const bool posted = !drop && (notice == Notice::start ? PostMessageW(window, message, 0, 0) :
        PostThreadMessageW(thread, message, generation, 0));
    const bool signaled = state->wake && fault != 9 && fault != 10 && SetEvent(state->wake);
    if (!posted || !signaled) {
        { std::lock_guard lock(state->mutex);
          state->notification_failed = true;
          if (notice == Notice::start) { state->start_requested = false; state->testing = false; } }
        diagnostic_sink(notice);
        std::cerr << "Notification failure: kind=" << static_cast<int>(notice)
            << ", post=" << posted << ", wake=" << signaled << ", injected=" << fault << '\n';
    }
}
std::shared_ptr<State> lookup(HWND identity) {
    std::lock_guard lock(registry_mutex);
    return registry && registry->identity == identity ? registry : nullptr;
}
bool valid_text(std::wstring_view text) noexcept {
    if (text.size() > 1024) return false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto unit = static_cast<unsigned>(text[i]);
        if (unit == 0 || unit == '\r' || unit == '\n') return false;
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            if (++i == text.size()) return false;
            const auto next = static_cast<unsigned>(text[i]);
            if (next < 0xDC00 || next > 0xDFFF) return false;
        } else if (unit >= 0xDC00 && unit <= 0xDFFF) return false;
    }
    return true;
}
HRESULT guard(const State& state, const Request& request) noexcept {
    if (state.closing || !state.hwnd || request.generation != state.generation) return unavailable;
    if (request.action == Action::control) return S_OK;
    if ((request.node == 1 && !state.enabled) || (request.node == 2 && !state.value_enabled) ||
        (request.node == 4 && state.testing)) return disabled;
    if (request.action == Action::value && state.read_only) return invalid;
    return S_OK;
}
void trace_focus(State& state, const Request& request, int phase, int before, HRESULT result) noexcept {
    state.focus_trace[state.focus_trace_count++ % state.focus_trace.size()] = {
        GetTickCount64(), request.token, request.caller, GetCurrentThreadId(), request.input_origin,
        phase, request.node, before, state.focused, state.held, Clock::now() >= request.deadline, result};
}

// Diagnostic phases:0=actor,1=submit,2=queued,3=rejected,4=complete,5=canceled,
// 6=mutation,7=front-pop,8=prune,9=notify-rollback,10=retirement-clear,
// 11=end-pump,12=host-focus. No record influences action eligibility.
void record_diagnostic(State& state,unsigned phase,const Request* request=nullptr,
                       HRESULT result=S_OK,int before=-1,unsigned stage=0,
                       std::uint64_t actor=0,ProbeActorKind kind=ProbeActorKind::provider_untagged,
                       unsigned total_before=0,unsigned controls_before=0) noexcept {
    if(!state.diagnostic_armed)return;
    const auto sequence=++state.diagnostic_sequence;
    if(state.diagnostic_count==state.diagnostic_records.size()){state.diagnostic_lost=true;return;}
    ProbeDiagnosticRecord record;
    record.sequence=sequence;record.epoch=state.diagnostic_epoch;record.pump=state.pump_cycle;
    record.actor=request?request->actor:actor;
    record.candidate_os=request?request->candidate_os:(state.active_os_actor?state.active_os_actor:state.recent_os_actor);
    record.token=request?request->token:0;record.generation=request?request->generation:state.generation;
    record.caller=request?request->caller:GetCurrentThreadId();record.executing=GetCurrentThreadId();
    record.phase=phase;record.stage=stage;
    record.action=request?static_cast<unsigned>(request->action):99;
    record.node=request?static_cast<unsigned>(request->node):static_cast<unsigned>(state.focused);
    record.completion=request?static_cast<unsigned>(request->completion):0;
    record.origin=request?request->actor_kind:kind;record.input=request?request->input_origin:own_input_origin;
    record.before=before<0?state.focused:before;record.after=state.focused;
    record.held=state.held;record.host_focused=state.host_focused;
    record.expired=request && Clock::now()>=request->deadline;
    record.total=static_cast<unsigned>(state.requests.size());
    record.pending=static_cast<unsigned>(std::count_if(state.requests.begin(),state.requests.end(),
        [](const auto& item){return item->completion==Completion::pending;}));
    record.controls=static_cast<unsigned>(state.controls.size());
    record.total_before=total_before;record.controls_before=controls_before;record.result=result;
    record.first_pop=state.first_front_pop;record.last_pop=state.last_front_pop;
    state.diagnostic_records[state.diagnostic_count++]=record;
}
void record_removal(State& state,unsigned phase,const Request& request,
                    unsigned total_before,unsigned controls_before) noexcept {
    record_diagnostic(state,phase,&request,request.result,-1,0,0,ProbeActorKind::provider_untagged,total_before,controls_before);
    if(phase==7 && state.diagnostic_armed){
        if(!state.first_front_pop)state.first_front_pop=state.diagnostic_sequence;
        state.last_front_pop=state.diagnostic_sequence;
    }
}

void finish(State& state, Request& request, HRESULT result, bool canceled = false) noexcept {
    if (request.action == Action::focus) trace_focus(state, request, canceled ? 2 : 1, state.focused, result);
    request.result = result; request.completion = canceled ? Completion::canceled : Completion::complete;
    record_diagnostic(state,canceled?5u:4u,&request,result);
    state.changed.notify_all();
}
HRESULT notify(State& state) noexcept {
    if (state.fail_post) return HRESULT_FROM_WIN32(ERROR_GEN_FAILURE);
    if (PostMessageW(state.hwnd, dispatch_message, 0, 0)) return S_OK;
    const auto error = GetLastError(); return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
}
void dispatch(const std::shared_ptr<State>&, const std::shared_ptr<Request>& direct = {});
HRESULT submit(const std::shared_ptr<State>& state, const std::shared_ptr<Request>& request, bool wait) {
    std::unique_lock lock(state->mutex);
    request->generation = state->generation; request->deadline = Clock::now() + std::chrono::milliseconds(750);
    request->caller = GetCurrentThreadId(); request->input_origin = own_input_origin;
    if(state->diagnostic_armed && diagnostic_actor.epoch==state->diagnostic_epoch){
        request->diagnostic_epoch=diagnostic_actor.epoch;request->actor=diagnostic_actor.actor;request->actor_kind=diagnostic_actor.kind;
    }
    if(own_input_origin)request->actor_kind=ProbeActorKind::own_input;
    request->candidate_os=state->active_os_actor?state->active_os_actor:state->recent_os_actor;
    record_diagnostic(*state,1,request.get(),E_PENDING);
    if (request->action == Action::focus) trace_focus(*state, *request, 0, state->focused, E_PENDING);
    if (const auto hr = guard(*state, *request); FAILED(hr)) {
        record_diagnostic(*state,3,request.get(),hr);
        if (request->action == Action::focus) trace_focus(*state, *request, 4, state->focused, hr);
        return hr;
    }
    if (wait && GetCurrentThreadId() == state->ui_thread) {
        lock.unlock();
        dispatch(state, request); // one direct action; no queue wait or draining unrelated requests
        lock.lock();
        return request->result;
    }
    auto& queue = request->action == Action::control ? state->controls : state->requests;
    std::array<std::shared_ptr<Request>,32> removed{};
    unsigned removed_count=0;
    const auto before_prune_total=static_cast<unsigned>(state->requests.size());
    const auto before_prune_controls=static_cast<unsigned>(state->controls.size());
    for(const auto& item:queue)if(item->completion!=Completion::pending && removed_count<removed.size())removed[removed_count++]=item;
    std::erase_if(queue, [](const auto& item) { return item->completion != Completion::pending; });
    for(unsigned i=0;i<removed_count;++i)record_removal(*state,8,*removed[i],before_prune_total,before_prune_controls);
    if (queue.size() >= (request->action == Action::control ? 8u : 32u)) {
        if (request->action == Action::focus) trace_focus(*state, *request, 4, state->focused, E_PENDING);
        return E_PENDING;
    }
    queue.push_back(request);
    if (const auto hr = notify(*state); FAILED(hr)) {
        const auto rollback_total=static_cast<unsigned>(state->requests.size());
        const auto rollback_controls=static_cast<unsigned>(state->controls.size());
        queue.pop_back();
        record_removal(*state,9,*request,rollback_total,rollback_controls);
        if (request->action == Action::focus) trace_focus(*state, *request, 4, state->focused, hr);
        return hr;
    }
    record_diagnostic(*state,2,request.get(),S_OK);
    if (!wait) return S_OK; // Invoke only: asynchronous acceptance
    if (!state->changed.wait_until(lock, request->deadline,
        [&] { return request->completion != Completion::pending; }))
        finish(*state, *request, timeout, true); // serialized with mutation; no late apply
    return request->result;
}
class Provider final : public IRawElementProviderSimple, public IRawElementProviderFragment,
    public IRawElementProviderFragmentRoot, public IInvokeProvider, public IValueProvider {
public:
    Provider(std::shared_ptr<State> state, int node) : state_(std::move(state)), node_(node) { ++live_providers; ++providers_created; }
    ~Provider() { state_.reset(); --live_providers; ++providers_retired; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (iid == IID_IUnknown || iid == IID_IRawElementProviderSimple) *result = static_cast<IRawElementProviderSimple*>(this);
        else if (iid == IID_IRawElementProviderFragment) *result = static_cast<IRawElementProviderFragment*>(this);
        else if (iid == IID_IRawElementProviderFragmentRoot && node_ == 0) *result = static_cast<IRawElementProviderFragmentRoot*>(this);
        else if (iid == IID_IInvokeProvider && (node_ == 1 || node_ == 4)) *result = static_cast<IInvokeProvider*>(this);
        else if (iid == IID_IValueProvider && node_ == 2) *result = static_cast<IValueProvider*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto count = --refs_; if (!count) { const std::unique_ptr<Provider> retired(this); } return count;
    }
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* result) override {
        if (!result) return E_POINTER;
        *result = static_cast<ProviderOptions>(ProviderOptions_ServerSideProvider | ProviderOptions_UseComThreading);
        // Immutable COM transport metadata remains available for disconnect;
        // semantic getters/patterns/actions still reject retired elements.
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID pattern, IUnknown** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (const auto hr = available(); FAILED(hr)) return hr;
        if (pattern == UIA_InvokePatternId && (node_ == 1 || node_ == 4)) return QueryInterface(IID_IInvokeProvider, reinterpret_cast<void**>(result));
        if (pattern == UIA_ValuePatternId && node_ == 2) return QueryInterface(IID_IValueProvider, reinterpret_cast<void**>(result));
        return S_OK; // TextPattern absent
    }
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID property, VARIANT* result) override {
        if (!result) return E_POINTER;
        VariantInit(result); std::lock_guard lock(state_->mutex);
        if (state_->closing) return unavailable;
        switch (property) {
        case UIA_ControlTypePropertyId:
            result->vt = VT_I4; result->lVal = node_ == 0 ? UIA_PaneControlTypeId : node_ == 2 ? UIA_EditControlTypeId : node_ == 3 ? UIA_TextControlTypeId : UIA_ButtonControlTypeId; break;
        case UIA_NamePropertyId: return text(result, name());
        case UIA_AutomationIdPropertyId: return text(result, std::array<std::wstring_view, nodes>{L"probe.root", L"probe.command", L"probe.value", L"probe.status", L"probe.selftest"}[node_]);
        case UIA_FrameworkIdPropertyId: return text(result, L"NSTU.NativeUiaProbe");
        case UIA_IsControlElementPropertyId: case UIA_IsContentElementPropertyId: boolean(result, true); break;
        case UIA_IsEnabledPropertyId: boolean(result, node_ == 1 ? state_->enabled : node_ == 2 ? state_->value_enabled : node_ == 4 ? !state_->testing : true); break;
        case UIA_IsKeyboardFocusablePropertyId: boolean(result, node_ == 1 || node_ == 2 || node_ == 4); break;
        case UIA_HasKeyboardFocusPropertyId: boolean(result, state_->host_focused && node_ == state_->focused); break;
        case UIA_IsOffscreenPropertyId: boolean(result, !state_->visible || IsRectEmpty(&state_->bounds[node_]) != FALSE); break;
        case UIA_IsPasswordPropertyId: boolean(result, false); break;
        case UIA_ValueValuePropertyId: if (node_ == 2) return text(result, state_->value); break;
        case UIA_ValueIsReadOnlyPropertyId: if (node_ == 2) boolean(result, state_->read_only); break;
        default: break; // VT_EMPTY: documented HWND fallback
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        // Windows needs immutable host/runtime identity during disconnect. This
        // cached COM host never calls an API with the destroyed HWND. It stays
        // alive with state; copy its owning ref outside the state lock.
        return node_ == 0 ? state_->host_provider.CopyTo(result) : S_OK;
    }
    HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction, IRawElementProviderFragment** result) override {
        if (!result) return E_POINTER;
        *result = nullptr; std::lock_guard lock(state_->mutex);
        if (state_->closing) return unavailable;
        int target = -1;
        if (direction < NavigateDirection_Parent || direction > NavigateDirection_LastChild) return E_INVALIDARG;
        if (node_ == 0 && direction == NavigateDirection_FirstChild) target = 1;
        else if (node_ == 0 && direction == NavigateDirection_LastChild) target = nodes - 1;
        else if (node_ && direction == NavigateDirection_Parent) target = 0;
        else if (node_ && direction == NavigateDirection_NextSibling && node_ + 1 < nodes) target = node_ + 1;
        else if (node_ > 1 && direction == NavigateDirection_PreviousSibling) target = node_ - 1;
        if (target >= 0) fragment(target, result);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** result) override {
        if (!result) return E_POINTER;
        *result = nullptr; std::lock_guard lock(state_->mutex);
        // Immutable fragment identity is also required by UiaDisconnectProvider.
        if (!node_) return S_OK;
        auto array = SafeArrayCreateVector(VT_I4, 0, 3); if (!array) return E_OUTOFMEMORY;
        std::array<LONG, 3> ids{UiaAppendRuntimeId, static_cast<LONG>(state_->generation), node_};
        for (LONG i = 0; i < 3; ++i) {
            const auto hr = SafeArrayPutElement(array, &i, &ids[static_cast<std::size_t>(i)]);
            if (FAILED(hr)) { SafeArrayDestroy(array); return hr; }
        }
        *result = array; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* result) override {
        if (!result) return E_POINTER;
        *result = {}; std::lock_guard lock(state_->mutex); if (state_->closing) return unavailable;
        const auto& rect = state_->bounds[node_];
        *result = {static_cast<double>(rect.left), static_cast<double>(rect.top), static_cast<double>(rect.right - rect.left), static_cast<double>(rect.bottom - rect.top)};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** result) override {
        if (!result) return E_POINTER;
        *result = nullptr; return available();
    }
    HRESULT STDMETHODCALLTYPE SetFocus() override { return action(Action::focus); }
    HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** result) override {
        if (!result) return E_POINTER;
        *result = nullptr; std::lock_guard lock(state_->mutex);
        // Fragment-root identity completes child runtime IDs during disconnect.
        if (!state_->providers[0]) return unavailable;
        *result = static_cast<IRawElementProviderFragmentRoot*>(state_->providers[0]); state_->providers[0]->AddRef(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** result) override {
        if (!result) return E_POINTER;
        *result = nullptr; std::lock_guard lock(state_->mutex); if (state_->closing) return unavailable;
        for (int i = nodes - 1; i >= 0; --i) {
            const auto& rect = state_->bounds[i];
            if (x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom) { fragment(i, result); break; }
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** result) override {
        if (!result) return E_POINTER;
        *result = nullptr; std::lock_guard lock(state_->mutex); if (state_->closing) return unavailable;
        if (state_->host_focused) fragment(state_->focused, result);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Invoke() override { return action(Action::invoke); }
    HRESULT STDMETHODCALLTYPE SetValue(LPCWSTR value) override {
        if (!value) return E_INVALIDARG;
        std::size_t size = 0; while (size <= 1024 && value[size]) ++size;
        if (size > 1024 || !valid_text({value, size})) return E_INVALIDARG;
        return action(Action::value, {value, size});
    }
    HRESULT STDMETHODCALLTYPE get_Value(BSTR* result) override {
        if (!result) return E_POINTER;
        *result = nullptr; std::lock_guard lock(state_->mutex); if (state_->closing) return unavailable;
        *result = SysAllocStringLen(state_->value.data(), static_cast<UINT>(state_->value.size()));
        return *result ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE get_IsReadOnly(BOOL* result) override {
        if (!result) return E_POINTER;
        *result = FALSE; std::lock_guard lock(state_->mutex); if (state_->closing) return unavailable;
        *result = state_->read_only; return S_OK;
    }
private:
    std::atomic<ULONG> refs_{1}; std::shared_ptr<State> state_; int node_{};
    HRESULT available() const { std::lock_guard lock(state_->mutex); return state_->closing ? unavailable : S_OK; }
    static void boolean(VARIANT* out, bool value) noexcept { out->vt = VT_BOOL; out->boolVal = value ? VARIANT_TRUE : VARIANT_FALSE; }
    static HRESULT text(VARIANT* out, std::wstring_view value) noexcept {
        auto result = SysAllocStringLen(value.data(), static_cast<UINT>(value.size())); if (!result) return E_OUTOFMEMORY;
        out->vt = VT_BSTR; out->bstrVal = result; return S_OK;
    }
    std::wstring_view name() const noexcept {
        if (!node_) return L"NSTU UI Automation feasibility";
        if (node_ == 1) return L"Lệnh thử an toàn";
        if (node_ == 2) return L"Giá trị tiếng Việt";
        if (node_ == 3) return state_->status;
        return L"Run own-window self-test (F5)";
    }
    void fragment(int node, IRawElementProviderFragment** result) noexcept {
        auto provider = state_->providers[node]; if (provider) { *result = static_cast<IRawElementProviderFragment*>(provider); provider->AddRef(); }
    }
    HRESULT action(Action kind, std::wstring_view value = {}) {
        try { auto request = std::make_shared<Request>(); request->action = kind; request->node = node_; request->text = value;
            return submit(state_, request, kind != Action::invoke); }
        catch (const std::bad_alloc&) { return E_OUTOFMEMORY; } catch (...) { return E_FAIL; }
    }
};
void publish_focus(const std::shared_ptr<State>& state) {
    const auto actual = ::GetFocus(); // own UI thread only
    ComPtr<IRawElementProviderSimple> provider; bool old{}, now{};
    { std::lock_guard lock(state->mutex); old = state->host_focused; now = !state->closing && actual == state->hwnd;
      state->host_focused = now; if (old != now) { provider = state->providers[state->focused];
          record_diagnostic(*state,12,nullptr,S_OK,state->focused,0,diagnostic_actor.actor,diagnostic_actor.kind); } }
    if (provider) {
        VARIANT before{}, after{}; before.vt = after.vt = VT_BOOL;
        before.boolVal = old ? VARIANT_TRUE : VARIANT_FALSE; after.boolVal = now ? VARIANT_TRUE : VARIANT_FALSE;
        UiaRaiseAutomationPropertyChangedEvent(provider.Get(), UIA_HasKeyboardFocusPropertyId, before, after);
        if (now) UiaRaiseAutomationEvent(provider.Get(), UIA_AutomationFocusChangedEventId);
    }
}
void close_window(const std::shared_ptr<State>& state) {
    if (state->retirement) state->retirement->arm(); // before lock, DestroyWindow or HWND map-clear can block
    HWND window{};
    { std::lock_guard lock(state->mutex); if (state->closing) return;
      state->closing = true; state->ready = false; state->start_requested = false;
      state->host_focused = false; window = std::exchange(state->hwnd, nullptr); state->bounds = {};
      for (auto& queue : {&state->requests, &state->controls}) for (auto& request : *queue)
          if (request->completion == Completion::pending) finish(*state, *request, unavailable, true); }
    const bool destroyed = !window || DestroyWindow(window);
    { std::lock_guard lock(state->mutex); state->window_retired = destroyed; }
    critical_notice(state, Notice::cleanup);
}
void dispatch(const std::shared_ptr<State>& state, const std::shared_ptr<Request>& direct) {
    while (true) {
        std::shared_ptr<Request> request; ComPtr<IRawElementProviderSimple> provider, previous;
        std::wstring before, after; bool close{}, invoked{}, start{}, changed_focus{}; HWND window{};
        { std::unique_lock lock(state->mutex);
          if (direct) request = direct;
          else {
              auto& queue = !state->controls.empty() ? state->controls : state->requests;
              if (queue.empty() || (state->controls.empty() && state->held)) return;
              const auto pop_total=static_cast<unsigned>(state->requests.size());
              const auto pop_controls=static_cast<unsigned>(state->controls.size());
              request = queue.front(); queue.pop_front();
              record_removal(*state,7,*request,pop_total,pop_controls);
          }
          if (request->completion != Completion::pending) { if (direct) return; continue; }
          auto result = guard(*state, *request);
          if (SUCCEEDED(result) && Clock::now() >= request->deadline) result = timeout;
          if (FAILED(result)) { finish(*state, *request, result, true); if (direct) return; continue; }
          window = state->hwnd;
          try {
              if (request->action == Action::control) {
                  switch (request->control) {
                  case 1: state->read_only = request->argument != 0; break;
                  case 2: state->enabled = request->argument != 0; break;
                  case 3: lock.unlock(); ::SetFocus(request->argument ? window : nullptr); publish_focus(state); lock.lock(); break;
                  case 4: for (auto& pending : state->requests) --pending->generation; break; // retire requests, preserve semantic IDs
                  case 5: close = true; break;
                  case 6: { // own UI direct-provider path: must not wait for itself
                      auto item = state->providers[2];
                      lock.unlock(); result = item->SetValue(L"own UI direct dispatch"); lock.lock();
                      break;
                  }
                  case 7: state->value_enabled = request->argument != 0; break;
                  case 8: { // controlled own UI actor, through the real direct provider path
                      if (request->argument != 1 && request->argument != 2) { result = E_INVALIDARG; break; }
                      auto item = state->providers[request->argument];
                      NestedDiagnosticActor inherited(request->diagnostic_epoch,request->actor,ProbeActorKind::fixture_control);
                      lock.unlock(); result = item->SetFocus(); lock.lock();
                      break;
                  }
                  default: result = E_INVALIDARG; break;
                  }
              } else if (request->action == Action::value) {
                  before = state->value; after = request->text; // allocate before mutation
                  state->value.swap(request->text); provider = state->providers[2];
                  record_diagnostic(*state,6,request.get(),S_OK);
              } else if (request->action == Action::focus) {
                  state->host_focused = ::GetFocus() == window;
                  if (!state->host_focused || !request->node || request->node == 3) result = invalid;
                  else { changed_focus = request->node != state->focused;
                         const auto before_focus = state->focused;
                         previous = state->providers[state->focused]; state->focused = request->node; provider = state->providers[request->node];
                         trace_focus(*state, *request, 3, before_focus, S_OK);
                          record_diagnostic(*state,6,request.get(),S_OK,before_focus); }
              } else {
                  provider = state->providers[request->node]; invoked = true;
                  if (request->node == 1) ++state->count; else { state->testing = true; start = true; }
              }
          } catch (const std::bad_alloc&) { result = E_OUTOFMEMORY; } catch (...) { result = E_FAIL; }
          finish(*state, *request, result); }
        if (close) { close_window(state); return; }
        if (provider && request->action == Action::value) {
            VARIANT old_value{}, new_value{}; old_value.vt = new_value.vt = VT_BSTR;
            old_value.bstrVal = SysAllocStringLen(before.data(), static_cast<UINT>(before.size()));
            new_value.bstrVal = SysAllocStringLen(after.data(), static_cast<UINT>(after.size()));
            if (old_value.bstrVal && new_value.bstrVal) UiaRaiseAutomationPropertyChangedEvent(provider.Get(), UIA_ValueValuePropertyId, old_value, new_value);
            VariantClear(&old_value); VariantClear(&new_value);
        }
        if (changed_focus && provider) {
            VARIANT old_value{}, new_value{}; old_value.vt = new_value.vt = VT_BOOL;
            old_value.boolVal = VARIANT_TRUE; new_value.boolVal = VARIANT_FALSE;
            if (previous) UiaRaiseAutomationPropertyChangedEvent(previous.Get(), UIA_HasKeyboardFocusPropertyId, old_value, new_value);
            std::swap(old_value.boolVal, new_value.boolVal);
            UiaRaiseAutomationPropertyChangedEvent(provider.Get(), UIA_HasKeyboardFocusPropertyId, old_value, new_value);
            UiaRaiseAutomationEvent(provider.Get(), UIA_AutomationFocusChangedEventId);
        }
        if (invoked && provider) UiaRaiseAutomationEvent(provider.Get(), UIA_Invoke_InvokedEventId);
        if (start) {
            { std::lock_guard lock(state->mutex); state->start_requested = true; }
            critical_notice(state, Notice::start);
        }
        InvalidateRect(window, nullptr, FALSE);
        if (direct) return;
    }
}
void update_bounds(const std::shared_ptr<State>& state) {
    HWND window{}; { std::lock_guard lock(state->mutex); if (state->closing) return; window = state->hwnd; }
    RECT client{}; POINT origin{}; GetClientRect(window, &client); ClientToScreen(window, &origin);
    const auto dpi = GetDpiForWindow(window); const double scale = dpi ? static_cast<double>(dpi) / 96.0 : 1.0;
    const auto scaled = [scale](int value) { return static_cast<LONG>(value * scale); };
    std::array<RECT, nodes> bounds{}; bounds[0] = {origin.x, origin.y, origin.x + client.right, origin.y + client.bottom};
    for (int i = 1; i < nodes; ++i) {
        bounds[i] = {origin.x + scaled(16), origin.y + scaled(12 + (i - 1) * 60),
            origin.x + std::max(scaled(16), client.right - scaled(16)), origin.y + std::min(client.bottom, scaled(56 + (i - 1) * 60))};
        if (bounds[i].bottom < bounds[i].top) bounds[i].bottom = bounds[i].top;
    }
    const bool visible = IsWindowVisible(window) && !IsIconic(window);
    std::lock_guard lock(state->mutex); state->bounds = bounds; state->visible = visible;
}
struct Host;
void shutdown_host(Host&) noexcept;
struct Host {
    RetirementWatchdog watchdog;
    std::shared_ptr<State> state; std::array<ComPtr<IRawElementProviderSimple>, nodes> owners;
    std::jthread worker; bool automatic{}, worker_started{}, retired{};
    int result{}, fault{}, final_worker_result{2};
    bool final_window_retired{}, final_worker_retired{}, final_refs_retired{};
    Clock::time_point worker_deadline{};
    ~Host() { shutdown_host(*this); }
};
Host* host_pointer{}; // own UI thread only
void start_worker(Host& host, bool fault_worker = false, bool competing_focus = false, int diagnostic_mode = 0) {
    auto state = host.state;
    bool finished{};
    { std::lock_guard lock(state->mutex); finished = state->worker_finished; }
    if (!finished || state->closing) return;
    if (host.worker.joinable()) host.worker.join(); // completion state says MTA refs retired
    { std::lock_guard lock(state->mutex); state->worker_finished = false;
      state->worker_result = 2; state->testing = true; state->start_requested = false; }
    host.worker_started = true;
    host.worker_deadline = worker_now() + std::chrono::seconds(60); // reset at each actual admission
    try {
        if (host.fault == 3) throw std::runtime_error("injected thread start failure");
        host.worker = std::jthread([state, fault_worker, competing_focus, diagnostic_mode] {
            int result = 2;
            try { result = diagnostic_mode==1 ? run_isolated_focus(state->identity) :
                diagnostic_mode==2 ? run_os_interleave(state->identity) :
                diagnostic_mode==3 ? run_probe_tests(state->identity,true) :
                competing_focus ? run_competing_focus(state->identity) :
                fault_worker ? run_fault_worker(state->identity) : run_probe_tests(state->identity); }
            catch (...) { std::cerr << "FAIL: escaped test exception\n"; }
            // This state is authoritative, independent of notifications. The
            // callback runs only after the originating MTA's refs are retired.
            { std::lock_guard lock(state->mutex); state->worker_result = result; }
            critical_notice(state, Notice::done);
            // Completion follows diagnostics too, so a host cannot join a
            // worker still blocked in its notice sink. The timed pump observes
            // this state even when the earlier message/wake hint was dropped.
            { std::lock_guard lock(state->mutex); state->worker_finished = true; }
        });
    } catch (...) {
        { std::lock_guard lock(state->mutex); state->worker_finished = true;
          state->worker_result = 2; state->testing = false; }
        host.result = 2;
        throw;
    }
}
void paint(HWND window, const std::shared_ptr<State>& state) {
    struct PaintScope {
        HWND window{}; PAINTSTRUCT ps{}; HDC dc{}; HGDIOBJ prior_font{};
        explicit PaintScope(HWND value) : window(value), dc(BeginPaint(value, &ps)) {}
        ~PaintScope() { if (dc) { if (prior_font) SelectObject(dc, prior_font); EndPaint(window, &ps); } }
    } scope(window);
    const auto dc = scope.dc; if (!dc) return;
    RECT client{}; GetClientRect(window, &client);
    FillRect(dc, &client, GetSysColorBrush(COLOR_WINDOW)); scope.prior_font = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
    std::array<RECT, nodes> bounds{}; std::wstring value, status; unsigned count{}; int focused{}; bool actual{};
    { std::lock_guard lock(state->mutex); bounds = state->bounds; value = state->value; status = state->status;
      count = state->count; focused = state->focused; actual = state->host_focused; }
    POINT origin{}; ClientToScreen(window, &origin);
    std::array<std::wstring, nodes> labels{L"", L"Lệnh thử an toàn — count: " + std::to_wstring(count), L"Giá trị tiếng Việt: " + value, status,
        L"Run own-window self-test (F5). Tab / Enter operate sample commands."};
    for (int i = 1; i < nodes; ++i) {
        auto rect = bounds[i]; OffsetRect(&rect, -origin.x, -origin.y);
        if (i != 3) FrameRect(dc, &rect, GetSysColorBrush(COLOR_WINDOWFRAME));
        if (actual && focused == i) DrawFocusRect(dc, &rect);
        InflateRect(&rect, -6, -4); DrawTextW(dc, labels[i].c_str(), static_cast<int>(labels[i].size()), &rect, DT_LEFT | DT_WORDBREAK);
    }
}
LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    struct InputOriginScope {
        UINT previous{own_input_origin};
        explicit InputOriginScope(UINT message_value) { own_input_origin = message_value == WM_KEYDOWN || message_value == WM_LBUTTONUP ? message_value : 0; }
        ~InputOriginScope() { own_input_origin = previous; }
    } input_origin(message);
    auto state = lookup(window); if (!state) return DefWindowProcW(window, message, wparam, lparam);
    if(message==WM_KEYDOWN || message==WM_LBUTTONUP){
        std::lock_guard lock(state->mutex);record_diagnostic(*state,13,nullptr,S_OK,state->focused,0,0,ProbeActorKind::own_input);
    }
    try {
        switch (message) {
        case WM_GETOBJECT: {
            ComPtr<IRawElementProviderSimple> root; { std::lock_guard lock(state->mutex); if (!state->closing && state->ready) root = state->providers[0]; }
            return root ? UiaReturnRawElementProvider(window, wparam, lparam, root.Get()) : 0;
        }
        case WM_MOVE: case WM_SIZE: case WM_SHOWWINDOW: update_bounds(state); return 0;
        case WM_SETFOCUS: case WM_KILLFOCUS: publish_focus(state); PostMessageW(window, dispatch_message, 0, 0); return 0;
        case dispatch_message: publish_focus(state); dispatch(state); return 0;
        case start_message: return 0; // hint only; timed pump reads the authoritative request
        case WM_KEYDOWN: {
            if (wparam == VK_F5) { if (host_pointer) start_worker(*host_pointer); return 0; }
            int focused{}; { std::lock_guard lock(state->mutex); focused = state->focused; }
            if (wparam == VK_TAB) {
                const int next = focused == 1 ? 2 : focused == 2 ? 4 : 1;
                ComPtr<IRawElementProviderFragment> item; state->providers[next]->QueryInterface(IID_PPV_ARGS(&item)); item->SetFocus(); return 0;
            }
            if (wparam == VK_RETURN && (focused == 1 || focused == 4)) {
                ComPtr<IInvokeProvider> item; state->providers[focused]->QueryInterface(IID_PPV_ARGS(&item)); item->Invoke(); return 0;
            }
            break;
        }
        case WM_LBUTTONUP: {
            POINT point{static_cast<SHORT>(LOWORD(lparam)), static_cast<SHORT>(HIWORD(lparam))}; ClientToScreen(window, &point); ::SetFocus(window); publish_focus(state);
            for (int i : {1, 2, 4}) {
                RECT rect{}; { std::lock_guard lock(state->mutex); rect = state->bounds[i]; }
                if (PtInRect(&rect, point)) {
                    ComPtr<IRawElementProviderFragment> item; state->providers[i]->QueryInterface(IID_PPV_ARGS(&item)); item->SetFocus();
                    if (i != 2) { ComPtr<IInvokeProvider> invoke; state->providers[i]->QueryInterface(IID_PPV_ARGS(&invoke)); invoke->Invoke(); }
                    break;
                }
            }
            return 0;
        }
        case WM_PAINT: paint(window, state); return 0;
        case WM_CLOSE: close_window(state); return 0;
        case WM_DESTROY:
            UiaReturnRawElementProvider(window, 0, 0, nullptr);
            { std::lock_guard lock(state->mutex); state->map_cleared = true; }
            return 0;
        default: break;
        }
    } catch (...) { std::cerr << "FAIL: window operation exception\n"; close_window(state); return 0; }
    return DefWindowProcW(window, message, wparam, lparam);
}
bool create_window(Host& host, HINSTANCE instance, std::wstring_view status = {}) {
    auto state = std::make_shared<State>();
    host.state = state; // own partial construction before anything can fail
    state->retirement = host.watchdog.control;
    state->ui_thread = GetCurrentThreadId(); state->generation = generation_counter++; state->fault = host.fault;
    if (!state->wake) throw std::runtime_error("owned wake event creation failed");
    if (!status.empty()) state->status = status;
    for (int i = 0; i < nodes; ++i) {
        auto item = std::make_unique<Provider>(state, i); state->providers[i] = item.get(); host.owners[i].Attach(item.release());
    }
    if (host.fault == 1) return false; // injected failure after all five allocations
    auto window = CreateWindowExW(0, L"NSTU.NativeUiaProbe", L"NSTU UIA feasibility — standalone probe", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 720, 390, nullptr, nullptr, instance, nullptr);
    if (!window) return false;
    state->identity = state->hwnd = window; state->window_retired = false; state->map_cleared = false;
    { std::lock_guard lock(registry_mutex); registry = state; }
    if (host.fault == 2 || FAILED(UiaHostProviderFromHwnd(window, &state->host_provider))) return false;
    state->ready = state->ever_ready = true;
    ShowWindow(window, SW_SHOWNOACTIVATE); update_bounds(state); return true;
}
void disconnect(Host& host) {
    host.watchdog.control->arm();
    auto state = host.state;
    if (!state) return;
    { std::lock_guard lock(state->mutex); if (state->cleanup_attempted) return; }
    if ((InSendMessageEx(nullptr) & (ISMEX_REPLIED | ISMEX_SEND)) == ISMEX_SEND) {
        critical_notice(state, Notice::cleanup); return;
    }
    HRESULT result = S_OK;
    for (auto& item : host.owners) if (item && state->ever_ready) {
        if (host.fault == 12) {
            std::cout << "OWN LOCAL retirement hook entered\n" << std::flush;
            std::lock_guard lock(state->mutex); // prove watchdog never needs this provider lock
            Sleep(INFINITE); // controlled own hook before real disconnect, never an OS COM hang
        }
        if (host.fault == 14) std::cout << "OWN LOCAL pre-call failure hook entered\n" << std::flush;
        const auto hr = host.fault == 14 ? E_FAIL : UiaDisconnectProvider(item.Get());
        if (FAILED(hr)) {
            // A real failure (or pre-call failure seam14) establishes no callback
            // quiescence. Retain identity/ownership and contain this own process.
            // Fault11 remains a separate post-success reporting failure.
            std::_Exit(3);
        }
    }
    // Fault wrapper reports failure after actual cleanup, so test failures do
    // not deliberately leak real UIA references.
    if (host.fault == 11) result = E_FAIL;
    { std::lock_guard lock(state->mutex); state->disconnect_result = result;
      state->cleanup_attempted = true; state->disconnected = SUCCEEDED(result); state->changed.notify_all(); }
}
void release_host(Host& host) {
    if (!host.state) return;
    { std::lock_guard lock(host.state->mutex); host.state->providers.fill(nullptr);
      const auto ordinary=static_cast<unsigned>(host.state->requests.size()),controls=static_cast<unsigned>(host.state->controls.size());
      for(const auto& request:host.state->requests)record_removal(*host.state,10,*request,ordinary,controls);
      for(const auto& request:host.state->controls)record_removal(*host.state,10,*request,ordinary,controls);
      host.state->requests.clear(); host.state->controls.clear(); }
    for (auto& item : host.owners) item.Reset();
    // Cached host metadata is immutable and owned until State's last reference.
    { std::lock_guard lock(registry_mutex); if (registry == host.state) registry.reset(); }
    host.state.reset();
}
bool pump_once(Host& host, bool inject_wait_failure = false) {
    auto state = host.state;
    const DWORD wait = inject_wait_failure ? WAIT_FAILED : MsgWaitForMultipleObjectsEx(
        state->wake ? 1 : 0, state->wake ? &state->wake : nullptr, 25, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    bool failed = wait == WAIT_FAILED;
    MSG message{};
    for (int count = 0; count < 256 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++count) {
        if (message.message == WM_QUIT) { failed = true; continue; }
        if (!message.hwnd && (message.message == done_message || message.message == cleanup_message)) continue;
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    if (failed) { host.result = 2; std::cerr << "Own message pump failure/quit: retiring host\n"; }
    return !failed;
}
void run_host(Host& host, bool fault_worker = false, void (*idle_seam)(Host&) = nullptr) {
    auto state = host.state;
    bool fault_triggered = false;
    if (idle_seam) idle_seam(host);
    while (true) {
        bool closed{}, finished{}, start{}, notification_failure{};
        unsigned pending{};
        { std::lock_guard lock(state->mutex); closed = state->closing; finished = state->worker_finished;
          start = state->start_requested; notification_failure = state->notification_failed;
          pending = static_cast<unsigned>(std::count_if(state->requests.begin(), state->requests.end(),
              [](const auto& item) { return item->completion == Completion::pending; })); }
        if (notification_failure) { host.result = 2; if (!closed) close_window(state); }
        if (start && !closed && !notification_failure) {
            try { start_worker(host, fault_worker); }
            catch (...) { host.result = 2; close_window(state); }
        }
        if (!fault_triggered && pending && host.fault >= 4 && host.fault != 6) {
            fault_triggered = true;
            if (host.fault == 4) PostQuitMessage(2); // actual own queue quit while Value waits
            else if (host.fault != 5) close_window(state);
        }
        { std::lock_guard lock(state->mutex); finished = state->worker_finished; }
        if (host.worker_started && finished) {
            host.watchdog.control->arm(); // completion-to-join is part of retirement too
            if (host.worker.joinable()) host.worker.join(); // state, not message, is authoritative
            { std::lock_guard lock(state->mutex);
              if (state->worker_result || state->notification_failed) host.result = 2; }
            if (!state->closing) close_window(state);
        }
        { std::lock_guard lock(state->mutex); closed = state->closing; finished = state->worker_finished; }
        if (closed) {
            disconnect(host); // outside input-synchronous handler, while owning refs stay live
            if (finished) return;
        }
        if (host.worker_started && !finished && worker_now() >= host.worker_deadline) {
            host.result = 2;
            close_window(state);
            return; // unified shutdown keeps pumping, never joins an active UI-dependent worker
        }
        const bool inject_failure = host.fault == 5 && fault_triggered;
        { std::lock_guard lock(state->mutex); ++state->pump_cycle;state->first_front_pop=state->last_front_pop=0; }
        if (!pump_once(host, inject_failure)) { close_window(state); return; }
        publish_focus(state);
        dispatch(state); // 25 ms fallback even if an optional queue hint is lost
        { std::lock_guard lock(state->mutex); if(state->first_front_pop)record_diagnostic(*state,11); }
    }
}
void shutdown_host(Host& host) noexcept {
    if (host.retired) return;
    host.retired = true;
    host.watchdog.control->arm(); // covers all synchronous teardown/release, not just pump polling
    if (!host.state) {
        for (auto& item : host.owners) item.Reset();
        if (host_pointer == &host) host_pointer = nullptr;
        host.watchdog.finish();
        return;
    }
    try {
        auto state = host.state;
        close_window(state); // mark closing, reject handoff, cancel/wake, destroy/map-clear
        while (true) {
            disconnect(host);
            bool finished{};
            { std::lock_guard lock(state->mutex); finished = state->worker_finished; }
            if (finished) break;
            pump_once(host);
        }
        if (host.worker.joinable()) host.worker.join();
        disconnect(host);
        { std::lock_guard lock(state->mutex);
          host.final_worker_result = state->worker_result;
          host.final_window_retired = state->window_retired && state->map_cleared;
          host.final_worker_retired = state->worker_finished && !host.worker.joinable();
          if (host.fault >= 4 && host.fault != 6 && state->worker_result != 0) host.final_worker_retired = false;
          if (FAILED(state->disconnect_result) || state->notification_failed || !host.final_window_retired) host.result = 2; }
        if (host_pointer == &host) host_pointer = nullptr;
        release_host(host);
        state.reset(); // cached COM host, wake event and state release before caller's COM RAII
        while (live_providers.load() || live_states.load()) Sleep(5);
        host.final_refs_retired = live_providers.load() == 0 && live_states.load() == 0;
        host.watchdog.finish(); // only after cached host, state/event and providers fully retire
    } catch (...) {
        std::_Exit(3);
    }
}
}

std::uint64_t probe_begin_focus_epoch(HWND window) {
    const auto state=lookup(window);if(!state)return 0;
    std::lock_guard lock(state->mutex);if(state->closing)return 0;
    state->diagnostic_epoch=++diagnostic_epochs;state->diagnostic_count=0;state->diagnostic_sequence=0;
    state->diagnostic_lost=false;state->diagnostic_armed=true;state->active_os_actor=state->recent_os_actor=0;
    state->first_front_pop=state->last_front_pop=0;
    record_diagnostic(*state,0,nullptr,S_OK,-1,static_cast<unsigned>(ProbeActorStage::ready));
    return state->diagnostic_epoch;
}
void probe_actor_mark(HWND window,std::uint64_t epoch,std::uint64_t actor,
                      ProbeActorKind kind,ProbeActorStage stage,HRESULT result) {
    const bool enter=stage==ProbeActorStage::call_begin || stage==ProbeActorStage::query_begin;
    const bool leave=stage==ProbeActorStage::call_return || stage==ProbeActorStage::query_return;
    if(enter){
        if(diagnostic_actor_depth<diagnostic_actor_stack.size()){
            diagnostic_actor_stack[diagnostic_actor_depth++]=diagnostic_actor;diagnostic_actor={epoch,actor,kind};
        } else ++diagnostic_actor_overflow;
    }
    try {
        const auto state=lookup(window);
        if(state){std::lock_guard lock(state->mutex);
            if(state->diagnostic_epoch==epoch && state->diagnostic_armed){
                if(diagnostic_actor_overflow)state->diagnostic_lost=true;
                if(enter && kind==ProbeActorKind::os_value_client){
                    if(state->active_os_actor && state->active_os_actor!=actor)state->diagnostic_lost=true;
                    state->active_os_actor=actor;
                }
                record_diagnostic(*state,0,nullptr,result,-1,static_cast<unsigned>(stage),actor,kind);
                if(leave && kind==ProbeActorKind::os_value_client && state->active_os_actor==actor){
                    state->recent_os_actor=actor;state->active_os_actor=0;
                }
            }
        }
    } catch(...) {} // non-authoritative mark failure cannot alter an action
    if(leave){
        if(diagnostic_actor_overflow)--diagnostic_actor_overflow;
        else if(diagnostic_actor_depth)diagnostic_actor=diagnostic_actor_stack[--diagnostic_actor_depth];
    }
}
ProbeEvidenceRead probe_try_epoch_snapshot(HWND window,std::uint64_t epoch,std::uint64_t actor,
                                          ProbeEpochSnapshot& output,bool disarm) noexcept {
    try {
        std::unique_lock registry_lock(registry_mutex,std::try_to_lock);
        if(!registry_lock.owns_lock())return ProbeEvidenceRead::busy;
        auto state=registry;
        if(!state || state->identity!=window)return ProbeEvidenceRead::invalid;
        registry_lock.unlock();
        std::unique_lock lock(state->mutex,std::try_to_lock);
        if(!lock.owns_lock())return ProbeEvidenceRead::busy;
        if(disarm && state->diagnostic_epoch==epoch && state->diagnostic_armed)
            record_diagnostic(*state,0,nullptr,S_OK,-1,static_cast<unsigned>(ProbeActorStage::epoch_end));
        output={};output.epoch=state->diagnostic_epoch;output.count=state->diagnostic_count;
        output.records=state->diagnostic_records;
        if(disarm && state->diagnostic_epoch==epoch) {
            state->diagnostic_armed=false;
            output.disarm_confirmed=true;
        }
        output.armed=state->diagnostic_armed;
        auto& e=output.evidence;
        e.valid=!state->closing && state->hwnd && state->diagnostic_epoch==epoch;
        e.trace_complete=!state->diagnostic_lost;e.generation=state->generation;
        e.held=state->held;e.host_focused=state->host_focused;e.semantic_focus=state->focused;
        e.ordinary_total=static_cast<unsigned>(state->requests.size());e.control_total=static_cast<unsigned>(state->controls.size());
        e.ordinary_pending=static_cast<unsigned>(std::count_if(state->requests.begin(),state->requests.end(),[](const auto& r){return r->completion==Completion::pending;}));
        e.event_sequence=state->diagnostic_sequence;
        for(unsigned i=0;i<output.count;++i){const auto& r=output.records[i];
            if(r.actor==actor && r.action==static_cast<unsigned>(Action::focus) && r.phase==2)e.target_token=r.token;
        }
        std::uint64_t interval_begin=0,interval_end=~std::uint64_t{0};
        for(unsigned i=0;i<output.count;++i){const auto& r=output.records[i];
            if(r.phase==0 && r.stage==static_cast<unsigned>(ProbeActorStage::latch_active))interval_begin=r.sequence;
            if(r.phase==0 && r.stage==static_cast<unsigned>(ProbeActorStage::assertion_return) && r.sequence>interval_begin)
                interval_end=r.sequence;
        }
        for(unsigned i=0;i<output.count;++i){const auto& r=output.records[i];
            if(e.target_token && r.token==e.target_token){
                e.target_completion=r.completion;e.target_result=r.result;
                if(r.phase==6)e.target_mutated=true;
                if(r.phase>=7 && r.phase<=10){
                    e.removal_reason=r.phase==7?ProbeRemovalReason::front_pop:r.phase==8?ProbeRemovalReason::prune_completed:
                        r.phase==9?ProbeRemovalReason::notify_rollback:ProbeRemovalReason::retirement_clear;
                    e.removal_sequence=r.sequence;e.pump_cycle=r.pump;e.target_result=r.result;
                }
            }
            if(r.sequence>interval_begin && r.sequence<=interval_end && r.phase==6 &&
               r.action==static_cast<unsigned>(Action::focus) && r.token!=e.target_token &&
               r.before!=r.after && !r.expired && SUCCEEDED(r.result))e.other_valid_mutation=true;
            if(r.sequence>interval_begin && r.sequence<=interval_end &&
               ((r.phase==1 && r.token!=e.target_token) || r.phase==13))e.unplanned_action=true;
        }
        for(unsigned i=0;i<output.count;++i){const auto& r=output.records[i];
            if(r.phase==11 && e.removal_reason==ProbeRemovalReason::front_pop && r.pump==e.pump_cycle &&
               r.sequence>e.removal_sequence && r.first_pop<=e.removal_sequence && r.last_pop>=e.removal_sequence){
                e.post_pump_sequence=r.sequence;e.pump_first_front_pop_sequence=r.first_pop;e.pump_last_front_pop_sequence=r.last_pop;
            }
        }
        for(const auto& request:state->requests)if(request->token==e.target_token)e.target_present=true;
        return ProbeEvidenceRead::copied;
    } catch(...) {return ProbeEvidenceRead::invalid;}
}
std::string probe_format_epoch_snapshot(const ProbeEpochSnapshot& snapshot) {
    const auto& e=snapshot.evidence;
    std::string out="DIAGNOSTIC epoch="+std::to_string(snapshot.epoch)+" complete="+std::to_string(e.trace_complete)+
        " valid="+std::to_string(e.valid)+" armed="+std::to_string(snapshot.armed)+
        " disarm_confirmed="+std::to_string(snapshot.disarm_confirmed)+" generation="+std::to_string(e.generation)+" target="+std::to_string(e.target_token)+" total="+std::to_string(e.ordinary_total)+
        " pending="+std::to_string(e.ordinary_pending)+" removal="+std::to_string(static_cast<unsigned>(e.removal_reason))+
        " pop_seq="+std::to_string(e.removal_sequence)+" post_pump="+std::to_string(e.post_pump_sequence)+
        " target_mutated="+std::to_string(e.target_mutated)+" other_valid="+std::to_string(e.other_valid_mutation)+
        " unplanned="+std::to_string(e.unplanned_action)+"\n";
    out+="seq,pump,token,actor,candidateOS,caller,executing,phase,stage,action,node,completion,origin,input,before,after,held,host,expired,total,pending,controls,totalBefore,controlsBefore,hr,firstPop,lastPop\n";
    for(unsigned i=0;i<snapshot.count;++i){const auto& r=snapshot.records[i];
        out+=std::to_string(r.sequence)+","+std::to_string(r.pump)+","+std::to_string(r.token)+","+std::to_string(r.actor)+","+
            std::to_string(r.candidate_os)+","+std::to_string(r.caller)+","+std::to_string(r.executing)+","+
            std::to_string(r.phase)+","+std::to_string(r.stage)+","+std::to_string(r.action)+","+std::to_string(r.node)+","+
            std::to_string(r.completion)+","+std::to_string(static_cast<unsigned>(r.origin))+","+std::to_string(r.input)+","+
            std::to_string(r.before)+","+std::to_string(r.after)+","+std::to_string(r.held)+","+std::to_string(r.host_focused)+","+
            std::to_string(r.expired)+","+std::to_string(r.total)+","+std::to_string(r.pending)+","+std::to_string(r.controls)+","+
            std::to_string(r.total_before)+","+std::to_string(r.controls_before)+","+std::to_string(r.result)+","+
            std::to_string(r.first_pop)+","+std::to_string(r.last_pop)+"\n";
        if(out.size()>65536)throw std::length_error("diagnostic output incomplete:64KiB cap");
    }
    return out;
}

// Narrow own-process bridge for explicitly labeled direct-contract/fault tests.
HRESULT probe_control(HWND window, int operation, int argument) {
    auto state = lookup(window); if (!state) return unavailable;
    try { auto request = std::make_shared<Request>(); request->action = Action::control; request->control = operation; request->argument = argument;
        return submit(state, request, true); } catch (...) { return E_OUTOFMEMORY; }
}
HRESULT probe_latch(HWND window, bool held, bool fail_post) {
    auto state = lookup(window); if (!state) return unavailable;
    std::lock_guard lock(state->mutex); if (state->closing) return unavailable;
    state->held = held; state->fail_post = fail_post;
    Request mark; mark.action = Action::focus; mark.node = state->focused; mark.caller = GetCurrentThreadId();
    mark.deadline = Clock::now() + std::chrono::seconds(1);
    trace_focus(*state, mark, held ? 13 : 15, state->focused, S_OK); // exact latch activation/release snapshot
    record_diagnostic(*state,0,nullptr,S_OK,state->focused,
        static_cast<unsigned>(held?ProbeActorStage::latch_active:ProbeActorStage::latch_release));
    return held || fail_post ? S_OK : notify(*state);
}
unsigned probe_observe(HWND window, int field) {
    auto state = lookup(window); if (!state) return 0;
    std::lock_guard lock(state->mutex);
    if (field == 1) return static_cast<unsigned>(std::count_if(state->requests.begin(), state->requests.end(), [](const auto& item) { return item->completion == Completion::pending; }));
    if (field == 2) return state->count;
    if (field == 3) return state->closing && !state->hwnd;
    if (field == 4) return state->disconnected;
    if (field == 5) return static_cast<unsigned>(state->focused);
    if (field == 6) return static_cast<unsigned>(state->requests.size());
    if (field == 7) return static_cast<unsigned>(state->controls.size());
    return 0;
}
void probe_mark_focus(HWND window, int marker) {
    const auto state = lookup(window); if (!state) return;
    std::lock_guard lock(state->mutex);
    Request mark; mark.action = Action::focus; mark.node = state->focused; mark.caller = GetCurrentThreadId();
    mark.deadline = Clock::now() + std::chrono::seconds(1);
    trace_focus(*state, mark, 10 + marker, state->focused, S_OK);
    record_diagnostic(*state,0,nullptr,S_OK,state->focused,
        static_cast<unsigned>(marker==1?ProbeActorStage::baseline:ProbeActorStage::expiry));
}
bool probe_focus_cancellation_proven(HWND window) {
    const auto state = lookup(window); if (!state) return false;
    std::lock_guard lock(state->mutex);
    const auto count = state->focus_trace_count;
    const auto begin = count > state->focus_trace.size() ? count - static_cast<unsigned>(state->focus_trace.size()) : 0;
    unsigned baseline{}, canceled{}; bool marked{}, ended{};
    for (auto i = begin; i < count; ++i) {
        const auto& item = state->focus_trace[i % state->focus_trace.size()];
        if (item.phase == 11) { baseline = i; marked = true; canceled = 0; ended = false; }
        if (marked && i > baseline && item.phase == 2 && item.result == timeout && item.held) canceled = item.token;
        if (marked && item.phase == 12) ended = true;
    }
    if (!marked || !ended || !canceled) return false;
    return std::none_of(state->focus_trace.begin(), state->focus_trace.end(),
        [canceled](const auto& item) { return item.token == canceled && item.phase == 3; });
}
std::string probe_focus_trace(HWND window) {
    const auto state = lookup(window); if (!state) return "own focus state unavailable\n";
    std::array<FocusTrace, 64> snapshot{}; std::array<FocusTrace, 32> outstanding{};
    unsigned count{}, queued{}; int focused{}; bool held{}, host_focused{};
    { std::lock_guard lock(state->mutex); snapshot = state->focus_trace; count = state->focus_trace_count;
      focused = state->focused; held = state->held; host_focused = state->host_focused;
      for (const auto& request : state->requests) if (request->action == Action::focus && queued < outstanding.size())
          outstanding[queued++] = {GetTickCount64(), request->token, request->caller, 0, request->input_origin,
              static_cast<int>(request->completion), request->node, focused, focused, held,
              Clock::now() >= request->deadline, request->result}; }
    std::string result = "Own focus trace: phases0=submit,1=complete,2=cancel,3=mutation,4=rejected,11=pre-latch,12=post-timeout,13=latch-active,15=latch-release\n";
    result += "Snapshot focused=" + std::to_string(focused) + " held=" + std::to_string(held) +
        " actual_host_focus=" + std::to_string(host_focused) + " queued_focus=" + std::to_string(queued) + "\n";
    const auto begin = count > snapshot.size() ? count - static_cast<unsigned>(snapshot.size()) : 0;
    for (auto i = begin; i < count; ++i) {
        const auto& item = snapshot[i % snapshot.size()];
        result += "tick=" + std::to_string(item.tick) + " token=" + std::to_string(item.token) +
            " caller=" + std::to_string(item.caller) + " executing=" + std::to_string(item.executing) +
            " input=" + std::to_string(item.input) + " phase=" + std::to_string(item.phase) +
            " requested=" + std::to_string(item.node) + " before=" + std::to_string(item.before) +
            " after=" + std::to_string(item.after) + " held=" + std::to_string(item.held) +
            " expired=" + std::to_string(item.expired) + " hr=" + std::to_string(item.result) + "\n";
    }
    for (unsigned i = 0; i < queued; ++i) {
        const auto& item = outstanding[i];
        result += "Outstanding token=" + std::to_string(item.token) + " caller=" + std::to_string(item.caller) +
            " input=" + std::to_string(item.input) + " requested=" + std::to_string(item.node) +
            " completion=" + std::to_string(item.phase) + " expired=" + std::to_string(item.expired) +
            " hr=" + std::to_string(item.result) + "\n";
    }
    return result; // format/I/O never runs under the provider lock
}
HRESULT probe_provider(HWND window, int node, REFIID iid, void** result) {
    if (!result) return E_POINTER;
    *result = nullptr;
    auto state = lookup(window); if (!state || node < 0 || node >= nodes) return E_INVALIDARG;
    std::lock_guard lock(state->mutex); auto item = state->providers[node];
    return item ? item->QueryInterface(iid, result) : unavailable;
}
ProbeFaultOutcome probe_fault_scenario(int fault) {
    const auto start = Clock::now();
    const auto created_before = providers_created.load(), retired_before = providers_retired.load();
    ProbeFaultOutcome outcome;
    {
        Host host; host.automatic = true; host.fault = fault; host_pointer = &host;
        try {
            if (!create_window(host, GetModuleHandleW(nullptr))) throw std::runtime_error("injected/actual partial creation failure");
            if (fault == 6) {
                ComPtr<IInvokeProvider> selftest;
                host.owners[4].As(&selftest);
                if (FAILED(selftest->Invoke())) throw std::runtime_error("selftest enqueue failed");
            } else {
                { std::lock_guard lock(host.state->mutex); host.state->held = fault >= 4; }
                start_worker(host, fault >= 4);
            }
            run_host(host, true);
        } catch (const std::exception& error) { host.result = 2; std::cerr << "Injected host setup/loop failure " << fault << ": " << error.what() << '\n'; }
        shutdown_host(host); // explicit plus destructor fallback use exactly the same path
        outcome.status = host.result;
        outcome.window_retired = host.final_window_retired;
        outcome.worker_retired = host.final_worker_retired;
        { std::lock_guard lock(registry_mutex); outcome.registry_cleared = !registry && !host_pointer; }
        outcome.before_com_uninitialize = host.final_refs_retired && !host.state &&
            std::all_of(host.owners.begin(), host.owners.end(), [](const auto& item) { return !item; });
    }
    outcome.created = providers_created.load() - created_before;
    outcome.retired = providers_retired.load() - retired_before;
    outcome.remaining = live_providers.load();
    outcome.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    return outcome;
}
bool probe_regression(int scenario) {
    if (scenario == 1) {
        Host host; host.automatic = true; host_pointer = &host;
        if (!create_window(host, GetModuleHandleW(nullptr))) return false;
        run_host(host, false, [](Host& own_host) {
            simulated_idle_ms = 61000; // simulated idle, real Invoke/start/client path
            ComPtr<IInvokeProvider> invoke; own_host.owners[4].As(&invoke);
            if (FAILED(invoke->Invoke())) throw std::runtime_error("delayed own selftest Invoke");
        });
        shutdown_host(host); simulated_idle_ms = 0;
        return host.result == 0 && host.final_refs_retired;
    }
    if (scenario == 2) {
        // MinGW initializes process-wide exception synchronization handles on
        // the first throw. Warm only that runtime before measuring State-owned
        // acquisitions; retain exact before/after equality for all three cases.
        try { throw std::bad_alloc(); } catch (const std::bad_alloc&) {}
        try { throw std::runtime_error("exception runtime warmup"); } catch (const std::runtime_error&) {}
        DWORD handles_before{}, handles_after{};
        if (!GetProcessHandleCount(GetCurrentProcess(), &handles_before)) return false;
        const auto created = wake_created.load(), closed = wake_closed.load();
        try { const auto aborted = std::make_shared<State>(1); return false; }
        catch (const std::bad_alloc&) {}
        try { const auto aborted = std::make_shared<State>(2); return false; }
        catch (const std::runtime_error&) {}
        try { const auto aborted = std::make_shared<State>(3); return false; }
        catch (const std::runtime_error&) {}
        const bool counted = GetProcessHandleCount(GetCurrentProcess(), &handles_after) != FALSE;
        std::cout << "Own construction handles: before=" << handles_before << ", after=" << handles_after
            << ", created=" << wake_created.load() - created << ", closed=" << wake_closed.load() - closed << '\n';
        return counted && handles_before == handles_after &&
            wake_created.load() - created == 2 && wake_closed.load() - closed == 2 && live_states.load() == 0;
    }
    if (scenario == 3) {
        auto state = std::make_shared<State>(); state->fault = 6;
        diagnostic_entered = false; diagnostic_release = false;
        std::jthread reporter([state] { critical_notice(state, Notice::start); });
        const auto deadline = Clock::now() + std::chrono::seconds(1);
        while (!diagnostic_entered.load() && Clock::now() < deadline) Sleep(1);
        const bool entered = diagnostic_entered.load();
        bool unlocked = state->mutex.try_lock(), updated{};
        if (unlocked) { updated = state->notification_failed && !state->start_requested; state->mutex.unlock(); }
        diagnostic_release = true; reporter.join();
        return entered && unlocked && updated;
    }
    if (scenario == 4) {
        Host host; host_pointer = &host;
        if (!create_window(host, GetModuleHandleW(nullptr))) return false;
        ComPtr<IRawElementProviderSimple> root = host.owners[0], child = host.owners[2], before;
        if (FAILED(root->get_HostRawElementProvider(&before)) || !before) return false;
        ComPtr<IRawElementProviderFragment> fragment; child.As(&fragment);
        struct Array { SAFEARRAY* value{}; ~Array() { if (value) SafeArrayDestroy(value); } } runtime_before, runtime_after;
        if (FAILED(fragment->GetRuntimeId(&runtime_before.value)) || !runtime_before.value) return false;
        close_window(host.state); disconnect(host);
        ProviderOptions options{};
        if (FAILED(root->get_ProviderOptions(&options)) ||
            options != static_cast<ProviderOptions>(ProviderOptions_ServerSideProvider | ProviderOptions_UseComThreading)) return false;
        if (FAILED(fragment->GetRuntimeId(&runtime_after.value)) || !runtime_after.value) return false;
        for (LONG i = 0; i < 3; ++i) {
            LONG before_id{}, after_id{};
            if (FAILED(SafeArrayGetElement(runtime_before.value, &i, &before_id)) ||
                FAILED(SafeArrayGetElement(runtime_after.value, &i, &after_id)) || before_id != after_id) return false;
        }
        ComPtr<IRawElementProviderFragmentRoot> retained_root;
        if (FAILED(fragment->get_FragmentRoot(&retained_root)) || !retained_root) return false;
        fragment.Reset(); retained_root.Reset();
        std::atomic<bool> metadata_ok{true}, delayed_refs_seen{};
        std::jthread final_refs([owned_root = std::move(root), owned_child = std::move(child),
            identity = before.Get(), &metadata_ok, &delayed_refs_seen]() mutable {
            const auto apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (FAILED(apartment)) metadata_ok = false;
            for (int i = 0; i < 40; ++i) {
                ComPtr<IRawElementProviderSimple> after;
                if (FAILED(owned_root->get_HostRawElementProvider(&after)) || after.Get() != identity) metadata_ok = false;
                VARIANT property{};
                if (owned_root->GetPropertyValue(UIA_NamePropertyId, &property) != unavailable) metadata_ok = false;
                VariantClear(&property);
                if (live_providers.load() >= 2 && live_states.load() == 1) delayed_refs_seen = true;
                Sleep(2);
            }
            owned_child.Reset(); owned_root.Reset();
            if (SUCCEEDED(apartment)) CoUninitialize();
        });
        before.Reset(); // no test-owned cached COM reference escapes shutdown
        shutdown_host(host); final_refs.join();
        return metadata_ok && delayed_refs_seen && host.final_refs_retired && live_states.load() == 0;
    }
    return false;
}
int probe_blocked_retirement_child(int fault) {
    if (fault == 15) {
        std::cout << "OWN LOCAL watchdog creation hook entered\n" << std::flush;
        RetirementWatchdog creation_failure(true);
        return 0;
    }
    Host host; host_pointer = &host; host.fault = fault;
    if (!create_window(host, GetModuleHandleW(nullptr))) return 2;
    std::jthread delayed_provider;
    if (fault == 13) delayed_provider = std::jthread([retained = host.owners[0]]() mutable {
        const auto apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(apartment)) std::_Exit(2);
        Sleep(100); retained.Reset(); // last State/cached-host release occurs inside provider destruction
        CoUninitialize();
    });
    shutdown_host(host);
    std::cout << "UNSAFE normal apartment teardown\n";
    return 0;
}
int probe_competing_focus_scenario(int diagnostic_mode = 0) {
    Host host; host.automatic = true; host_pointer = &host;
    if (!create_window(host, GetModuleHandleW(nullptr))) return 2;
    start_worker(host, false, diagnostic_mode==0, diagnostic_mode);
    run_host(host);
    shutdown_host(host);
    return host.result;
}
namespace {
struct Apartment {
    HRESULT result{CoInitializeEx(nullptr, COINIT_MULTITHREADED)};
    ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};
struct RegisteredClass {
    HINSTANCE instance{}; bool registered{};
    explicit RegisteredClass(HINSTANCE value) : instance(value) {
        WNDCLASSW wc{}; wc.lpfnWndProc = window_proc; wc.hInstance = instance;
        wc.lpszClassName = L"NSTU.NativeUiaProbe"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        registered = RegisterClassW(&wc) != 0;
    }
    ~RegisteredClass() { if (registered) UnregisterClassW(L"NSTU.NativeUiaProbe", instance); }
};
}
int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR command, int) {
    Apartment apartment; // declared first: every host/class retires before COM last
    if (FAILED(apartment.result)) return 2;
    RegisteredClass window_class(instance);
    if (!window_class.registered) return 2;
    if (std::string_view(command) == "--fault-test") return run_fault_tests();
    if (std::string_view(command) == "--regression-idle") return run_regression_tests(1);
    if (std::string_view(command) == "--regression-event") return run_regression_tests(2);
    if (std::string_view(command) == "--regression-diagnostic") return run_regression_tests(3);
    if (std::string_view(command) == "--regression-metadata") return run_regression_tests(4);
    if (std::string_view(command) == "--retirement-child") return probe_blocked_retirement_child(12);
    if (std::string_view(command) == "--retirement-release-child") return probe_blocked_retirement_child(13);
    if (std::string_view(command) == "--retirement-failure-child") return probe_blocked_retirement_child(14);
    if (std::string_view(command) == "--retirement-creation-child") return probe_blocked_retirement_child(15);
    if (std::string_view(command) == "--retirement-test") return run_retirement_child_tests();
    if (std::string_view(command) == "--focus-competing-test") return probe_competing_focus_scenario();
    if (std::string_view(command) == "--focus-isolated-test") return probe_competing_focus_scenario(1);
    if (std::string_view(command) == "--focus-os-interleave-test") return probe_competing_focus_scenario(2);
    int result = 2;
    bool session_failed = false;
    const bool controlled_child=std::string_view(command)=="--focus-failure-capture-child";
    if(controlled_child && run_focus_evidence_contract_tests()!=0)return 2;
    const bool automatic = std::string_view(command) == "--self-test" || controlled_child;
    std::wstring result_text;
    try {
        while (true) {
            bool show_result = false;
            {
                Host host; host.automatic = automatic; host_pointer = &host;
                try {
                    if (!create_window(host, instance, result_text)) throw std::runtime_error("partial creation failure");
                    if (automatic) start_worker(host,false,false,controlled_child?3:0);
                    run_host(host);
                } catch (const std::exception& error) {
                    host.result = 2; std::cerr << "FAIL: owned host operation: " << error.what() << '\n';
                }
                shutdown_host(host);
                result = host.result;
                if(controlled_child) {
                    bool registry_cleared{};
                    { std::lock_guard lock(registry_mutex);registry_cleared=!registry && !host_pointer; }
                    std::cout<<"CONTROLLED-CHILD retirement process_result="<<result
                        <<" worker_result="<<host.final_worker_result<<" nonzero_retained="<<(result!=0)
                        <<" window_retired="<<host.final_window_retired<<" registry_cleared="<<registry_cleared
                        <<" worker_retired="<<host.final_worker_retired<<" providers_retired="<<(live_providers.load()==0)
                        <<" state_retired="<<(live_states.load()==0)<<" refs_retired="<<host.final_refs_retired<<'\n';
                }
                session_failed = session_failed || result != 0;
                show_result = !automatic && host.worker_started;
                if (automatic && host.final_refs_retired) std::cout << "PASS cleanup: all five provider objects released before COM exit\n";
            }
            if (!show_result) break;
            result_text = result == 0 ? L"Own-window probe self-test: PASS. NSTU / AT / IME: NOT TESTED." :
                L"Own-window probe self-test: FAIL. NSTU / AT / IME: NOT TESTED.";
        }
    } catch (...) { std::cerr << "FAIL: host construction/reporting exception\n"; result = 2; }
    return session_failed ? 2 : result; // class then COM RAII; no early COM exit or hidden earlier failure
}
