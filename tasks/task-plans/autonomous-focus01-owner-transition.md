# FOCUS01 owner transition implementation plan

> Agentic execution: use the NSTU delivery loop and TDD, with coordinator-assigned independent PLAN_ACCEPT before product edits and CODE_ACCEPT before commit/integration. No additional agents are spawned by this owner while the team slots are occupied.

**Goal:** A same-client Remote to Focus ownership transition emits a real new start request and supplies its actual result to Focus presentation, while rejecting stale request completions and preserving bounded watchdog recovery.

**Architecture:** Extract the dashboard's stream-request decision into a small production seam in the existing Focus observer header. The dashboard executes the returned stop/start/rearm commands through the unchanged synchronous control-plane API and supplies only the matching real result to the observer. The decision state tracks owner, target, and request serial; presentation remains based on the existing report, generation, texture, and freshness conditions.

**Tech stack:** C++20, existing StreamRearmWatchdog, unchanged Dear ImGui/Win32 control-plane caller, existing assert-based CTest target.

**Spec:** `C:/Users/xkhoa/.codex/attachments/6ed20f89-c5da-4d93-8f14-eb018532f5bc/Pasted text.txt`, FOCUS01 delegation in this conversation. Historical lead only: `D:/Codex/design-drafts/nstu-project-health-20261107/C-streaming/REPORT.md` C3.

## Source and ownership contract

- Source checkout (read-only): `D:/Codex/im/work/nstu-ui-reviewed-20261107`, observed clean at `edb79e872339e372de9ee662ed71e9a0cacef154`.
- Implementation worktree: `C:/Users/xkhoa/.codex/worktrees/autonomous-focus01/nstu-ui-reviewed-20261107`, created through native create_worktree from that exact commit, detached HEAD; initial status clean.
- Implementation/build owner: delivery_focus only. Coding Agent owns Studio and the live checkout/builds; coordinator owns its separate integration worktree.
- Never edit live checkout, checkpoints, Studio, client/service/agent, protocol, package contract, gates, CMake, global settings, or another owner's files/build outputs.
- Commit identity when independently accepted: `khoasoma <songtu438@gmail.com>`; zero attribution. The explicit user autonomous delivery mandate authorizes routine commits/integration; coordinator controls integration.
- Work integrated into main remains INDEV. No claim of physical live delivery, capture provenance, stream FPS/latency/capacity, native runtime, or production readiness follows from these tests.
- No service installation, remote input/control, capture, or policy changes on the main host. Performance qualification is separate and NOT TESTED unless a safe real two-endpoint lab is established.
- AGENTS.md was read. WORKSPACE_SCOPE.md was not found in the supplied/live roots; coordinator was asked for its authoritative path. Investigation is limited to this worktree and the explicitly authorized source/reference paths.

### Exact write allowlist

| File | Owned section | Baseline SHA256 |
|---|---|---|
| `server/app/main.cpp` | DashboardState Focus stream fields; loop block from `// Live streaming` through `dashboard.focus_stream.tick`; Focus shutdown stream cleanup only | `5a73b2879b19b3796636e2368dd4a52b6f54d38a54e918f5b6141fab5124dc60` |
| `server/app/focus_stream_observer.hpp` | Existing Focus observer plus narrowly scoped stream request decision seam | `f04cda6c6d772d2815f220fb23460a8eeab26b81129e420c8a045328cbd9aaec` |
| `tests/focus_stream_observer_tests.cpp` | Add production decision seam + observer regressions, retain all current assertions | `b59b0c6fdd34d0eda2550805020e477380a863b8070a55a0de3d4de0bb04efc6` |
| `tasks/task-plans/autonomous-focus01-owner-transition.md` | This plan and durable acceptance/evidence record | New file |

Read-only dependency hashes: `server/include/nstu/stream_rearm_watchdog.hpp` = `4460019b4d1c7049e979c4872618d42bc3a451a7d7c257c65729b3d3641a1c94`; `scripts/verify-gate.sh` = `1e0683d109608dac248e5b96b6ede5d165cbdd96c7e8018acfa78e29953609ea`; root `CMakeLists.txt` = `0997335a1ec0cb6a7a15df52d4722909011857399e743eee9e7ca39421a5c8d6`.

The observer header is actually under `server/app`, not `server/include/nstu`. No new product files or target registration are needed. Build/log/receipt outputs go only into this owner's ignored `build/focus01-evidence`, `build/mingw`, and `build/gate`.

## Reconciled defect and failed integrated caller scenario

At the pinned source, main.cpp sets `legacy_remote` when a valid remote window owns client 7. Closing the window can leave Focus selected on client 7, so desired target stays 7 and owner becomes Focus. The caller starts only when `streaming_client_id != desired_stream_id`; no start is emitted on this owner-only change. The observer resets under Remote ownership and accepts only a Focus start/rearm success. Continued increasing generations reset the watchdog and prevent its recovery request indefinitely. The result is an unverified Focus presentation despite continuing usable frames.

