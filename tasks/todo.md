# Phase 4 Exam Timing Telemetry — Task List

## Workflow state rule

Each task has two states. Antigravity may check **Implementation ready** only after RED evidence, focused tests and required gates. Only Claude may check **Review accepted** after independent diff review and verification. Antigravity stops after implementation-ready and cannot advance without prior Claude acceptance. Checklist files do not count toward the five-file ceiling.

## Current execution note � 2026-10-27

The resumed Tasks 5�11 fixes and the new review repairs are recorded in
[the current review](review-2026-10-27.md) and
[the ordered improvement plan](improvement-plan.md). Historical notes below
remain historical: the strict dangling-reference failure and routing/expiry
findings were subsequently repaired and locally tested.

Fresh final fast and strict gates both passed 49/49 with zero compiler warnings;
the browser suite passed 32 cases. No acceptance or checkpoint box is changed.
Fresh required review,
Checkpoint B peer/runtime evidence and the collector P2 repairs remain pending.
The user explicitly authorized commit and push of reviewed work to the feature
branch on October 27, 2026. Tasks 12�17 retain their existing dependencies.

## Autonomous continuation

The user authorized the one-minute resume heartbeat, planned scoped subagents,
and conditional main promotion after completion, working behavior, applicable
reviews/runtime evidence and exact-revision CI. See
[the resumable workflow](autonomous-workflow.md) and
[the derived product task index](product-execution-plan.md).
The private original product roadmap must never be published.

[CAPTURE-CLOCK-1](task-plans/capture-clock-1.md) is locally implemented and
independently re-reviewed: browser 43 cases, native focused coverage, fast and
strict 49/49 with zero compiler warnings. Its scope includes generation clock
mapping, explicit raw collector IDs, native initial uncertainty, bounded startup
retry under the unchanged quota, and direct closure discontinuity checks.
Required historical acceptance/checkpoint boxes are unchanged. Browser
aggregation is now locally implemented and independently reviewed: 63 browser
cases, native focused coverage, and Root fast/strict 49/49 with zero warnings.
Question-ID compatibility is now locally repaired and independently reviewed,
with shared/native3/3, browser63 and Root fast/strict49/49 checks. Server
durability/ACK, required acceptance/peer evidence and runtime qualification still
keep the integration incomplete for main.

STORAGE-RECOVERY-1A is locally implemented and independently reviewed: owned
numeric temporary cleanup includes blocked outbox copies, deletion failures
retain obligations, and marker/authorization refusal persists. Root gates
passed49/49 with zero warnings; exact published revision CI is checked separately.
Remaining storage slices and existing acceptance/server/runtime dependencies
remain pending; no historical checkbox is changed by this execution note.

## Task 0: Isolate the work

**Description:** Keep Phase 4 out of PR #6. Human decision: merge PR #6 first, then branch from updated `main`. Merge/branch actions still require explicit authorization.

**Acceptance criteria:**
- [x] PR #6 is merged by explicit human action.
- [x] New Phase 4 branch starts from updated `main`.
- [x] `artifacts/`, `.claude/`, and `HANDOFF.md` remain unstaged.

**Verification:**
- [x] `git status -sb` and `git log --oneline -5` prove base/branch.

**State:**
- [x] Implementation ready — Antigravity
- [x] Review accepted — Claude

**Dependencies:** None

**Files:** None

## Task 1: Define telemetry codecs

**Description:** Add pure typed, versioned sample/batch/ACK/enable codecs and canonical hash.

**Acceptance criteria:**
- [x] Reject unknown versions/kinds/reserved bits, zero identities, invalid UTF-8/IDs, numeric overflow, oversized values and trailing bytes.
- [x] Batch identity is producer + nonzero sequence + exact hash; no answer dependency.
- [x] ACK statuses exactly match `tasks/plan.md`.

**Verification:**
- [x] Focused round-trip/rejection/duplicate/conflict tests pass.
- [x] Existing answer codec bytes/tests remain unchanged.

