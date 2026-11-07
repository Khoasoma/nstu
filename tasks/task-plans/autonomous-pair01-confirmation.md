# PAIR01 — production student SAS confirmation

Status: PLAN_REJECT / SUPERSEDED. No product edits were made under this plan. Its SHA256 at review was `70D21ADA1A145CD205D37127EAE965703559DBBEEFAB6FDC1934B5E5814FB842`.

Rejection: the teacher's existing `pending_pairings()` hides the SAS until the client confirmation tag arrives, so requiring typed teacher-screen SAS before that tag deadlocks. The revised phase plans `autonomous-pair01a-codec.md`, `autonomous-pair01b-state.md`, `autonomous-pair01c-teacher-view.md`, and `autonomous-pair01d-wiring.md` replace this plan. They retain actual six-digit entry and bilateral human approval, add only a read-only early teacher view, and preserve the existing server approval gate. See `autonomous-pair01-phases.md` for cumulative scope and review sequencing. Do not implement this superseded plan.

Owner: delivery_pairing. Priority: P0. Date: 2026-11-07.

Authoritative read-only source: `D:/Codex/im/work/nstu-ui-reviewed-20261107`.
Baseline HEAD: `edb79e872339e372de9ee662ed71e9a0cacef154`.
Implementation worktree: `C:/Users/xkhoa/.codex/worktrees/autonomous-pair01/nstu-ui-reviewed-20261107`.
Native worktree creation operation: `5891d07a-d983-479e-98cb-429c05bfdb0e`.
Initial status: clean, detached HEAD at the baseline above.

## Requirement and current evidence

The owner mandate requires bilateral human SAS confirmation, preserves discovery/enrollment/authorization separation, and authorizes isolated engineering work and safe tests. `AGENTS.md` says never auto-approve without confirmation on both machines. Applicable skills: NSTU engineering, NSTU delivery loop, native git worktrees, and test-driven development. WORKSPACE_SCOPE.md is absent from this source tree; the coordinator's explicit source and file boundaries govern this task. No services, firewall, host security, real schools, or production environments will be changed.

At the exact baseline:

- `client/service/main.cpp:2522` guards initial discovery with `g_agent_connected`; every selected candidate converges on `attempt_pairing`.
- `client/service/main.cpp:2611` calls `pair_with_server` with a display observer, but passes `stop_token, {}, &error`. Thus its production options contain no `student_confirmation`.
- `client/src/client_pairing.cpp:472` fails closed when that callback is absent. Its confirmation loop is bounded, checks stop/deadline after callback return, sends heartbeats, verifies matching nonempty SAS, and sends the crypto client confirmation only afterward.
- `client/agent/main.cpp:219` displays the SAS and teacher name; its pairing window has only the discovery Connect button. `WM_CLOSE` merely hides the current view. There is no affirmative, refusal, or SAS mismatch reply.
- The existing local IPC has pairing selection/code/status types 25–28, with strict old payload layouts. It has no correlated human-confirmation reply. The service's existing named-pipe accept path validates the installed agent executable and an interactive session before marking it connected.
- The previous student-SAS task result is insufficient evidence for the normal service path: helper tests supply the callback while production does not.

This task will wire the existing cryptographic handshake to an actual student decision. It will not change pairing crypto, network protocol, key storage, registration, authorization, discovery routing, or teacher approval.

## Exact modification allowlist and source SHA256

Existing files (hashes are bytes in this worktree before any edit):