Existing `verify_owner_and_target` manually injects `start_sent` after the transition; it cannot prove the actual caller generates that outcome. Baseline focused observer compile/run passed, output `Focus request, ownership, delivery, freshness and FPS observations: PASS`; build and run exit 0. Logs under `build/focus01-evidence/baseline-{build,run}.log`; SHA256 respectively `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` and `1cf4e7e8f59e6d00e86293c0f3b8d704ffd01266a330c5c74411b67642b200d6`.

## Tasks

### 1. Independently accept this plan

- [x] Recheck exact source/dirty status, read rules and required skills, create own worktree, hash allowlisted files, run baseline focused tests.
- [x] Send PLAN_READY with this path, head, and plan hash. Wait for independent PLAN_ACCEPT. Address review comments in this plan before product edits.

### 2. Expose and reproduce the production caller defect (RED)

- [x] Extract the existing ID-only request/watchdog decision without fixing its owner condition into the existing header; wire the dashboard caller to this seam. Preserve stop/start rates, synchronous real send result handling, watchdog timing, and diagnostics.
- [x] Add tests using the same seam plus the real observer: Remote owns client 7; generations advance; Focus assumes client 7; generation advances after transition. Assert a real Focus request is selected and its completed outcome permits requesting then Live only after a newer usable generation. Confirm failure against the extracted ID-only behavior and save exact command/output/hash.
- [x] Additional regressions cover Focus to Remote to Focus same ID with uninterrupted frames, failed start with continued frames, successful request pending report/no new generation, changed target, stale completion under superseded owner/target/request, stop to late completion to restart, and two-second watchdog behavior. The initial integrated handover case is the captured RED; the other groups are GREEN lifecycle guards, not separate claimed baseline failures. No independently reimplemented fake caller replaces the production seam.

### 3. Fix owner transitions and request result scoping (GREEN)

- [x] Track `(target, owner)` explicitly; either changing causes one new start. Stop the old target only when target changes (including none), preserving capture on a same-ID handover.
- [x] Reset/reseed the unchanged two-second watchdog on owner/target transition, then preserve generation-driven liveness and once-per-window first/repeat behavior. Never send both edge start and rearm on the same transition tick. Keep requested rate 15 and stop rate 0.
- [x] Every selected request carries target, owner, kind, and monotonically changing local serial. Complete only the current matching request; superseded/duplicate delayed results cannot produce a Focus accepted outcome. Remote-owned completions cannot certify Focus. Serial is local caller protection, not capture provenance or a wire epoch.
- [x] Wire actual `set_streaming` bool results to start_sent/start_failed/rearm_sent/rearm_failed. A sent request alone stays requesting/rearming. A failed request cannot earn Live from continuing frames; recovery uses the preserved watchdog policy.
- [x] Keep target/owner mismatch and stale texture generation checks. No new visual design, strings, platform dependencies, or capture metadata.
- [x] Run focused tests and record GREEN outputs and post-edit hashes.

### 4. Verify, obtain independent CODE_ACCEPT, and deliver

- [x] Configure an owner-local MinGW Debug `build/mingw`; run the repository fast gate unpiped, retaining output and real exit status.
- [x] Coordinate strict-gate execution with coordinator because the unchanged gate writes fixed shared `/tmp/nstu-cfg.log`; run repository strict gate in own `build/gate` with zero warnings and full CTest. Do not change or weaken gates, tests, or CMake.
- [x] Compile the actual `nstu-server` caller through the full gates; targeted assert tests exercise the production seam, not merely arbitrary observer outcomes. Attempted sanitizer-supported focused build; installed MinGW lacks ASan/UBSan libraries, reported explicitly.
- [x] Record exact diff, source hashes, command exits, logs/hashes, and limitations in this plan/evidence. Check allowlist and unchanged read-only hashes.
- [x] Send CODE_READY to coordinator with exact cumulative diff/evidence; receive independent CODE_ACCEPT before branch/commit. No required rejection findings.
- [ ] After acceptance, create task branch if needed, commit allowlisted changes with required identity, send commit hash and receipts to coordinator. Coordinator integrates and independently tests exact integrated commit. No push/merge from this owner unless explicitly assigned.

## Acceptance matrix

| Scenario | Required result |
|---|---|
| Remote(7) -> Focus(7), generations uninterrupted | Exactly one new Focus start; actual success yields requesting; baseline frame stays unverified; newer usable generation under reported streaming can earn Live |
| Focus(7) -> Remote(7) -> Focus(7) | Explicit one-owner transitions each reassert start; Remote frames/results never preserve Focus acceptance |
| Start/rearm fails, generations keep advancing | request_failed; no fabricated Live or per-frame retry flood |
| Request sent but report/frame pending | No Live/FPS sample until current evidence conditions pass |
| Target 7 -> 9 / desired and selected disagree | Stop old, start new; old frame/selected mismatch and old result cannot certify new target |
| Late completion after owner change, target change, newer rearm, or stop | Rejected safely by current request identity; cannot grant Focus authority |
| Stop -> late frame/result -> restart same target | Clear owner and watchdog; restart emits fresh start, baseline late frame alone not Live |
| Watchdog | No rearm before 2000 ms; first at boundary, repeat only after another full window; generation advances end episode; transition reseeds without same-tick duplicate |
| Offline/texture unavailable/generation reset | Existing observer masks retained; no stale target/texture earns Live |