**State:**
- [x] Implementation ready — Antigravity
- [x] Review accepted — Claude

**Dependencies:** Task 0 review accepted

**Files (5):**
- `common/include/nstu/exam_telemetry.hpp`
- `common/src/exam_telemetry.cpp`
- `common/CMakeLists.txt`
- `tests/exam_telemetry_tests.cpp`
- `tests/CMakeLists.txt`

## Task 2: Add protocol and IPC identifiers

**Description:** Add TCP IDs 55–57 and IPC IDs 36–37 without changing existing IDs/version.

**Acceptance criteria:**
- [x] Directions documented; existing numeric values unchanged.
- [x] IPC validator accepts new types and rejects above-bound values.
- [x] Unknown-command old-peer behavior is characterized.

**Verification:**
- [x] Protocol/IPC regression tests pass, including previous upper-bound cases.

**State:**
- [x] Implementation ready — Antigravity
- [x] Review accepted — Claude

**Dependencies:** Task 1 review accepted

**Files (5):**
- `common/include/nstu/protocol.hpp`
- `client/include/nstu/agent_protocol.hpp`
- `client/src/agent_protocol.cpp`
- `tests/protocol_tests.cpp`
- `tests/client_ipc_tests.cpp`

## Task 3: Implement browser aggregation

**Description:** Add disabled-by-default paged-question timing aggregation with one activation boundary.

**Acceptance criteria:**
- [x] Buttons/arrows share `activateQuestion`; rerenders are not views.
- [x] Dwell excludes hidden/blurred/unknown intervals; late enable/reload silently baseline restored state.
- [x] IME, paste, clear/reselect and identical input follow normalized semantics.
- [x] Samples contain no answer/note/key/clipboard/URL/title/free text.

**Verification:**
- [x] Node tests cover disabled default, late enable, navigation, focus, visibility, IME, reload, clock jump and submission.
- [x] `node --check exam/web/app.js` passes.

**State:**
- [x] Implementation ready — Antigravity
- [x] Review accepted — Claude

**Dependencies:** Tasks 1–2 review accepted

**Files (2):**
- `exam/web/app.js`
- `tests/exam_web_recovery_tests.js`

## Task 4: Validate native samples and blocked shortcuts

**Description:** Bind browser samples to native context, generate collector ID, define host-side matching-enable application, and collect native shortcut summaries outside the hook.

**Acceptance criteria:**
- [x] Browser identity ignored; native context supplies identity/collector.
- [x] Host collection remains off without a matching enable; duplicate matching enable is idempotent and stale/wrong-context enable is ignored.
- [x] Strict telemetry JSON field/size/depth/count limits.
- [x] Keyboard hook does no allocation, lock, JSON, logging, disk or IPC.
- [x] Wrong source/context, malformed values and bursts reject/coalesce.

**Verification:**
- [x] Native protocol tests cover hostile JSON, stale collector and bursts.

**State:**
- [x] Implementation ready — Antigravity
- [x] Review accepted — Claude

**Dependencies:** Task 3 review accepted

**Files (3):**
- `client/include/nstu/exam_host.hpp`
- `client/src/exam_host.cpp`
- `tests/exam_host_protocol_tests.cpp`

## Checkpoint A: Contract and capture

Only Claude checks checkpoint boxes after independent review and verification. Antigravity treats every unchecked checkpoint item as a hard dependency block.

- [x] Tasks 1–4 review accepted.
- [x] Fast gate + Node suite pass independently.
- [x] Answer contract/recovery unchanged.

## Task 5: Model suspension and revocation

**Description:** Pure state machine for disabled/enabled/suspended/revoked, reconnect and restart ordering.

**Acceptance criteria:**
- [x] Disconnect suspends and retains eligible batches; matching re-enable resumes.
- [x] Explicit stop permanently revokes and forbids replay.
- [x] Restart cannot upload before matching enable; transitions are idempotent.

