# STORAGE-RECOVERY-1C-1 — mixed cleanup stages and path isolation

## Objective and allowlist

Verify the two real cleanup failure stages with mixed retained/expired batches
and loss metadata, then prove that pending cleanup cannot touch another outbox.
Dependency: 1B writer and independent review completed; rebind its committed
test helpers and source bytes. Baseline feature HEAD is
`0b535d14096e260905ab9e977708c46f7d7529ba`; its exact Windows CI is tracked separately.

Primary product file: `tests/exam_telemetry_outbox_tests.cpp`.
Conditional file: `common/src/exam_telemetry_outbox.cpp`, only after an actual
failing assertion proves a defect and Root records a small repair intent.
No header, coordinator, CMake, answer, codec, quota, retention or authorization
change. Safe ACK retirement and purge/clear matrices are later 1C-2/1C-3 units.
Do not extend this first unit into those tests or a generic fault framework.

## Fixture and failure preconditions

Reuse the protected-file reader introduced by 1B and existing batch/path/clock
helpers. Build durable fixtures through real open/enqueue/terminal-ACK publication.
Capture local time `t` once; choose `B = t + 3600000` and test B/B-1 without sleeps.
Seed a drained stale loss context L with one rejected ACK stamped B minus
retention, then a current loss context S with one conflict ACK at t. In S enqueue
interleaved E1, R1, E2, R2: E producer stamps equal B minus retention; R stamps
equal t, with distinct sequences/sample contents. Capture canonical encodings,
hashes, payload bytes and total encoded-byte count. Verify the real protected
fixture round-trip/checksum and initial loss counters before locking. Keep mtimes
fresh so old whole-file age cleanup cannot mask the intended stage.

A separate protected path B-path contains one original eligible batch, a valid
protected expiry sidecar stamped t and an owned numeric temporary sentinel.
Capture exact bytes for all three before an attempted path switch. These are
isolated exact owned paths, with no traversal or alias-normalization requirement.

Use real `CreateFileW` handles allowing read/write sharing but excluding
`FILE_SHARE_DELETE`. Assert each lock succeeds and release handles before cleanup.
First lock A's payload and prune at B to generate a valid protected barrier B and
prove payload replacement actually failed. To reach deletion-after-publication,
lock that existing barrier, release the payload lock, and retry at B-1. Cached B
allows cleaned payload publication before barrier removal fails. Never install
a simulated cleaned store or count failed initial barrier publication as this
second stage. Use existing production DPAPI entropy and atomic replacement.

## Acceptance groups

1. Payload replacement failure preserves the mixed state. Prune returns zero
   with a real replacement error, payload bytes stay exact, protected barrier
   decrypts to B, pending encodings remain E1/R1/E2/R2 in order and bytes stay at
   their original total. Current S has conflict 1/expired 0; observed-expired L's
   public getter is zero even though its bytes remain in the old protected file.
   Offers and a valid enqueue remain refused at rollback time. A refused switch
   to B-path leaves A open and both paths' captured state/files untouched. After
   unlocking, same-path rollback recovery commits only E1/E2 expiry, retains
   exact R1/R2 order/bytes, removes stale L and records S conflict 1/expired 2
   once. Fresh reopen and repeated rollback never restore E or duplicate loss.
2. Barrier deletion failure preserves committed cleanup. Require the two-lock
   precondition, the actual barrier-cleanup error and a real changed protected
   payload containing only R1/R2; exact retained encodings, byte count and S
   conflict 1/expired 2 agree in memory and on disk, and L is absent. Barrier
   bytes stay unchanged. Offers, valid enqueue and alternate-path open remain
   refused without rewriting payload or counting E again. Fresh-instance open
   remains closed while sidecar deletion is locked. After unlock, same-path
   rollback/restart recovers only R1/R2 and unchanged loss, removes the barrier,
   and offers retained evidence. Do not infer rollback from public count zero.
3. Keep this thin and additive. Share one readable mixed fixture between two
   named cases and register both in normal execution; optional selectors must
   retain default calls. B-path payload/sidecar/temp bytes remain exact during A
   refusal and recovery. Only a later successful B-path open performs B-owned
   cleanup and recovers its original eligible batch. Preserve all existing
   assertions, raw aged-barrier coverage and 1B protected cases. Report already
   passing coverage honestly; retain genuine RED before any conditional repair.

## Verification and recovery

```bash
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake --build build/mingw --target nstu_exam_telemetry_outbox_tests -j 4
ctest --test-dir build/mingw --output-on-failure --timeout 60 -R '^nstu[.]exam_telemetry_outbox$'
ctest --test-dir build/mingw --output-on-failure --timeout 60 -R '^nstu[.]exam_telemetry_(outbox|service)$'
./scripts/verify-gate.sh
./scripts/verify-gate.sh --strict
```

Root freezes this plan/source/checklist hashes and one writer/build owner before
implementation. Save command intents, PID/creation records, full output, exits,
preconditions and matching hashes in the local operation before/after each phase.
Fresh supporting review and Root gates precede explicit publication; the exact
feature revision receives all three required Windows CI jobs. Never publish the
private original roadmap, local evidence or unrelated README/About edits. Required
Opus acceptance, peer/runtime and unfinished server integration still block main.
