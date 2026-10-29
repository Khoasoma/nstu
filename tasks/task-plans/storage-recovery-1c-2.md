# STORAGE-RECOVERY-1C-2 — exact ACK retirement with a blocked barrier

## Objective, dependency and allowlist

Verify exact accepted/duplicate ACK retirement after cleaned payload publication
while deleting the protected expiry barrier is blocked. Dependency: 1C-1 is
reviewed, published as `7ad796076ec296549eb16dad35b3a0a53b0aaf2e`, and all three
required Windows CI jobs passed. Root rebinds this plan and the current source
before assigning one writer/build owner. The private read-only ACK proposal is
planning evidence; this scoped plan is the implementation contract.

Primary product file: `tests/exam_telemetry_outbox_tests.cpp`.
Conditional file: `common/src/exam_telemetry_outbox.cpp`, only after a genuine
failing regression proves a defect and Root records a scoped repair intent.
No header, coordinator/router, service wiring, CMake, codec, answer, format, quota,
retention or producer/sequence change. Purge/clear belong to 1C-3. Reuse existing
fixture helpers without removing assertions or introducing a fault framework.

## Acceptance groups

1. Add exactly two named Windows cases, accepted and duplicate, sharing one
   readable runner. Reuse `MixedCleanupFixture`, its protected disk reader and
   full loss comparison. Assert the original interleaved fixture and establish
   both real lock stages: payload sharing excludes delete, prune at B returns
   zero with the atomic-rename error and publishes a valid protected barrier B;
   lock that existing barrier, release the payload handle, then prune at B-1
   returns zero with the barrier-cleanup error after real cleaned R1/R2 payload
   publication. Check exact encodings/hashes/local arrivals, current S conflict 1
   and expired 2, stale L absence and the original barrier bytes. Capture all S
   fields, protected payload and mtime. No simulated cleaned store or sleeps.
2. While the barrier handle stays held, a wrong hash for R1 and exact ACKs for
   expired E1/E2 are ignored without changing payload, mtime, barrier or S loss.
   Then an exact case-status ACK for R1 returns `retired` with no error and
   publicly persists only R2. Add a small one-record state assertion rather than
   weakening the fixture's existing two-retained-record assertion: one pending
   and disk batch, exact R2 encoding/hash/local arrival, exact byte count and
   active S, only S loss with every field unchanged, public stale L zero and
   unchanged barrier bytes. Repeating retired R1's ACK is ignored without a
   write or loss update. Do not require blanket ACK failure from the barrier.
3. Preserve gates and original retention through recovery. With the barrier
   locked, offer, valid enqueue, alternate-path switch and fresh-instance reopen
   stay refused; the original instance stays open, exact R2-only payload/mtime,
   barrier, loss and alternate protected payload/sidecar/numeric sentinel stay
   intact. Release the handle. Same-path open at B-1 removes the barrier without
   rewriting payload or loss and offers only R2. Close/fresh reopen at B-1 and
   repeated rollback reopen at B-2 retain its original arrival and exact loss;
   E1/E2/R1/L never return and expiry is not counted again. Preserve alternate
   bytes until the existing fixture's later successful alternate open/cleanup.
   Register both named cases in normal execution; an optional selector retains
   default calls. Preserve every original ACK, mixed-cleanup, protected-barrier,
   rollback and purge/clear assertion. Report first-run passing coverage honestly.

## Verification and recovery

```bash
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake --build build/mingw --target nstu_exam_telemetry_outbox_tests -j 4
ctest --test-dir build/mingw --output-on-failure --timeout 60 -R '^nstu[.]exam_telemetry_outbox$'
ctest --test-dir build/mingw --output-on-failure --timeout 60 -R '^nstu[.]exam_telemetry_(outbox|service)$'
./scripts/verify-gate.sh
./scripts/verify-gate.sh --strict
```

Use real DPAPI/public durable publication, synthetic B/B-1 from one fixture time,
successful Windows lock handles, unpiped commands and one build owner. Root saves
immutable intents, PID/creation identity, complete output/exits and exact plan/
source hashes before and after each phase. Only actual RED permits a conditional
repair. Fresh supporting review and independent Root gates precede publication;
the exact feature revision receives all three required Windows CI jobs.

This tests the public outbox contract. Coordinator forwarding, server ACK delivery,
required Opus/peer acceptance and real VM qualification remain separate integration
requirements. No historical boxes are ticked and main remains gated. Preserve
unrelated README/About edits. Never publish the private original roadmap or local
evidence under `_agents/`, `artifacts/`, `.claude/` or `HANDOFF.md`.
