# Implementation Plan: Phase 4 Exam Timing Telemetry

## Overview

Add a separate, bounded, durable exam-behavior telemetry channel for timing and review signals. It must not modify answer event bytes, hash chain, sequence, outbox, journal, or recovery. Telemetry is evidence for human review—never proof of cheating, an automatic flag, score, or penalty.

Antigravity implements one small slice at a time. Claude independently reviews and verifies every slice. Phase 4 must not expand PR #6. **Decision:** merge PR #6 first, then branch from updated `main`; merging/branching still requires an explicit action request.

## User-facing policy

- Automatic for low-tech operators: no settings, version prompts, or fleet-upgrade ritual.
- Upgraded server sends authenticated enable for each active exam. Client collects only after matching enable.
- Old clients ignore enable; new clients with old servers do not collect.
- Signals are **observations requiring review**. Missing/lost data is **unknown**.
- Retention is a **30-day maximum**. Quota pressure may remove detail earlier; aggregate loss remains visible.
- Never collect answer text, notes, raw keys, clipboard, URLs, window titles, typed text, device fingerprint, or arbitrary free-form JSON.

## Architecture

### Ownership

1. Browser aggregates typed observations; no durable batch sequence or ACK handling.
2. `ExamHost` validates source/schema, binds immutable native exam identity and collector generation, and emits typed samples.
3. A testable service coordinator owns lifecycle, producer ID, batching, independent sequence, durable outbox, retry and ACK retirement.
4. `client/service/main.cpp` is thin wiring around that coordinator.
5. Server authenticated callback validates and enqueues bounded work. A separate worker persists to a dedicated journal and returns ACK results without disk I/O under connection/control locks.
6. Browser never receives telemetry ACKs; telemetry cannot occupy the bounded answer-response bridge.

### Compatibility

Authenticated command IDs after `exam_begin = 54`:

- `exam_telemetry_enable = 55` — server → client
- `exam_telemetry_batch = 56` — client → server
- `exam_telemetry_ack = 57` — server → client

Agent IPC IDs after `exam_alive = 35`:

- `exam_telemetry_enable = 36` — service → agent
- `exam_telemetry_samples = 37` — agent → service

Enable carries the complete server-issued exam tuple. Server emits only after the client lifecycle/coordinator path exists, after successful exam start, and after authenticated reconnect while the same context remains active. Client remains disabled without enable. No general protocol-v2 framework.

### Typed contract

Implement under `nstu::exam`, not public diagnostic `nstu::telemetry`. Use explicit versioned little-endian codecs; never serialize struct layout.

Sample kinds: `collector_started`, `question_first_view`, `question_dwell`, `question_first_answer`, `answer_change_summary`, `focus_changed`, `visibility_changed`, `blocked_shortcut_summary`, `exam_submitted`, `loss_summary`, `clock_discontinuity`.

Fields are fixed: kind, source (`browser`/`native`), collector ID, optional existing-valid question ID/revision, monotonic elapsed/duration milliseconds, untrusted client wall-clock milliseconds, and bounded numeric value/count/reason enum. No arbitrary strings except question ID. Shortcut data is coarse category + count, never key codes.

Batch: complete exam tuple, nonzero random 128-bit service producer ID, nonzero sequence starting at 1, first local enqueue time, 1–32 samples, canonical hash. The protected service state reuses its active producer ID and next sequence across disconnect/restart; it creates a new producer generation only when no valid active-context producer state can be recovered after matching enable. No previous-hash chain or answer dependency.

ACK: session ID, producer ID, exact sequence/hash, server receipt time and status:

- `accepted`: retire after durable batch + dedup commit;
- `duplicate`: retire only when exact durable identity/hash exists;
- `rejected` / `conflict`: terminal; delete payload immediately, retain only bounded aggregate loss/security metadata;
- `unavailable`: nonterminal; retain and retry with exponential backoff 2–60 s.

Gaps are accepted and recorded; they do not stall later evidence.

### Browser summary and loss representation

