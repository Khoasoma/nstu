# Native UI Automation feasibility probe

The tested small Windows UIA provider boundary works under the local GNU 15.2
strict Release build. Actual NSTU, Narrator/NVDA, TextPattern, physical Vietnamese
IME and mixed-DPI workflows remain NOT TESTED. This standalone probe does not
modify the teacher app, student app, services, exam policy or ImGui framework.

Open `nstu-native-uia-probe.exe` normally. The window offers a harmless command,
a Value-pattern sample, status, and **Run own-window self-test (F5)**. Mouse,
Tab and Enter operate the sample commands. The Value sample intentionally has
no keyboard text editor, caret, selection, TextPattern or IME implementation;
its contents can be replaced through UIA Value. The self-test retires its own
window to exercise teardown, then shows a fresh window with the result. An
automated own-HWND GUI smoke launched with no arguments, invoked that command,
observed the fresh PASS status and closed normally with exit 0. Actual physical
manual GUI and AT acceptance remain pending; no terminal is required by a user.

Only the exact HWND owned by this executable is passed to ElementFromHandle;
navigation and event subscriptions stay in that subtree. There is no desktop
enumeration, global focus subscription, remote input, screen capture, privileged
operation, AT installation or system configuration change. Build-generated
resources declare this process nonadmin/asInvoker and PerMonitorV2; they do not
change system DPI settings. The local executable statically links its GNU runtime
and imports only Windows system DLLs, so an ordinary launch needs no UCRT64 PATH.

## Implemented and tested contract

* Five explicit semantic IDs: root, harmless command, single-line value, status,
  and self-test. Child runtime IDs append the fragment generation and node ID;
  labels, screen coordinates and frame-local pointers never define identity.
* Simple/fragment/root providers expose names, roles, control/content flags,
  enabled/read-only/focus state, bounds, hit testing, navigation, Invoke and Value.
  Unsupported TextPattern returns null; the observed OS GetCurrentPattern result
  was S_OK with a null interface, so tests verify absence through the interface.
* Own UI and provider/client threads use COM MTA. ProviderOptions explicitly
  advertises ServerSideProvider | UseComThreading. Provider methods read bounded
  synchronized snapshots; actual GetFocus is sampled on the own UI thread.
* Actions use stable identity/generation and recheck current guards on the UI
  thread. Invoke reports bounded enqueue acceptance immediately. SetValue and
  SetFocus return the actual completion HRESULT, never successful enqueue alone.
  The ordinary action queue holds at most 32 requests. Technical controls use
  a separate queue bounded to eight so tests can release a held action queue.
* Values allow at most 1,024 valid UTF-16 code units without embedded NUL, CR/LF
  or unpaired surrogates. Requests expire after 750 ms. Mutation and pending
  cancellation serialize on one lock; canceled requests never apply later.
  Own-UI result-bearing calls execute directly without waiting for themselves.
* Closing rejects new actions/handoff, invalidates the live HWND/focus/bounds,
  cancels and wakes pending callers, destroys only the own window, clears the
  HWND event map, then disconnects outside input-synchronous SendMessage handling.
  The UI pump stays alive while clients remove their own subscriptions and refs
  on the originating MTA. App refs and COM apartments retire last.

The actual Windows disconnect experiment refined the plan's blanket retired
getter wording: **immutable transport/identity getters must remain available
through disconnect**—ProviderOptions, cached HostRawElementProvider, GetRuntimeId
and FragmentRoot. Rejecting the latter three produced ELEMENTNOTAVAILABLE from
UiaDisconnectProvider. Returning their immutable identity allowed successful
disconnection for root and all children. No cleanup getter queries the destroyed
HWND. Semantic properties, bounds, navigation, focus, patterns and actions reject
retirement; retained OS and direct interfaces are tested after real destruction.
Independent review admitted this narrow metadata exception against the pinned
Windows experiment. It is not a universal guarantee for every Windows build.

Host retirement now uses one explicit and RAII-backed shutdown path, including
partial creation, automatic thread-start exceptions, normal close and pump
failure/quit. State ownership is assigned before provider/window/host acquisition.
Closing cancels pending Value/focus calls and disconnects outside input-synchronous
handling while owned refs remain live. The pump continues until the worker has
removed subscriptions and released interfaces on its original MTA. Only then may
the UI thread join, clear synchronized weak registry/provider links, and release
app refs. Cached-host metadata stays immutable until the last State reference
ends. Provider and State counters change after their potentially blocking owned
COM releases complete. Both lifetimes and the owned event must retire before
COM RAII can exit. The unnamed event has an unwind-safe owner even when later
State string initialization or acquisition validation throws.
Disconnected/notification failures remain nonzero results, including after the
ordinary GUI displays its result.