## Durable state

- 2026-11-07: PLAN_READY pending independent review. No production edits. Baseline focused test passes; integrated owner handover remains source-proven and will be captured as RED through the production seam after PLAN_ACCEPT.
- Capture-to-display performance, two-endpoint runtime and throughput: **NOT TESTED**. This fix does not resolve capture-mode/request provenance, invalid-JPEG availability recovery, or transport/pipe performance findings.
- 2026-11-07: Independent PLAN_ACCEPT by delivery_pairing for original plan SHA256 `534cd8cde711c7ad587a3e3890c4d5a19905e1c61736d6102497d83209488832`; receipt in sibling `build/pair01-evidence/focus01-plan-review.json`, SHA256 `bc2346b0dac68d96ef38a7abc82007616dbfdbdcbe768e739ee9c61cb666a33f`. Coordinator admitted acceptance before any product edits.
- RED: shared production `FocusStreamRequests::advance`, wired into main.cpp with extracted ID-only behavior, failed same-client handover assertion `handover.request.client_id == 7`; warning-clean compile exit 0, assertion run exit `-1073740791`. `build/focus01-evidence/red-production-seam.patch` SHA256 `68c8cf5a5313c0af1b5d3e7fb853ed8b524a0b5e372b9a6e4b812d4ddacab220`, RED run log SHA256 `7c91ced9658a845f79eebb91ff7c41d7618d0929dc675df27cb4df3a362ba55c`.
- Focused GREEN: all original tests plus five production-seam lifecycle groups compile with C++20 `-Wall -Wextra -Wpedantic -Werror` and run exit 0. Includes both same-ID directions, successful/failed/pending real-result conversion, stale/duplicate/superseded completions, target switch, live-evidence stop/restart, and exact 2000ms watchdog first/repeat/reset. Source-bound receipt `build/focus01-evidence/green-result.json` SHA256 `6c21be9b5da3d29d45514985e7bce0d19fa5a9b38177689cc81f2f7e689cf16c`.
- Sanitizer capability: attempted GCC ASan+UBSan build failed at link (exit 1), libraries `-lasan` and `-lubsan` unavailable in installed MinGW; `build/focus01-evidence/sanitizer-build.log`. No sanitizer execution/pass is claimed.
- Owner performance target clarified via coordinator: **one** Focus target, **1280x720**, **15 FPS**, approximately **100ms or less display latency**. Qualification remains **NOT TESTED**. Owner will perform ordinary-installer/GUI physical acceptance later; no VM is assumed and no command-line testing is assigned to the user.
- Full fast/strict gate lease queued behind Coding Agent C01. Only owner-local direct focused builds may run before handoff. Independent code review may run read-only in parallel; no commit before CODE_ACCEPT and full gate results.
- Full-gate lease granted by coordinator, then both gates completed and lease released. Fast and strict each exit 0, zero warnings, 74/74 CTests; browser recovery 63 plus two presentation and two finalization regressions pass. Debug and Release/WERROR=ON compile the actual `nstu-server`; raw focused CTest 1/1 per configuration passes. Final source/log/binary receipt `build/focus01-evidence/final-verification.json`, SHA256 `2ac591859a97be5be62a747eef9ffad225dce29133a12787edf63ec26cbe9f43`.
- CODE_READY sent to coordinator. Production source hashes: main.cpp `10f126ff697c8c76ad3bf0d324f07cc780fb2053d0088742e3e79f19a55f1229`; observer header `ff4d021035af1ad1b952ad5c4dea3cc52b05036c28d34e7ea83c5691c16dbcbb`; tests `093543f7bc4583951f890a091ee80c391cc6dbf0bec5948b73f102105d3ffcd9`. No staging, commit, CI, or integration acceptance yet. Request state is UI-thread owned; sequential late-completion identity guards do not make the helper a cross-thread synchronization primitive.
- Independent CODE_ACCEPT granted by coordinator/nonauthor with delivery_pairing pre-review. Coordinator re-read the cumulative source diff, validated all 14 source/log/binary hashes, parsed both actual full gates, and independently rebuilt/reran the focused tests with C++20 `-Wall -Wextra -Wpedantic -Werror` (build/run exit 0). Receipt `D:/Codex/design-drafts/nstu-autonomous-delivery-20261107/operations/focus01-code-review-r1.json`, SHA256 `7ec84b0b24880aba6cbbb372dbf548923eb8dc9d7fc9743aaa03ba1649ebbf87`. Component commit explicitly authorized for the four allowlisted files only; root retains integration, push, CI and release ownership. Final commit/tree identifiers are recorded in own ignored delivery receipt to avoid a self-referencing commit hash.
