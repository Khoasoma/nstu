# NSTU product execution plan — October 27, 2026

Derived from the revised roadmap and the private original product requirements.
This is a capability/task index, not a second acceptance checklist. Existing
Phase 4 IDs and checkpoints in `todo.md` remain authoritative. Each implementation
unit receives its own scoped plan before coding, following
[autonomous-workflow.md](autonomous-workflow.md).

## Current evidence

`6709d33` is published on the telemetry feature branch. Its local fast and strict
gates passed 49/49, browser checks passed 32 cases, and all three required Windows
build/test/installer CI jobs passed. These are revision-bound results, not proof
of feature completion. Bounded browser summaries,
required acceptance/peer evidence, and Tasks 12–17 remain unfinished.

The current code already has answer recovery, snapshot/Focus/broadcast/chat,
discovery/preferred-room/SAS pairing, installer role selection, diagnostics and
rollback foundations. Reuse them. Teacher startup is an interactive Run-key
application, not a Teacher Windows service. The student service is separate.

## Capability order

| Capability | Depends on | Completion evidence |
|---|---|---|
| Exam telemetry integration | Current capture repairs and existing Tasks 5–17 | Exact-hash acceptance, peer/lifecycle matrix, bounded scheduling, durable journal/ACK, channel/privacy equivalence and real runtime qualification. |
| Official exam security | Completed telemetry/runtime baseline and approved threat contract | IPv4/IPv6/alternate-port enforcement, app-lifecycle decision, instructor/package/session authorization and signed image/runtime evidence. |
| Installation and onboarding | Exam/security production gates and scoped ownership clearance | Release-only fresh/upgrade/repair setup, compatibility remedies, discovered rooms/manual fallback and mandatory SAS. |
| Teacher usability and daily tools | Installation/onboarding plus usability/authorization contracts | Understandable role surfaces, list/multi-select, accurate bulk progress/retry and observed lesson workflows. |
| Fleet and policy | Usable classroom workflows and privacy/update trust contracts | Lightweight stale-aware health, signed resumable updates/canary/rollback, sanitized support export and policy recovery. |

## Immediate task index

| ID | Scope and dependency | Acceptance groups |
|---|---|---|
| REC-01 | Reconcile recorded local provenance and current hashes; evidence only. | Preserve unknown README/banner/About work; identify current task/owner without inventing provenance; retain failed-run history. |
| EX-R1 | Implemented native/browser clock repair; five capture source/test files. Scoped plan: [capture-clock-1.md](task-plans/capture-clock-1.md). | Generation-bound origin mapping; slow-load/late-enable/reload/unknown-time behavior; unchanged answer channel and truthful uncertainty. |
| EX-R2 | Implemented bounded browser summaries after EX-R1 review. `app.js` and browser tests. | Coalesced normalized changes; bounded memory/flush and loss with preserved totals/navigation/dwell; privacy and answer equivalence. |
| EX-R3 | Hostile storage recovery tests; add production changes only for a reproduced defect. | Malformed protected barriers refuse unsafe recovery; blocked cleanup/interrupted initialization retain refusal; mutation retries preserve quotas/revocation. |
| EX-A5…A11 | Close canonical acceptance in its existing dependency order. | Current source review and applicable focused/full gates; required reviewer evidence; no historical acceptance inferred from changed hashes. |
| EX-B | Existing Checkpoint B. | Actual old/new peers; disconnect suspension versus explicit revocation; shutdown/restart and enable behavior. |
| EX-12 | Existing Task 12, after Task 11 acceptance. | Higher-priority progress; one telemetry send per second/loop then receive/wait; valid-frame synchronized partial progress, 500 ms deadline and durable matching reconnect. |
| EX-13a | Task 13 crash-safe journal slice, after scheduler in product order. | Separate durable detail/dedup; duplicate/conflict/torn-tail/restart behavior; honest disk-failure results. |
| EX-13b | Task 13 quota/admission/floor slice, after EX-13a. | Canonical quotas/generation admission; non-resurrecting eviction/expiry floor; authorization termination removes floors and refuses replay. |
| EX-14 | Existing Task 14 after Tasks 12–13 acceptance. | Active authenticated tuple; bounded worker outside control locks; durable ACK and restricted dedicated path. |
| EX-15/16 | Existing integration and browser-gate/docs tasks after Checkpoint C. | Answer equivalence/privacy/replay; Node failure propagation; truthful retention and evidence documentation. |
| EX-17 | Existing real WebView2/VM task after Checkpoint D. | IME/accessibility/focus/reload evidence; interruption/stop/cleanup; redacted human-reviewed runtime results. |