**Verification:**
- [x] Table-driven transition tests pass.

**State:**
- [x] Implementation ready — Antigravity
- [ ] Review accepted — Claude

**Dependencies:** Checkpoint A

**Files (5):**
- `client/include/nstu/exam_telemetry_lifecycle.hpp`
- `client/src/exam_telemetry_lifecycle.cpp`
- `client/CMakeLists.txt`
- `tests/exam_telemetry_lifecycle_tests.cpp`
- `tests/CMakeLists.txt`

**Review note — 2026-10-26, Codex static review:** Parent and completed fresh Codex reviewer subagent found no actionable Task 5 defect under fix-5-2. The revocation archive holds 16 identities; full-archive refusal is intentional. [Review evidence](<C:/Users/xkhoa/Documents/Codex/2026-08-30/im/outputs/nstu-task5-review-2026-10-25.md>). Required Opus review/gates remain pending; all checkbox states are preserved.

**Execution note — 2026-10-26, Codex verification:** Focused lifecycle build/test passed (1/1); the fast gate passed 49/49. The strict gate failed at Task 8's reference-to-temporary accessor. A fresh tools-disabled Opus attempt returned HTTP 503 before inference and produced no verdict. [Commands, raw output and source bindings](<C:/Users/xkhoa/Documents/Codex/2026-08-30/im/outputs/nstu-task5-verification-2026-10-26.md>). Review acceptance remains pending; no product edits or checkbox changes.

## Task 6: Add coalesced enable bridge state

**Description:** Store/deliver the latest matching enable separately from the bounded answer-response FIFO and route service-to-agent enable into that state.

**Acceptance criteria:**
- [x] Enable cannot evict answer ACK/state response/begin.
- [x] Agent transport routes telemetry enable to the independent coalesced state and wakes the UI host; no enable is stranded in the generic exam-command queue.
- [x] Stale/wrong-context enable ignored; duplicate matching enable idempotent.
- [x] Host collection remains off without enable.

**Verification:**
- [x] Saturated FIFO tests prove independent enable availability and protected response preservation.

**State:**
- [x] Implementation ready — Antigravity
- [ ] Review accepted — Claude

**Dependencies:** Task 5 review accepted

**Files (5):**
- `client/include/nstu/exam_bridge.hpp`
- `client/src/exam_bridge.cpp`
- `client/src/exam_host.cpp`
- `client/agent/main.cpp`
- `tests/exam_bridge_tests.cpp`