Critical start/done/cleanup messages are checked hints. An independent shared
completion/request state, an owned unnamed wake event, and a 25 ms timed
MsgWaitForMultipleObjectsEx/PeekMessage pump provide recovery when a message or
wake signal fails. Failed start cancels unstarted testing state. Lost done/cleanup
cannot leave the loop blocked in GetMessage. The 60-second worker budget starts
at each actual worker admission; ordinary GUI idle time does not consume it.
Diagnostic state updates finish under the mutex, then the diagnostic sink and
stderr output run after unlocking.

An independent watchdog arms before closing, destroying the own HWND, clearing
the event map, disconnecting, joining, or releasing ownership. Its only shared
inputs are independent lock-free atomics; it never uses a provider mutex, the
UI pump, COM or diagnostics. The five-second retirement budget triggers own
process failure exit 3 if retirement cannot finish. It disarms only after both
State and provider references have completed retirement. Watchdog creation
failure and a genuinely failed disconnect also exit 3 before normal apartment
teardown; failed disconnect does not establish callback quiescence.

This is failed-process containment. It does not cancel an OS COM call, bound
COM completion, or provide a hard real-time guarantee under Windows scheduling
or OS failure. Emergency exit performs no diagnostic write first. A contained
failure never counts as successful lifecycle cleanup or capability acceptance.

## Verification evidence

Local evidence is private under this worktree's `build/native01-probe-red`,
`build/native01-probe-release`, `build/native01-probe-r3-release`,
`build/native01-probe-r3-metadata-red` and `_agents/autonomous-resume/operations`.
Base revision: `4d1c12350a4699b489b5a5851dfb95508d97f500`.

The original actual OS-client RED failed with exit 8 because the OS HWND proxy
lacked the custom provider name. The final standalone Release build used
`-std=c++20 -Wall -Wextra -Wpedantic -Werror -UNDEBUG`; the test source rejects
NDEBUG. The preserved R2 configure, build and CTest exited 0; both tests passed in 2.19 seconds
(normal OS-provider suite 1.89 s, lifecycle faults 0.30 s).
It checked OS names/roles/IDs/navigation, exact NFC/NFD Vietnamese and surrogate
round trips, real Invoke/Value events, and separately labeled direct contracts:
focus/hit bounds, read-only/disabled guards, queue/post failures, cancellation,
stale generation, concurrent snapshots, and destruction during a Value wait.
Focus and Value timeouts measured 753 and 756 ms; destruction woke the Value caller in
16 ms; Invoke returned in 0 ms; all five provider objects were released.
Later source changes require fresh evidence; immutable receipts bind exact hashes.

The added lifecycle RED reproduced retained registry/state after stack-unwind
fallback (exit 8). Eleven GREEN injected scenarios cover: window creation after
five allocations, host-provider acquisition, thread start, actual own WM_QUIT
and injected pump failure while a Value caller waits, failed self-test start
message, failed done message, failed cleanup message, failed wake signal, combined
done/cleanup/wake loss, and surfaced disconnect failure. Each reports failure
status 2, retires exactly five providers and all app state/wake ownership, clears
the registry/host pointer, and finishes MTA retirement before COM exit within
five seconds (observed 0–44 ms). The disconnect failure injection reports failure
after real OS cleanup so the test does not deliberately leak UIA references.
Creation cases that fail before provider exposure have no UIA association to
disconnect. Other cases acquire the actual own OS provider and scoped MTA event
subscription, then prove that closing aborts the pending Value and unsubscribes.

R3 captured four matching RED failures before the corresponding corrections:
simulated 61-second GUI idle followed by the real own self-test Invoke/start
path, leaked constructor event ownership, a withheld local diagnostic sink
holding the provider mutex, and an own child blocked before real disconnect.
All four failed (CTest exit 8). A separate isolated copy reintroducing R2's
eager cached-host reset also failed the retained-metadata regression; it does
not alter the corrected product sources.

The appended regressions preserve all original OS/direct and eleven fault
assertions. They cover three constructor/acquisition failures with actual own
process handle counts; state queries while the local sink is withheld; the
four retained identity getters and cached-host stability while final provider
references are delayed; and exact own children for blocked disconnect with the
provider mutex held, blocked last State/cached-host Release with completion
counters still live, pre-call disconnect failure without OS cleanup, and
watchdog-creation failure. Fault 11 remains the distinct post-success reporting
failure, not proof of an actual failed OS disconnect.

The child controller launches the exact compiled application path, waits at
most 8.5 seconds, and checks exact exit 3 and absence of the normal apartment
success path. It accepts at most 64 KiB of own file output through a timed
overlapped read. Controller timeout terminates only its exact owned child handle
and fails the test. These safe local blocking hooks never hang UIAutomationCore
or change a foreign process. The expected five-second exits allow three seconds
of scheduling/startup tolerance. Fresh source/binary/log hashes and actual
elapsed results are pinned in immutable R3 receipts.