For answer_change_summary, count is the cumulative observed transition count and
numeric_value is the pending delta represented by that summary. The otherwise
unused duration_milliseconds on this kind is a two-bit flag: bit0 marks cumulative
count overflow/lower-bound, bit1 marks delta overflow/lower-bound. Exact UINT32_MAX
without overflow has no flag. For loss_summary only, duration_milliseconds bit0
marks saturated/lower-bound loss; numeric_value remains zero. Other kinds retain
their existing duration meaning. Field layout/codecs and answer formats do not
change. Browser posting remains an attempt, not proof of native/durable acceptance.

### Collector/recovery semantics

- New native collector ID for each successful top-level exam document generation/reload.
- Monotonic origin starts with collector generation.
- Late enable silently baselines current question/recovered answers, emits `collector_started(late_enable)`, and does not manufacture pre-enable first-view/first-answer/change events.
- Reload baselines restored answers; restored nonempty answers are not new events.
- Suspend/reload/wall-clock jump/monotonic discontinuity closes dwell as unknown and emits discontinuity; no inferred duration crosses unknown time.

### Browser semantics

One `activateQuestion(index, reason)` boundary handles buttons and arrows; rerenders are not views. Dwell counts visible+focused segments only. Changes count normalized value transitions; IME composition and identical input do not inflate counts. Native blocked shortcuts are coalesced outside the low-level hook. Flush on navigation, visibility/focus transition, submission and every 30 s.

### Lifecycle

Current service uses `queue_exam_stop()` for both TCP disconnect and authenticated stop. Telemetry must separate:

- **suspension:** disconnect stops collection, retains outbox, clears in-flight markers; replay only after matching enable on reconnect to a still-active server context;
- **revocation:** explicit stop stops collection, revokes context and deletes unsent payloads without delaying stop; retains only bounded aggregate loss metadata;
- **service shutdown:** retains active-exam outbox unless already revoked.

No post-stop authorization tombstone. Missing tail is unknown/loss, not misconduct.

### Storage, retention and replay

- Client outbox ≤4096 batches / 8 MiB. Unsent data expires 30 days after local enqueue. Terminal payloads are deleted immediately; aggregate terminal counters are bounded per context and expire by 30 days.
- Server detail store rotates at 8 × 8 MiB; detail may evict earlier than 30 days under quota.
- Dedup metadata has independent hard bounds: ≤262,144 recent hashes, ≤4096 per producer, ≤4096 exam contexts, ≤32 MiB total, all expiring no later than 30 days from first server receipt.
- Each active telemetry authorization stores a monotonic per-producer **replay floor**. This is minimal authorization control state (producer ID + highest expired/evicted sequence), not retained behavioral evidence. It survives hash/detail expiry only until the earlier of explicit exam stop/context removal or 25 hours after the original server exam-context creation time. Reconnect/re-enable never resets this fixed deadline. Expiry disables telemetry enable/ingest for that context and deletes its telemetry floors while leaving answer authorization/recovery behavior unchanged; every later telemetry batch is rejected because telemetry authorization is inactive.
- Producer admission is authenticated first-batch registration: while telemetry authorization is active, the first valid batch for an unseen producer may register only with sequence 1 and only when its producer ID is not in the bounded retired-producer set. Exact reconnect/restart reuse of an admitted producer continues its sequence. Creating a replacement generation consumes one of ≤16 lifetime producer slots for that exam context; retired/evicted producer IDs never become admissible again. Further generations are rejected/unavailable and recorded as aggregate loss.
- When exact hash metadata is evicted/expired during active telemetry authorization, its floor advances past it. A later sequence at or below the floor is rejected as expired replay and cannot renew retention. Unseen gaps below a floor become explicit loss/unknown. Client timestamps never extend server retention.

### Priority and measurable latency