The original 1 MiB slow-socket fixture is invalid for the 64 KiB command limit.
Task 12 must prove actual socket partial progress using a valid bounded frame,
not count an encoder rejection as a deadline test.

## Productization tasks after exam/security gates

Every row below is a future admission. Inspect code, ownership and direct callers
again to choose at most five actual files; split contract, routing and UI when
they span independent subsystems.

| ID | Sources and dependency | Next independently testable slice |
|---|---|---|
| SEC-01…05 | Revised E3 and cross-cutting security/privacy. | Threat/traffic contract; IPv6/default-deny enforcement; app-lifecycle requirement; official authorization; signed/runtime/image qualification. Separate implementation and human VM/certificate operations. |
| SET-01 | Revised P1.1; original P0-01. | Inventory release provisioning, then fix one evidenced ACL/firewall/bootstrap gap with pairing/data-preserving repair and rollback tests. |
| SET-02 | P1.1 / P0-02; after SET-01. | Add one missing compatibility check to the existing diagnostics/preflight catalog, classify blocker versus warning, and provide an actionable remedy. |
| SET-03 | P1.2 / P0-03; after SET-02. | Extend discovery/preferred-room onboarding, duplicate-room identity and manual/VLAN fallback; SAS remains mandatory. |
| SET-05 | P1.3 / P0-05…07 and UX-01…03; after reviewed installer/UI. | Release-matching Quick Start, human-readable errors/remedies and focused link/version drift checks. README/banner ownership must be resolved first. |
| SET-04 | P1.4 / P0-04; after discovered-room onboarding. | First-run wizard for unconfigured installs with skip/reopen/partial/restart behavior and no silent trust grant. |
| UX-01 | Revised P2.1 / original P1-01. | Teacher/Technician/Advanced surfaces that simplify normal use without deleting diagnostics or weakening server authorization. |
| UX-02/03 | P2.2 / P1-02,03,07; after UX-01. | List/search/multi-select and accurate target identity, then bulk count/room/action confirmation, progress/failure/retry. Benchmark 10/30/50 clients. |
| UX-04 | P1-04/05 and privacy requirements. | Device/student/lesson identity and retention contract before adding collection or session transport/UI. |
| TOOL-01/02 | P2.3 / P1-06,08; after bulk/action contracts. | Scoped help request/cancel/ack/reconnect, then reuse broadcast/chat with truthful delivery and retry states. |
| TOOL-03/04/05 | P1-09…11. | Validated website schemes; typed application launch definitions/presets; authorized power actions with force-close off by default and honest offline/WOL behavior. No raw shell. |
| TOOL-06/07 | P1-12; after authorization/privacy contracts. | Teacher-to-client file codec and safe bounded staging, then transfer/progress/retry UI. Reject traversal, verify hashes and never auto-execute received files. |
| TOOL-08/09 | P1-13. | Reuse Focus broadcast with resource/input-lock cleanup, then separate resizable Follow-Along. Keep the class wall snapshot-first. |
| FLEET-01/02 | P3.1 / P2-01,02. | Stale-aware existing health/details first; add only justified low-frequency/change-driven version/service metadata under a privacy contract. |
| FLEET-03/04 | P3.2 / P2-03…05. | Signed trust/rotation/hash/anti-downgrade and resumable staging/recovery, then canary health gates/pause/rollback and sanitized history. |
| FLEET-05 | P2-06/07. | Portable secret-free room profiles separately from previewed, consented, canary-tested support sanitization/export. |
| POL-01 | P3.3 / P2-08/09. | Authorized lesson-scoped web/app policy with expiry, stop/reconnect/crash/reboot and emergency recovery; validate alternate routes without MITM. |

Checkpoint after two or three admitted slices: fresh independent review, matching
focused/full/browser results, exact-revision MSVC CI and observed user workflow.
File distribution and both demonstration modes remain planned capabilities.
Question banks, grading, SIS/LMS and broad rewrites remain deferred.

## Private source and publication

`PRODUCT_ROADMAP_NSTU.md` stays local and must not enter any candidate Git tree.
Only derived task requirements are published. Main promotion follows the scoped
completion gate in the autonomous workflow; green CI alone does not clear known
functional defects, unfinished production paths or missing qualification.
