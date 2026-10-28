# AUTH-ADMISSION-1A — current-carrier registry and proof ownership

## Objective, dependencies and allowlist

Build the coherent ownership foundation for the confirmed P1 admission finding.
Part 1A fixes replacement/proof/registry ownership; part 1B must subsequently
revalidate admission and install its context at that ownership boundary. Part 1A
alone does not establish complete admission correctness or clear the main blocker.
The source review and three bounded adversarial proposal cycles are saved in
local recovery evidence. All actionable findings were reconciled; the final
cycle found none. Runtime RED/GREEN remains required. Cross-model review was
skipped because this is a non-interactive heartbeat run.

Dependency: reviewed `bc21bf8` fixture repair and all three exact Windows CI jobs
passed. Bind current HEAD, this plan/checklist and actual source bytes before one
writer/build owner begins. Product allowlist:

- `server/src/control_plane.cpp`
- `tests/control_plane_exam_auth_tests.cpp`

No public header, wire, CMake, registry API, answer, telemetry codec, SAS/UWF
predicate or DEV exception change. Root owns shared plans/state, independent
verification and explicit publication. Preserve every original test assertion,
fixture and the prior ten-line readiness repair.

## Acceptance groups

1. Prove stale/removed ownership through actual authenticated loopback before
   repair. Reuse the existing source-included `NSTU_CONTROL_PLANE_AUTH_TESTING`
   TU and disconnect-order helpers. Add only narrowly guarded one-shot rendezvous
   for post-auth-accept/pre-publication and pre-registry-mutation/completion;
   callbacks must not exist in the production static library. Park an already
   authenticated A report/status callback, complete same-client B replacement and
   demotion, then release A and observe completion. Assert B's proof/hostname/
   last-seen state is not restored/touched by A. Also park B after auth acceptance
   before registration, close its real channel and observe disconnect removal,
   then release publication: A's live ownership/proof remains, and B is not
   republished. Capture genuine failing assertions on unchanged production
   behavior with test seams; no scheduler-luck repeats or fabricated VM proof.
2. Apply a narrow common live/current-owner check under `states_mutex_`: exact
   live state entry, exact connection identity/current registry mapping, and
   authenticated stage. Replacement holds its connection mutex then a short
   ownership scope; reject an erased state before publishing, and make mapping,
   old-proof retirement and registry publication coherent. All authenticated
   registry mutations (UWF reports, hello/status, heartbeat, freeze, snapshot)
   validate carrier and mutate in one short ownership scope; decode and any
   disconnect remain outside it. Include `begin_uwf_fleet_operation` proof
   retirement in the same serialization and release before send/audit. Preserve
   current-owner versus late superseded disconnect behavior, boot/operation
   checks, valid-current-carrier outcomes and offline fleet-operation semantics.
   No global ownership lock may cover transport, persistence, callback reentry
   or waits for context/state mutexes. Do not blanket-lock answer/audit handlers.
3. Green controls use B's valid wire proof/status and show current updates still
   work, old callbacks cannot restore metadata, late A disconnect preserves B and
   B's own disconnect retires its proof. All original exact identity/session/
   answer/telemetry/launch assertions still execute. Report real RED and GREEN,
   guard placement and lock order; do not claim physical UWF/VM qualification.
   Fresh code review, independent Root gates and exact feature Windows CI follow.
   Keep part 1B explicitly unfinished: admission's copied proof/current-owner
   decision still needs its separately frozen context-installation plan.

## Verification and recovery

```bash
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake --build build/mingw --target nstu_control_plane_exam_auth_tests -j 4
ctest --test-dir build/mingw --output-on-failure --timeout 60 -R '^nstu[.]control_plane_exam_auth$'
ctest --test-dir build/mingw --output-on-failure --timeout 60 -R '^nstu[.](control_plane|control_plane_exam_auth|server_state)$'
./scripts/verify-gate.sh
./scripts/verify-gate.sh --strict
```

Root freezes source/plan bindings, records one writer and build owner for
`build/mingw` and `build/gate`, and saves immutable intents, PID/creation records,
full output/exits, RED/GREEN and final hashes before/after phases. Stop and report
any needed file expansion or contradiction; preserve all partial work. Review
the actual implementation after the proposal review, then all three required
MSVC Release CI jobs for the exact revision. Main remains blocked by part 1B,
unfinished server integration and required Opus/peer/VM acceptance. Never publish
the private original roadmap, `_agents/`, `artifacts/`, `.claude/` or `HANDOFF.md`;
preserve unrelated README/About and historical checklist boxes.