- Sample ≤4 KiB; batch ≤32 samples / 32 KiB; ingest ≤8 batches/s and 256 samples/s per authenticated client.
- Telemetry provider offers ≤1 batch/s and ≤1 telemetry send per control loop, only after higher-priority work.
- Add an absolute whole-frame send deadline using `steady_clock`; repeated partial writes cannot extend it. Telemetry deadline ≤500 ms. Timeout disconnects the session and leaves the durable batch eligible for retry after matching reconnect enable.
- Stop/answers/state/ready/begin/heartbeat/pairing outrank telemetry.
- Server callback performs bounded validation/enqueue only. Full/stalled persistence queue returns `unavailable`; it cannot block control locks.
- Tests cover slow-progress socket (not only zero-progress timeout) and stalled disk.

## Dependency graph

```text
codec → IDs → browser/native capture
                 │
         lifecycle → bridge state → outbox → service coordinator
                                            │
                             thin client wiring + server enable
                                            │
             send deadline → scheduling     journal → server worker
                         └──────────────┬──────────────┘
                                  integration/VM qualification
```

## Tasks

Detailed file bounds, acceptance criteria, verification and two-party states are in `tasks/todo.md`. The coding agent must reconstruct state from `tasks/coding-agent-context.md` plus repository/Git evidence at every invocation; conversation memory is never a prerequisite.

1. Codecs
2. IDs
3. Browser collector
4. Native collector
5. Lifecycle state machine
6. Coalesced enable bridge
7. Durable outbox
8. Testable service coordinator
9. Testable client routing adapter + thin executable wiring
10. Server enable emission
11. Absolute send deadline
12. Low-priority scheduling
13. Journal, bounded dedup and retention
14. Async server persistence/ACK/path
15. Cross-channel integration
16. Local browser gate and docs
17. Real WebView2/VM qualification

### Checkpoint A — Tasks 1–4

Contract/capture frozen; browser disabled by default. Node and focused C++ tests pass. No answer behavior changed.

### Checkpoint B — Tasks 5–10

Lifecycle, coalesced enable, outbox, coordinator and thin wiring exist before production enable. Mixed old/new peers pass; disconnect differs from explicit stop.

### Checkpoint C — Tasks 11–14

Absolute send deadline, priority scheduling, bounded journal/dedup and async server worker pass slow-socket/disk and retention tests.

### Checkpoint D — Tasks 15–16

Cross-channel answer equivalence/leakage tests and all local gates/reviews pass.

### Checkpoint E — Task 17

Real WebView2/VM evidence passes. Human separately authorizes any push; MSVC `/W4 /WX` Release CI is final automated gate.

## Autonomous continuation and scoped task plans — 2026-10-27

The user authorized a one-minute resume heartbeat, planning and task splitting
before implementation, prioritized subagents, and main promotion only after
the relevant work is complete, functioning, reviewed and tested with passing CI.
The durable protocol is [autonomous-workflow.md](autonomous-workflow.md); the
derived product capability/task index is
[product-execution-plan.md](product-execution-plan.md). Preserve this incomplete
Phase 4 plan and the authoritative checklist while extending its follow-up work.

CAPTURE-CLOCK-1, bounded browser aggregation, question-ID compatibility and the
first two storage recovery units are published with exact-revision Windows CI.
The active follow-up is
[STORAGE-RECOVERY-1C-1](task-plans/storage-recovery-1c-1.md): mixed cleanup stages
and path isolation. Its local tests and independent evidence must be reconciled
before publication. Safe ACK retirement and purge/clear remain separate units in
[storage-recovery-1c.md](task-plans/storage-recovery-1c.md); pristine interrupted
initialization follows. No telemetry codec, answer channel or service
producer/sequence change is allowed.

`PRODUCT_ROADMAP_NSTU.md` is a private local source and must never be staged,
committed or uploaded. Derived requirements may be recorded in scoped plans.
Green CI alone does not complete the unfinished server durability/ACK path or
supply required acceptance/runtime evidence for main integration.

## Out of scope

Automatic verdicts/flags/penalties/grading; teacher monitor UI; answer keys/content; generic protocol v2; post-stop upload; app suppression; IPv6/default-deny WFP; replacing answer journal, audit sink or public diagnostics telemetry.