| Path | SHA256 | Intended modification |
|---|---|---|
| `client/service/main.cpp` | `DA69661EADF9431082AE5343102F9E211F7AAC3CC729A2AFF7403862A6C4D1BF` | Replace the display-only pairing invocation with the production interactive-pairing wrapper; dispatch confirmation replies; invalidate pending confirmation on agent disconnect/service stop; remove or skip obsolete queued confirmation messages. |
| `client/agent/main.cpp` | `73A90CC8B0B35B3B81BAE1CF02D436B7286433EBE1C38B4326143F17DB41127C` | Receive correlated requests/end; use the agent confirmation state; add teacher-code edit, explicit Codes match/Codes differ/Reject actions to the existing pairing window; acknowledge actual display on UI thread; handle close/Escape/destruction/expiry/disconnect without approval. Preserve existing native theme/DPI/keyboard conventions. |
| `client/include/nstu/agent_protocol.hpp` | `69C79C505DF69DF0C1D49001D9517135C5E0F0F3F4A506DAABFA6211F295AD2E` | Append local message kinds and bounded request/reply/end payload declarations; retain every existing numeric value and layout. |
| `client/src/agent_protocol.cpp` | `F66F38F5A77E3E81960405A0DA110B6E7A8E1383BFB08C2B12C86E81AF42D738` | Strict additive confirmation payload codecs and message-type range update. No changes to old codecs. |
| `client/CMakeLists.txt` | `0F04741B9C7458E5548DC033B4D15C0BB59A72BBBDB6347E5D9D49E6A61DBD7E` | Add only `src/service_pairing_confirmation.cpp` inside the initial `add_library(nstu_client_platform STATIC ...)` source list. Coordinator granted this single entry; all other sections excluded. |
| `tests/CMakeLists.txt` | `D9D9EA805D2FCCEEEEEBC71DFFA05F5F813ECCC28AEB22D7CBA1066E66345D9F` | Add only `nstu_service_pairing_confirmation_tests`/`nstu.service_pairing_confirmation`, linked to the client/server libraries with existing warnings/assertion helpers, inside the existing `if(TARGET nstu_server_state)` client-test section beside pairing tests. Coordinator granted this target; all other targets/sections excluded. |

New files (baseline ABSENT):

- `client/include/nstu/service_pairing_confirmation.hpp`
- `client/src/service_pairing_confirmation.cpp`
- `client/include/nstu/agent_pairing_confirmation.hpp`
- `tests/service_pairing_confirmation_tests.cpp`
- `tasks/task-plans/autonomous-pair01-confirmation.md` (this plan)
- `tasks/task-plans/autonomous-pair01-confirmation-result.md` (review/gate/CI receipts)

No other paths may be modified. In particular no server main/control plane, common pairing crypto/network codec, `client/src/client_pairing.cpp`, Studio, package stager, global gates, workflow files, policies, private roadmap, README files, assets, Coding Agent checkout/checkpoints/builds, or INDEV status edits. Any necessary scope expansion requires an amended hashed plan and independent acceptance first.

Build ownership: only this worktree's `build/mingw`, `build/gate`, and `build/pair01-*`; logs stay under this worktree's build directory and are not staged. Each build/gate runs sequentially. No builds use the authoritative live checkout or other agents' directories. The unchanged gate writes a shared `/tmp/nstu-cfg.log`: obtain the root's exclusive full-gate lease before either fast/strict gate. Isolated unit builds may start only after PLAN_ACCEPT.

## Design and lifecycle contract

### Compatible local IPC

Append local IPC types 42=request, 43=reply, 44=end after the existing maximum 41. Keep `nstu-control-v1`, frame header, and all old message bytes unchanged. The network pairing wire is untouched. Old peers reject or ignore new kinds and cannot enroll through a fallback; installation already deploys the matched service and agent binaries together. Adding capability negotiation or old-peer auto-approval is out of scope.

Each request contains a fresh random nonzero 128-bit attempt token generated by existing `security::generate_random`, the six ASCII SAS digits, bounded printable teacher name, and an absolute expiry using the same machine's Win32 monotonic tick clock. The service retains the corresponding steady-clock deadline for callback slices. Maximum local confirmation duration is 180 seconds, capped by the existing configured pairing timeout. Tick timestamps are local-only; no time values or new fields go on the network wire.

Reply contains the exact request token, explicit choice, UI-presented flag, and six ASCII digits entered by the student from the teacher screen for affirmative confirmation. Empty/non-digit/malformed affirmative replies refuse. End contains only the token. Strict decoders reject invalid choices, zero tokens, truncated/oversized/trailing data, bad digit counts, invalid flags, and unbounded text/time values. Every old pairing codec remains byte-compatible.