The final R3 GNU strict Release build exited 0 with no warnings; all seven
focused CTests passed in 14.57 seconds. The two blocking child hooks exited 3
in 5,044 and 5,049 ms, while pre-call failure and watchdog creation failure
exited 3 in 47 and 25 ms. The noargs/system-only-PATH GUI smoke invoked its own
self-test, observed the new PASS window and closed normally with exit 0.

Two earlier runs failed the original internal-focus cancellation assertion
before causal tracing existed. Their outputs remain preserved; their cause was
not established. Later isolated repeats and the final full focused run passed.
The final tests establish command identity before the latch and retain the
original focus assertions, plus a token-specific cancellation assertion. A
bounded 64-record trace distinguishes provider caller threads, own keyboard or
mouse origins, submission, mutation and cancellation. Records are captured under
the mutex and formatted/emitted after unlocking. The observed expired token had
no mutation record; this does not retroactively explain the earlier failures.
The event regression warms only the first-throw exception runtime before exact
own process handle equality, avoiding MinGW's two lazy runtime handles in that
measurement; both acquired probe events close and no State remains live.

Run the developer-only automated check with:

```text
cmake -S tools/native-uia-probe -B build/native01-probe-release -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM=C:/msys64/ucrt64/bin/mingw32-make.exe -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe
cmake --build build/native01-probe-release
ctest --test-dir build/native01-probe-release --output-on-failure
```

CTest launches only the own executable with developer-only `--self-test` (90 s),
`--fault-test` (75 s), and five correction regressions (30 s each).
The test worker checks a 60-second overall bound, individual bounded event waits,
and 1,750 ms elapsed limits for 750 ms request deadlines (scheduler tolerance).
Forced termination is a failure, not lifecycle acceptance.

This standalone CMake is outside the root project. Normal root gates and current
Windows CI do **not** compile the probe. Standalone MSVC is NOT RUN locally;
the independent Windows owner must execute:

```text
cmake -S tools/native-uia-probe -B build/native01-probe-msvc-release -G "Visual Studio 17 2022" -A x64
cmake --build build/native01-probe-msvc-release --config Release
ctest --test-dir build/native01-probe-msvc-release -C Release --output-on-failure --timeout 90
```

MSVC flags require `/W4 /WX /permissive- /UNDEBUG /utf-8`, C++20 and static CRT.
There is no sanitizer/MSVC/CI/runtime qualification claim from the GNU result.

## Sources and remaining application work

Official sources fetched 2026-11-07:

* [Provider interfaces and events](https://learn.microsoft.com/en-us/windows/win32/winauto/uiauto-serversideprovider), [WM_GETOBJECT and map cleanup](https://learn.microsoft.com/en-us/windows/win32/api/uiautomationcoreapi/nf-uiautomationcoreapi-uiareturnrawelementprovider), [runtime IDs](https://learn.microsoft.com/en-us/windows/win32/api/uiautomationcore/nf-uiautomationcore-irawelementproviderfragment-getruntimeid).
* [MTA/provider options](https://learn.microsoft.com/en-us/windows/win32/api/uiautomationcore/ne-uiautomationcore-provideroptions), [client and event threading](https://learn.microsoft.com/en-us/windows/win32/winauto/uiauto-threading), [own-thread GetFocus](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getfocus), [fragment focus](https://learn.microsoft.com/en-us/windows/win32/api/uiautomationcore/nf-uiautomationcore-irawelementproviderfragment-setfocus).
* [Asynchronous Invoke](https://learn.microsoft.com/en-us/windows/win32/api/uiautomationcore/nf-uiautomationcore-iinvokeprovider-invoke), [provider Value](https://learn.microsoft.com/en-us/windows/win32/api/uiautomationcore/nf-uiautomationcore-ivalueprovider-setvalue), [client Value BSTR](https://learn.microsoft.com/en-us/windows/win32/api/uiautomationclient/nf-uiautomationclient-iuiautomationvaluepattern-setvalue), [scoped property events](https://learn.microsoft.com/en-us/windows/win32/api/uiautomationclient/nf-uiautomationclient-iuiautomation-addpropertychangedeventhandlernativearray), [late callback/refcount lifetime](https://learn.microsoft.com/en-us/windows/win32/api/uiautomationclient/nf-uiautomationclient-iuiautomation-removepropertychangedeventhandler).
* [Disconnect](https://learn.microsoft.com/en-us/windows/win32/api/uiautomationcoreapi/nf-uiautomationcoreapi-uiadisconnectprovider), [input-synchronous detection](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-insendmessageex), [COM last Release](https://learn.microsoft.com/en-us/windows/win32/api/unknwn/nf-unknwn-iunknown-release).
* [Timed message/event wait](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-msgwaitformultipleobjectsex), [owned unnamed event](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createeventw), [checked wake signal](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-setevent).
* [Manifest DPI awareness](https://learn.microsoft.com/en-us/windows/win32/hidpi/setting-the-default-dpi-awareness-for-a-process), [MSVC Unicode source](https://learn.microsoft.com/en-us/cpp/build/reference/utf-8-set-source-and-executable-character-sets-to-utf-8?view=msvc-170).
* [Text/TextRange/caret requirements](https://learn.microsoft.com/en-us/windows/win32/winauto/uiauto-implementingtextandtextrange), [ImGui v1.92 accessibility and text shaping limits](https://github.com/ocornut/imgui/blob/v1.92.0/docs/README.md).

A later separately reviewed application plan must publish stable classroom and
modal identities from real state, dispatch through guarded actual handlers,
preserve role/exam/target guards and focus restoration, handle virtualization,
coalesce events and update real DPI bounds. It must add appropriate text/caret/
selection/IME semantics and verify actual core workflows with Narrator and NVDA.
This probe does not establish those capabilities or production readiness.

## Focus queue diagnostics

The diagnostic extension preserves the existing action, guard, queue and
retirement behavior. It adds a separate fixed 256-record epoch with explicit
loss reporting, direct-caller tags and own-input origins. Marshalled provider
calls remain untagged; a nearby OS-client operation is only a candidate context.
Records contain scalar own-process metadata, not Value text.

Physical queue totals include canceled entries. Front-pop, completed-entry
pruning, notification rollback and retirement clearing have separate records.
A completed UI pump pass records the range of front-pop events after both the
message pass and fallback dispatch. Pending count zero alone is never drain
or callback-quiescence evidence.

The previous four owned-process runs remain preserved evidence for their old
source. Their passing results did not exercise the failure collector and do not
explain the two historical pretrace failures, whose cause remains UNKNOWN.

The corrected bridge reports actual same-epoch disarm and post-disarm armed
state. Final acceptance requires the pinned epoch/generation, complete records
after epoch_end, confirmed disarm and the exact canceled front-pop with its
covering completed pump. Busy, invalid, closed and overflowing reads retain the
last valid fixed snapshot. Failure output separately names collection and
finalization dispositions and the last read; preserved intermediate evidence
never substitutes for a valid complete final epoch. The local catch remains
before COM-owner unwind, with one read-only nonrenewable two-second budget,
the exact primary rethrow and original nonzero cleanup behavior.

The isolated oracle requires focused command1 at baseline, latch and final,
and rejects all action/input contamination and host-focus loss through the
assertion boundary even if host focus later returns. The OS oracle matches the
actual returned HRESULT to an actual node2 Value token, its submit/admission,
completion and mutation records within the candidate OS call interval. Missing
Value records, incompatible candidates or mismatch are nonzero inconclusive
results. An internal focus-only timeout cannot prove Value timeout. COM origin
remains candidate interval consistency, not transported TLS attestation.

The revised campaign has exactly four serial owned-process rows:

| Mode | Required evidence |
|---|---|
| --focus-failure-capture-child | Pure fixed-data negative contracts before UI; then real legitimate control8 competitor, unchanged original scalar failure, read-only collector, complete canceled target and valid competitor evidence, EXPECTED_CONTROLLED_ERROR, actual retirement and preserved nonzero |
| --self-test | Original ordinary action flow and assertions, exit0, corrected final evidence |
| --focus-isolated-test | Focused baseline/latch/final, original scalar1, canceled pop/post-pump and no final contamination |
| --focus-os-interleave-test | Actual Value result supported by complete token evidence, canceled direct target, candidate OS association |

The controlled failure child replaces the refreshed direct-competitor campaign
row; the old --focus-competing-test developer mode and old receipts remain.
Only the selected child adds the legitimate competitor. Its real original
assertion throws naturally; nonzero alone is insufficient qualification.
The root-owned external supervisor captures complete bounded output once per
row, inspects it before the next, and stops on inconclusive/containment failure.
The child and ordinary flow have 90s external ceilings, focused cases 30s;
request 750 ms / check 1750 ms / worker 60 s / retirement watchdog 5 s remain unchanged.
No fifth run, retry, fake throw, forced pop or alternate collector is admitted.

This correction is compiled only pending independent source review and the
root-owned campaign. The pure checks validate scalar adjudication, not Windows
lock contention, deadline durability or historical reproduction. Failure
collection and all new runtime paths remain unexecuted on this source. No
passing fixture substitutes for failed-invocation evidence. Actual NSTU, AT,
TextPattern, IME, mixed-DPI, MSVC and physical/production qualification remain
outside this component correction. No semantic or fixture-boundary repair,
integration or production-readiness claim is made.
