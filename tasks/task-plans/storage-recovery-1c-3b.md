# STORAGE-RECOVERY-1C-3B — mixed removal failure and revocation recovery

## Objective, dependency and allowlist

Verify failed purge and clear during observed expiry cleanup with a real payload
replacement lock. Prove durable revocation fallback, refusal while deletion is
blocked and empty fresh-instance recovery. Dependency: reconcile reviewed 3A
publication and exact Windows CI, then freeze current HEAD/source/checklist
hashes and one writer/build owner before coding. Root owns shared plans/state,
review and publication. The read-only removal proposal supplies the split; this
plan corrects its diagnostic expectation using the actual first-error behavior.

Primary product file: `tests/exam_telemetry_outbox_tests.cpp`.
Conditional file: `common/src/exam_telemetry_outbox.cpp`, only after a genuine
failing regression proves a defect and Root records a small repair intent.
No header, service/coordinator/router, CMake, codec, answer, format, quota,
retention, authorization or producer/sequence change. Preserve every baseline
assertion and the existing generic purge-failure coverage.

## Acceptance groups

1. Add two named Windows cases, failed purge and failed clear, with one thin
   runner. Use the unchanged mixed fixture and real protected/public publication.
   Assert original E1/R1/E2/R2, S/L metadata, canonical bytes/hashes/arrivals and
   fresh payload mtime. Hold a successful payload handle allowing read/write
   sharing but excluding delete. Prune at B must fail replacement with zero
   count, exact original payload/mtime and a valid protected barrier decoding
   to B. Memory retains exact original records/bytes and appropriate S loss;
   observed-expired L is hidden. Capture full pre-removal S. Keep the handle held.
2. Clear the diagnostic before the selected removal. It returns false and
   closes the old instance with zero pending records/bytes. Purge intentionally
   keeps full current S loss (conflict 1/expired 0) in memory and drops L; clear
   resets both default and per-session public losses. Neither failure rolls payload memory
   back. Disk retains exact original protected bytes/mtime and mixed data, and
   the protected barrier remains unchanged. A real atomic `.revoked` publication
   must contain exactly one byte 1; the payload handle proves deletion is still
   blocked. The first diagnostic is the actual atomic-rename failure: existing
   `set_error` preserves the first nonempty error, so the later fallback message
   need not replace it. Do not assert the proposal's overwritten final-message
   assumption. Old same-path open remains refused by its revocation latch, offers
   and valid enqueue remain refused, and alternate-path open remains refused by
   old-path expiry ownership. A fresh instance at B-1 refuses while marker-driven
   deletion is locked and has no records/bytes/offer. Every refusal preserves
   payload/mtime, barrier, marker and alternate protected payload/sidecar/temp.
3. Release the payload handle. A fresh instance at B-1 deletes old payload and
   marker, publishes a real protected empty store, removes the expiry barrier
   and offers nothing. Its active session/losses reset completely; failed purge
   does not promise aggregate preservation after destructive fallback. Close/
   fresh/repeated rollback reopen at B-2 stays empty, with no marker/barrier or
   E/R/L resurrection. The original failed instance stays latched closed even
   after fresh cleanup. Prove that before moving the recovered object into the
   owned fixture to reuse its existing successful alternate-path finish; preserve
   every finish assertion. Keep exact owned marker setup/cleanup isolated so
   reruns cannot inherit their own failure. Register both cases in default
   Windows execution; an optional selector preserves all default calls. Report
   actual exits honestly; expected operation failures are passing preconditions,
   and a mistaken new assertion is not evidence of a production defect.

## Verification and recovery

```bash
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake --build build/mingw --target nstu_exam_telemetry_outbox_tests -j 4
ctest --test-dir build/mingw --output-on-failure --timeout 60 -R '^nstu[.]exam_telemetry_outbox$'
ctest --test-dir build/mingw --output-on-failure --timeout 60 -R '^nstu[.]exam_telemetry_(outbox|service)$'
./scripts/verify-gate.sh
./scripts/verify-gate.sh --strict
```

Use actual DPAPI, Windows locks and public durable writes, with captured B/B-1
time and no sleeps, injected clean stores or fault framework. One build owner
holds `build/mingw` and `build/gate`. Root saves immutable intents, PID/creation
identities, complete outputs/exits and exact plan/source bindings before and
after each phase. Actual RED precedes any conditional production repair. Fresh
supporting review and independent Root gates precede publication, then all
three required Windows CI jobs apply to its exact feature revision.

This closes the scoped public outbox removal matrix only. Pristine initialization,
server integration and required Opus/peer/VM acceptance remain separate; main
remains gated. Never publish the private original roadmap or local evidence.
Preserve unrelated README/About work and all historical checklist boxes.