### Service broker and production pairing seam

`ServicePairingConfirmation` owns one attempt and its correlation token, deadline, connected-agent generation, bounded decision, mutex, and condition variable. It never performs pipe/network I/O while holding a mutex. A new attempt or connection reset invalidates the previous token before starting. Outbound message enqueue happens after releasing state locks.

The production wrapper `pair_with_interactive_agent` owns options construction and invokes the existing `pair_with_server` with the broker-backed display observer and student callback. `attempt_pairing` calls this wrapper for every discovery route. The display observer opens one correlated request; the callback returns a result in at most the supplied slice (250 ms maximum), preserves waiting between slices, and refuses unavailable UI, cancellation, disconnect, invalid/mismatched replies, or expired input. It never manufactures a confirmed SAS from its own computed code.

The pipe receive loop delivers only strictly decoded replies to the broker for the currently validated connection generation and validated WTS session/agent process peer. Preserve the existing installed-image/session validation and retain its session identity rather than treating a global connected bit as a transferable authority. A reply for another token, an already consumed decision, or a closed/expired attempt cannot authorize pairing. A same-attempt affirmative with another SAS or without successful presentation becomes refusal. Connection loss wakes waiters and invalidates the attempt before another agent can connect. Service stop does the same. Late confirmed replies after stop/expiry are rejected even if queued before the boundary. The random correlation token has no cryptographic trust/key custody meaning; the existing bilateral SAS remains the root of trust.

The wrapper emits correlated end and invalidates pending state on every return (success, timeout, refusal, server rejection, cancellation, failure). Pending request/reply/end messages are discarded on connection loss or checked against current attempt before transmission so a replacement agent cannot inherit the previous prompt. Existing crypto secret zeroization, teacher confirmation, DPAPI save only after enrolled, and service-only identity persistence remain unchanged.

### Actual interactive action

`AgentPairingConfirmation` is the shared production state used by the existing agent window; its implementation resides alongside the service broker in the single owned `service_pairing_confirmation.cpp` source entry. Receiving a request alone cannot mark it presented or confirmed. The UI thread verifies a visible valid window, successful paint/control creation, and a live request before acknowledging presentation. No UI/paint/control/post-message failure returns affirmative; the service timeout remains a final bound if the UI thread stops responding.

The existing pairing window displays the formatted SAS and teacher name. It asks the student to compare both screens and enter the six digits from the teacher screen, then choose Codes match. The input starts empty; no default code is inserted. Codes differ and Reject are explicit refusal actions. Close, Escape, destruction, expiry, and pipe loss invalidate the local prompt and send refusal when a live pipe/request still exists. Buttons/Enter never auto-confirm without complete teacher-code entry, successful presentation, live token, and matching SAS. A matching student response still waits for the existing teacher approval before enrollment.

The model returns one serialized reply per request at most. End applies only to the matching token so a delayed cleanup for an old attempt cannot close a newer prompt. UI controls follow existing dark theme tokens, per-monitor DPI scaling, visible keyboard focus, and `IsDialogMessageW`; this is a functional extension of the existing approved native window, with no visual redesign.

## Acceptance criteria

1. Every normal production route (one candidate, preferred room, and multiple-candidate selection) converges on the same wrapper with a real callback; no empty-options invocation or automatic confirmation remains in that seam.
2. Both humans must confirm the same SAS. Student affirmative alone cannot enroll without teacher approval. Teacher approval alone cannot bypass student confirmation.
3. No UI, UI presentation failure, refusal, mismatch, timeout, stale/replayed/malformed reply, cancelled attempt, disconnected/replaced agent, and stopped service produce no enrollment/key handoff/save.
4. A stale token after retry, a late reply after expiry, duplicate response, or delayed end cannot affect a new attempt.
5. Each callback wait honors stop and the supplied slice, at most 250 ms; heartbeats remain operational while waiting; no service control lock is held across network/pipe I/O.
6. IPC framing and old pairing payloads remain byte-compatible; no TCP pairing schema, crypto primitive, authorization, or persistence contract is changed.
7. Automated tests exercise the production wrapper with the real client/server cryptographic exchange and the production agent state/codecs, not only the callback evaluator. Failure cases observe empty returned config/no server key; success requires both sides.
8. Fast and strict gates pass with existing assertions preserved; actual MSVC Windows CI must be checked by the coordinator before integration is complete. Local/CI success remains INDEV and does not claim physical lab or production acceptance.

