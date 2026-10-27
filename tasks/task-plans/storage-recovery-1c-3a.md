# STORAGE-RECOVERY-1C-3A — safe removal after committed cleanup

## Objective, dependencies and allowlist

Verify successful purge and clear after cleaned payload publication while expiry
barrier deletion remains blocked. The read-only removal proposal splits 1C-3
into this success unit and a separate 1C-3B mixed replacement-failure/revocation
unit. Each gets its own writer, verification and publication. The parent matrix
is incomplete until both are verified.

Dependency: reconcile 1C-2's reviewed publication `049a11e` and exact Windows CI,
then bind this plan to the resulting HEAD/source/checklist bytes before coding.
Root owns shared plans, state, writer/build claims, review and publication.
Primary product file: `tests/exam_telemetry_outbox_tests.cpp`.
Conditional file: `common/src/exam_telemetry_outbox.cpp`, only after a genuine
failing regression proves a defect and Root records a small repair intent.
No header, service/coordinator/router, CMake, codec, answer, format, quota,
retention, authorization or producer/sequence change. Preserve every assertion.

## Acceptance groups

1. Add two named Windows cases, successful purge and successful clear, sharing
   a readable runner and a narrow zero-record assertion. Reuse the unchanged
   `MixedCleanupFixture`, real public publication and protected disk/loss readers.
   Check original E1/R1/E2/R2 and S/L loss. A successful payload handle excluding
   delete sharing plus prune at B must produce the actual atomic-rename error,
   exact old payload and a protected barrier decoding to B. Lock that existing
   barrier, release the payload handle and prune at B-1: require the barrier
   cleanup error after actual cleaned R1/R2 publication. Verify full retained
   encodings/hashes/local arrivals/bytes, active S, conflict 1/expired 2 and stale
   L absence. Snapshot every S field, cleaned payload and original barrier bytes.
2. Call the selected removal once while the barrier stays locked. It succeeds
   with no error and leaves the instance open with zero records/bytes/pending
   batches and a checksum-valid protected store with no batches or arrivals.
   Purge preserves active S and exactly the captured S loss, including both
   timestamps; clear resets active session, disk losses and all public losses.
   Public stale L remains zero. Protected payload changes, the barrier stays
   byte-exact and no revoked marker is created. Capture resulting bytes/mtime;
   do not require filesystem mtime to increase. Offers, a valid enqueue, alternate
   path switch and fresh-instance reopen remain refused until barrier deletion
   succeeds. Each refusal preserves resulting payload/mtime, exact expected loss,
   barrier and alternate protected payload/sidecar/numeric sentinel bytes.
3. Release the barrier handle before cleanup. Same-path open at B-1 removes it
   without rewriting the empty protected payload or changing loss. Close/fresh
   reopen at B-1 and repeated rollback reopen at B-2 remain empty with no E/R/L
   resurrection; purge retains S expired exactly 2 and clear stays reset. Keep
   alternate bytes unchanged until the existing fixture's later successful
   alternate open/cleanup. Register both cases in default Windows execution;
   an optional selector keeps all default calls. Preserve existing two-record
   fixture assertions, ACK cases, raw/protected barriers, generic purge failure
   and every baseline assertion. Report first-run passing coverage honestly.

## Verification and recovery

```bash
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake --build build/mingw --target nstu_exam_telemetry_outbox_tests -j 4
ctest --test-dir build/mingw --output-on-failure --timeout 60 -R '^nstu[.]exam_telemetry_outbox$'
ctest --test-dir build/mingw --output-on-failure --timeout 60 -R '^nstu[.]exam_telemetry_(outbox|service)$'
./scripts/verify-gate.sh
./scripts/verify-gate.sh --strict
```

Use real DPAPI/public durable writes, actual successful Windows lock handles and
one captured fixture time B/B-1 without sleeps or simulated cleaned stores.
There is one build owner for `build/mingw` and `build/gate`. Root saves immutable
intents, PID/creation identities, full outputs/exits and matching plan/source
hashes before and after each phase. Actual RED precedes any conditional repair.
Fresh supporting review and independent Root gates precede publication; all
three required Windows CI jobs must apply to its exact feature revision.

This unit covers public outbox success semantics. 1C-3B failure recovery,
pristine initialization, required Opus/peer/VM evidence and unfinished server
integration remain separate requirements; main remains gated. Never publish the
private original roadmap or local evidence. Preserve unrelated README/About work.
