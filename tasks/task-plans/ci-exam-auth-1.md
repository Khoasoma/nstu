# CI-EXAM-AUTH-1 — synchronize protection-proof setup after authentication

## Objective, evidence and allowlist

Repair the missing asynchronous readiness precondition in the existing auth
integration test. Exact-revision CI run 40606900983 at `4ec7b31` failed its initial
`launch_exam` assertion at line 376; all other targets and both installer jobs
passed. Fifty unchanged local GCC Release repeats passed. Do not claim local RED
or a conclusively observed error string: the failed CI did not capture it.

Root and a fresh read-only diagnosis identified a feasible ordering race.
Handshake processing holds the connection mutex, publishes the registry entry,
then demotes its old UWF proof. The size-only test wait can inject a new proof
between publication and demotion; launch correctly waits for authentication and
can then reject the demoted proof. Authenticated hello processing uses that same
mutex afterward and publishes the reported hostname/online status. The existing
deadline fixture already uses that observable readiness pattern.

Primary product allowlist: `tests/control_plane_exam_auth_tests.cpp` only.
No production, header, CMake, auth, SAS/UWF, telemetry or answer-policy changes.
Any separate production concern needs its own scoped plan and genuine evidence.
Root owns recovery state, writer/build claims, review, explicit staging and CI.

## Acceptance groups

1. Preserve the original size-one wait, snapshot/identity checks, proof setup and
   every launch/auth/UWF assertion byte-exact. Add one bounded wait before proof
   injection that observes the same registry ID with the authenticated hello's
   expected hostname and online status. Use the existing `wait_until` and its
   unchanged 300-attempt/10 ms budget. Explain why this proves handshake cleanup
   completed. No retry around `launch_exam`, extra sleep, longer timeout or
   suppression is permitted.
2. Keep the repair additive and test-local. The final launch still executes once
   and all exact identity/session/telemetry/answer/reconnect checks still run.
   Preserve the failing CI/logs and unchanged 50-run baseline as honest evidence.
   Confirm source ordering and registry `upsert` preservation independently; the
   separate unverified direct `start_exam` check/use concern is recorded without
   silently expanding this fixture repair.
3. Rebuild and run the focused test, meaningful Release repeats, then independent
   review and Root fast/strict gates. All relevant assertions remain, with no
   production diff or historical acceptance changes. Publish the reviewed passing
   repair with the authorized identity and verify all three required Windows
   CI jobs for its exact new revision. Do not clear the observed CI blocker or
   start pristine-initialization implementation merely on a rerun of old code.

## Verification and recovery

```bash
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake --build build/mingw --target nstu_control_plane_exam_auth_tests -j 4
ctest --test-dir build/mingw --output-on-failure --timeout 60 -R '^nstu[.]control_plane_exam_auth$'
./scripts/verify-gate.sh
./scripts/verify-gate.sh --strict
ctest --test-dir build/gate --output-on-failure --timeout 60 --repeat until-fail:20 -R '^nstu[.]control_plane_exam_auth$'
```

Root first binds current HEAD, plan/checklist and exact source bytes and assigns
one writer/build owner for `build/mingw` and `build/gate`. Save immutable command
intents, PID/creation identities, full output/exits and matching hashes before
and after each phase. No duplicate build or CI operation. Preserve unrelated
README/About edits and private evidence; never publish the original roadmap,
`_agents/`, `artifacts/`, `.claude/` or `HANDOFF.md`. Required Opus/peer/VM and
unfinished server integration still gate main after this CI repair qualifies.