## RED → GREEN implementation sequence and test evidence

1. After PLAN_ACCEPT, capture baseline configure/build/test output before behavior changes. No borrowed outputs. Request exclusive full-gate lease before full gates.
2. First RED test reproduces the production empty-options failure using the current production invocation contract over a real loopback server: observed code plus an agent decision cannot yield enrollment because production fails to wire it. Record the exact failure, then introduce and test the shared production wrapper rather than a test-only callback that bypasses it.
3. Test strict local codecs with hand-derived byte fixtures and preserved old pairing codec bytes. Observe failing tests for new behavior, then implement minimal additive payload support.
4. Test the service broker: no UI/presentation, bounded waits, cancellation, matching/mismatching decisions, expiry before/after reply, connection generation replacement, old/duplicate tokens, supersession, and token-bound end. No test-only production cleanup APIs.
5. Test the actual agent state used by the UI: received is not presented; no empty/autofilled code; matching typed teacher digits accepted only after display; close/Escape/refusal/timeout/disconnect fail closed; old end cannot close new request.
6. Integration test the production wrapper with a real loopback `ServerControlPlane`, real SAS exchange, production IPC request/reply codecs and agent confirmation state. Use a dedicated test named pipe if needed, never `nstu-control-v1`. Assert no server pending approval/key before human student action; after matching student action verify server pending SAS and explicitly approve there. Refusal/mismatch/no UI/timeout/stale/cancel/disconnect all leave returned key empty. Use separate fixture key stores or compare unchanged counts; temp keyring files only.
7. Keep existing `nstu.client_pairing`, `nstu.ui_pairing`, `nstu.client_ipc`, and `nstu.pairing_transport` suites unchanged and passing. Build the actual service and agent targets to prove product wiring compiles. Independent review inspects actual wrapper call and pipe/UI dispatch in the cumulative diff.
8. Run fast and strict full gates once the scoped tests pass. Fix only within allowlist; report outside-scope baseline/environment failures without assertion weakening. Record exit codes, test counts, warnings, and log paths.
9. Send cumulative diff, exact HEAD, hashes, red/green receipts and gate outputs for independent CODE_ACCEPT. Only afterward commit with `khoasoma <songtu438@gmail.com>` and no attribution if the coordinator instructs it. No autonomous push/merge before CODE_ACCEPT.

Commands (Git Bash executable `C:/Program Files/Git/bin/bash.exe`; worktree is explicit):

```sh
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake -S . -B build/mingw -G 'MinGW Makefiles' -DCMAKE_BUILD_TYPE=Debug
cmake --build build/mingw -j 8
ctest --test-dir build/mingw --output-on-failure -R 'nstu\.(service_pairing_confirmation|client_pairing|ui_pairing|client_ipc|pairing_transport)$'
./scripts/verify-gate.sh
./scripts/verify-gate.sh --strict
git diff --check
git diff --name-only
```

Gates run unpiped or redirected with their actual exit status captured. Build/test operations do not install services, activate security policy, or control host applications. Optional native UI smoke is qualified separately on an isolated authorized test environment; current unit/build evidence cannot prove bilateral physical human operation.

## Dependencies and review checkpoint

Coordinator has granted the single client source entry and single test target above. Independent plan review must specifically assess local IPC compatibility, real code-entry semantics, correlation across disconnect/retry, clock/deadline boundary, actual UI presentation, and whether the production wrapper integration test catches removing its callback construction; actual `attempt_pairing` wrapper invocation and pipe/UI dispatch must also be inspected independently. The product author does not accept their own plan or code. If a required change materially breaks protocol compatibility, stop that scope and continue nonbreaking tests/preparation; do not create an auto-confirm fallback.