**Review note — 2026-10-26, Codex static review:** Changes required: pre-enable shortcut counts leak into enabled evidence (T6-1); WebView2 code discards a nodiscard result (T6-2); queued startup clears the pending matching enable (T6-3). Add started-host context/duplicate and startup-order regressions. [Findings and source hashes](<C:/Users/xkhoa/Documents/Codex/2026-08-30/im/outputs/nstu-review-tasks5-11-2026-10-26.md#task-6-findings>). No runtime tests/gates or acceptance tick.

## Task 7: Implement durable telemetry outbox

**Description:** Independent protected, atomic, crash-safe outbox with exact ACK handling, terminal payload deletion and 30-day client expiry.

**Acceptance criteria:**
- [x] ≤4096 batches/8 MiB; no answer outbox reuse/eviction.
- [x] accepted/duplicate retire; conflict/rejected delete payload and increment bounded aggregate metadata; unavailable retries 2–60 s.
- [x] Unsent and aggregate metadata expire by 30 days.

**Verification:**
- [x] Lost/conflicting ACK, tamper, torn tail, restart, quota, terminal deletion, expiry and storage-unavailable tests pass.

**State:**
- [x] Implementation ready — Antigravity
- [ ] Review accepted — Claude

**Dependencies:** Tasks 1 and 5 review accepted

**Files (5):**
- `common/include/nstu/exam_telemetry_outbox.hpp`
- `common/src/exam_telemetry_outbox.cpp`
- `common/CMakeLists.txt`
- `tests/exam_telemetry_outbox_tests.cpp`
- `tests/CMakeLists.txt`

**Review note — 2026-10-26, Codex static review:** Current fix-7-2 manifest matches all five task files. Explicit expiry pruning exists, but production never calls it and send polling can return expired records (T7-1); enforce the 30-day maximum during idle, reopen and polling. [Finding and source hashes](<C:/Users/xkhoa/Documents/Codex/2026-08-30/im/outputs/nstu-review-tasks5-11-2026-10-26.md#task-7-finding>). No fresh execution/Opus acceptance; existing checkbox states are preserved.

## Task 8: Build a testable service coordinator

**Description:** Pure/callable coordinator binds samples, producer/sequence, lifecycle, batching, outbox, retry and loss accounting without service globals.

**Acceptance criteria:**
- [x] Agent/browser cannot choose producer or sequence.
- [x] No samples admitted before enable or after revoke.
- [x] Full outbox drops/coalesces telemetry only and records bounded loss.

**Verification:**
- [x] Coordinator tests cover enable, batching, ACK, suspend/resume, restart, revoke, stale context and full queue.

**State:**
- [x] Implementation ready — Antigravity
- [ ] Review accepted — Claude

**Dependencies:** Tasks 6–7 review accepted

**Files (5):**
- `client/include/nstu/exam_telemetry_service.hpp`
- `client/src/exam_telemetry_service.cpp`
- `client/CMakeLists.txt`
- `tests/exam_telemetry_service_tests.cpp`
- `tests/CMakeLists.txt`

**Review note — 2026-10-26, Codex static review:** Changes required: reopen self-deadlock, dangling aggregate-loss reference, permanent rejection of subsequent exams, split outbox/sequence persistence, orphan-state recovery, incomplete/unbounded drop accounting and discarded persistence failures (T8-1–T8-7). Parent verified the partial subagent findings; the failed subagent attempt is not a completed verdict. [Findings and missing regressions](<C:/Users/xkhoa/Documents/Codex/2026-08-30/im/outputs/nstu-review-tasks5-11-2026-10-26.md#task-8-findings>). fix-8-1 and required verification remain pending; no acceptance tick.

**Execution note — 2026-10-26, Codex verification:** The strict gate reproduced T8-2 at `exam_telemetry_service.cpp:551` with `-Werror=return-local-addr`; build exit 1, CTest skipped. The separate fast gate passed 49/49 and does not clear this failure. [Fresh execution evidence](<C:/Users/xkhoa/Documents/Codex/2026-08-30/im/outputs/nstu-task5-verification-2026-10-26.md#reproduced-strict-failure>). fix-8-1 remains required.

## Task 9: Add testable client routing adapter and thin service wiring

**Description:** Move production telemetry routing into a callable platform component; service executable only binds existing queues/callbacks.

**Acceptance criteria:**
- [x] Adapter handles decoded IPC samples/enable, TCP ACK, disconnect suspension, restart and explicit revocation through injected coordinator/queue callbacks.
- [x] Exact adapter code used by production is linked into focused tests; no duplicate test-only handler implementation.
- [x] Explicit stop revokes without delay; disconnect suspends; shutdown retains eligible active-exam outbox unless revoked.
- [x] Agent-side existing bridge/callback path delivers enable/samples without new global test seams in `agent/main.cpp`.

**Verification:**
- [x] Adapter tests exercise every production handler with fake coordinator/outbox/queue callbacks, including stale context and malformed payload.
- [x] Thin `service/main.cpp` bindings compile and full gate passes.

**State:**
- [x] Implementation ready — Antigravity
- [ ] Review accepted — Claude

**Dependencies:** Task 8 review accepted

**Files (5):**
- `client/include/nstu/exam_telemetry_router.hpp`
- `client/src/exam_telemetry_router.cpp`
- `client/CMakeLists.txt`
- `client/service/main.cpp`
- `tests/client_control_tests.cpp`

**Review note — 2026-10-26, Codex static review:** Production restart callback is a success stub with no startup call; shutdown bypasses the lifecycle transition; the low-priority scheduling test is defined but never invoked (T9-1–T9-3). Test the real-coordinator constructor and report failures. The earlier byte narrowing at line 354 is already fixed. [Findings](<C:/Users/xkhoa/Documents/Codex/2026-08-30/im/outputs/nstu-review-tasks5-11-2026-10-26.md#task-9-findings>). fix-9-1 review/gates remain pending; no Task 12 dispatch or acceptance tick.

## Task 10: Emit server enable

**Description:** Enable production collection only now that lifecycle/coordinator wiring exists.

**Acceptance criteria:**
- [x] Enable only for active matching server context after start/reconnect and before the fixed telemetry deadline.
- [x] Telemetry eligibility ends at the earlier of explicit stop/context removal or 25 hours from original exam-context creation; reconnect/re-enable never extends it.
- [x] Telemetry expiry does not alter answer authorization/recovery.
- [x] Old client ignores enable; new client + old server collects nothing.

**Verification:**
- [x] Control-plane tests cover start, reconnect, wrong client, stop, 25-hour expiry without deadline renewal, preserved answer recovery and old-peer matrix.

**State:**
- [x] Implementation ready — Antigravity
- [ ] Review accepted — Claude

**Dependencies:** Task 9 review accepted

**Files (2):**
- `server/src/control_plane.cpp`
- `tests/control_plane_exam_auth_tests.cpp`

**Review note — 2026-10-26, Codex static review:** Changes required: the wall-clock eligibility predicate can re-enable an expired context after rollback (T10-1). Complete current old/new peer execution evidence is missing (T10-2); command-99 framing alone does not establish it. [Findings and regression requirements](<C:/Users/xkhoa/Documents/Codex/2026-08-30/im/outputs/nstu-review-tasks5-11-2026-10-26.md#task-10-findings>). Forward-expiry answer recovery is represented in current tests, but no fresh run/gate or acceptance tick occurred.

## Checkpoint B: Lifecycle and compatibility

- [ ] Tasks 5–10 review accepted.
- [ ] Mixed old/new peer matrix passes.
- [ ] Disconnect differs from explicit stop.

**Review note — 2026-10-26, Codex static review:** All three checkpoint items remain pending. Tasks 6–10 have unresolved correctness/verification findings; Task 5 still needs required Opus/gate evidence. [Saved execution inventory](<C:/Users/xkhoa/Documents/Codex/2026-08-30/im/outputs/nstu-checkpoint-b-inventory-2026-10-25.md>) distinguishes historical passes from current-source evidence. [Current review and ordered follow-up](<C:/Users/xkhoa/Documents/Codex/2026-08-30/im/outputs/nstu-review-tasks5-11-2026-10-26.md#checkpoint-b-and-remaining-review-work>) require the identified peer matrix and production suspension/stop/restart verification. Acceptance dependencies remain unchanged.

**Execution note — 2026-10-26, Codex verification:** Fresh fast execution passed 49/49, but strict compilation failed and the required Task 5 Opus review could not run (HTTP 503). This supplies execution evidence without resolving the peer-matrix, production-path or acceptance dependencies. [Evidence](<C:/Users/xkhoa/Documents/Codex/2026-08-30/im/outputs/nstu-task5-verification-2026-10-26.md>). All checkpoint boxes remain unchanged.

## Task 11: Add absolute whole-frame send deadline

**Description:** Add `steady_clock` absolute send deadline so partial progress cannot extend timeout indefinitely.

**Acceptance criteria:**
- [x] Deadline applies to entire frame across repeated partial writes.
- [x] Deadline expiry fails/disconnects consistently; ordinary existing sends remain compatible.
- [x] Slow-progress test proves elapsed bound, not only zero-progress timeout.

**Verification:**
- [x] Network/control-channel tests cover partial writes, expiry and successful completion.

**State:**
- [x] Implementation ready — Antigravity
- [ ] Review accepted — Claude

**Dependencies:** Checkpoint B

**Files (5):**
- `common/include/nstu/network.hpp`
- `common/src/network.cpp`
- `common/include/nstu/control_channel.hpp`
- `common/src/control_channel.cpp`
- `tests/control_channel_tests.cpp`

**Review note — 2026-10-26, Codex static review:** One steady deadline spans partial sends; no separate algorithm defect was established. The slow-progress test does not deterministically force backpressure (T11-1); add synchronized partial-progress proof and successful deadline-send followed by ordinary send coverage. [Finding](<C:/Users/xkhoa/Documents/Codex/2026-08-30/im/outputs/nstu-review-tasks5-11-2026-10-26.md#task-11-verification-finding>). Preserve the historical five-hash ACCEPT and its unticked Checkpoint B dependency; no new acceptance/gate claim.

## Task 12: Enforce low-priority telemetry scheduling

**Description:** Offer ≤1 telemetry batch/s and ≤1 per loop, with a 500 ms absolute deadline and no back-to-back telemetry sends.

**Acceptance criteria:**
- [ ] Higher-priority outbound work always wins.
- [ ] After telemetry send attempt, loop proceeds to receive/wait.
- [ ] Timeout disconnects and preserves durable retry eligibility for matching reconnect.

**Verification:**
- [ ] Flood + slow-progress socket tests prove answer/status/stop progress and no four-batch burst.

**State:**
- [ ] Implementation ready — Antigravity
- [ ] Review accepted — Claude

**Dependencies:** Task 11 review accepted

**Files (3):**
- `client/src/client_control.cpp`
- `client/service/main.cpp`
- `tests/client_control_tests.cpp`

## Task 13: Implement journal, bounded dedup and replay floor

**Description:** Dedicated detail/dedup store with rotation, hard metadata quotas, 30-day maximum and non-resurrecting replay floor.

**Acceptance criteria:**
- [ ] Exact duplicates/conflicts survive restart; gaps accepted/visible.
- [ ] Detail rotates 8×8 MiB; dedup ≤262144 hashes, ≤4096/producer, ≤4096 contexts, ≤32 MiB, ≤16 lifetime producer generations/context.
- [ ] First valid batch for an unseen producer registers only at sequence 1 under active telemetry authorization; reconnect/restart reuses admitted producer state, retired IDs never register again, and the generation cap survives restart.
- [ ] Eviction/expiry advances the active-context replay floor; expired sequences cannot reinsert with renewed retention.
- [ ] Replay floor contains only producer ID + sequence, exists only until telemetry stop/context removal/fixed 25-hour deadline, and is deleted when telemetry authorization ends; no behavioral detail survives expiry.
- [ ] Torn tail/disk full recover safely; client timestamps never extend retention.

**Verification:**
- [ ] Duplicate/conflict/gap/torn-tail/quota/eviction/floor/30-day tests pass.
- [ ] Tests cover first-batch registration, non-1 rejection, restart reuse, retired-ID rejection and persistent 16-generation cap.
- [ ] Replay after complete hash/detail expiry while telemetry authorization remains active is rejected by floor; replay after stop or fixed deadline is rejected for inactive telemetry authorization.

**State:**
- [ ] Implementation ready — Antigravity
- [ ] Review accepted — Claude

**Dependencies:** Task 1 review accepted

**Files (5):**
- `common/include/nstu/exam_telemetry_journal.hpp`
- `common/src/exam_telemetry_journal.cpp`
- `common/CMakeLists.txt`
- `tests/exam_telemetry_journal_tests.cpp`
- `tests/CMakeLists.txt`

## Task 14: Integrate async server persistence and ACK

**Description:** Validate/enqueue in authenticated callback, persist on bounded worker outside control locks, ACK outcome, configure dedicated path/ACL.

**Acceptance criteria:**
- [ ] Active matching tuple required; malformed/stale/cross-exam rejected consistently.
- [ ] No disk I/O under connection/control locks; full/stalled queue returns unavailable without blocking control traffic.
- [ ] ACK durability/terminal semantics match plan.
- [ ] Separate restricted `%ProgramData%\NSTU\exams\telemetry-journal` path configured.

**Verification:**
- [ ] Authorization, rate-limit, worker-full and stalled-disk tests pass.

**State:**
- [ ] Implementation ready — Antigravity
- [ ] Review accepted — Claude

**Dependencies:** Tasks 12–13 review accepted

**Files (4):**
- `server/include/nstu/control_plane.hpp`
- `server/src/control_plane.cpp`
- `server/app/main.cpp`
- `tests/control_plane_exam_auth_tests.cpp`

## Checkpoint C: Durable channel

- [ ] Tasks 11–14 review accepted.
- [ ] Strict gate passes unpiped.
- [ ] Slow-progress socket and stalled disk tests pass.

## Task 15: Add cross-channel integration tests

**Description:** Prove answer equivalence, mixed-version, reconnect/restart/stop/flood and leakage behavior.

**Acceptance criteria:**
- [ ] Telemetry flood cannot change answer bytes/hash/state or block finalize/recovery.
- [ ] Stop forbids replay; disconnect + matching enable permits eligible replay.
- [ ] Canary answer/notes/clipboard text absent from telemetry/audit files.

**Verification:**
- [ ] Dedicated integration target passes all cases.

**State:**
- [ ] Implementation ready — Antigravity
- [ ] Review accepted — Claude

**Dependencies:** Checkpoint C

**Files (2):**
- `tests/exam_telemetry_integration_tests.cpp`
- `tests/CMakeLists.txt`

## Task 16: Add local browser gate and truthful docs

**Description:** Run browser suite locally and document evidence/privacy/retention limits.

**Acceptance criteria:**
- [ ] Local gate runs Node syntax + recovery/telemetry suite and propagates either failure.
- [ ] Docs say evidence requiring review, missing=unknown, 30-day maximum, possible earlier quota loss, no automatic penalty/raw input.
- [ ] Roadmap marks only completed pieces.

**Verification:**
- [ ] Fast and strict gates pass from clean state.
- [ ] C++ and security reviews have no unresolved HIGH/CRITICAL findings.

**State:**
- [ ] Implementation ready — Antigravity
- [ ] Review accepted — Claude

**Dependencies:** Task 15 review accepted

**Files (4):**
- `scripts/verify-gate.sh`
- `exam/README.md`
- `docs/EXAM_ASSESSMENT.md`
- `docs/ROADMAP.md`

## Checkpoint D: Local complete

- [ ] Tasks 15–16 review accepted.
- [ ] All local gates/reviews pass.

## Task 17: Qualify real WebView2/VM behavior

**Description:** Validate focus/visibility, accessibility/IME/dialog behavior, shortcuts, reconnect/restart/stop and cleanup on isolated VM without student content.

**Acceptance criteria:**
- [ ] Evidence matches observed UI behavior without false claims.
- [ ] Kill/restart/network interruption obey suspension/revocation.
- [ ] No secrets/answers in evidence; cleanup/retention verified.

**Verification:**
- [ ] Redacted VM checklist reviewed by Claude and human.
- [ ] Human explicitly authorizes push; MSVC `/W4 /WX` Release CI passes.

**State:**
- [ ] Implementation ready — Antigravity
- [ ] Review accepted — Claude

**Dependencies:** Checkpoint D

**Files:** Evidence only under never-staged `artifacts/`.

## Checkpoint E: Complete

- [ ] Tasks 1–17 review accepted.
- [ ] Real VM evidence reviewed without secrets/student content.
- [ ] No commit/push/PR action occurred without explicit human approval.
