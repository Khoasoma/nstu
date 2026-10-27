# STORAGE-RECOVERY-1B — protected expiry-barrier rejection

## Objective and scope

Add meaningful missing boundaries for the existing protected expiry barrier.
Primary product file: `tests/exam_telemetry_outbox_tests.cpp`.
Conditional file: `common/src/exam_telemetry_outbox.cpp`, only if a genuine runtime
failure proves a defect requiring repair. No new production behavior is expected;
already-passing cases are coverage, not fabricated RED.

Keep answer paths, coordinator code, public APIs, formats, quotas, expiry and
authorization policy untouched. Root saves source/operation baseline and assigns
one writer before tests. The prior storage planner inspected actual barrier load
and existing raw malformed/31-day cleanup coverage; do not duplicate that test.

## Acceptance groups

1. Real protected malformed payloads are refused. Use successfully protected
   seven-byte, nine-byte and eight-byte-zero plaintext; verify each original
   protection/decryption precondition. Separately corrupt a valid protected blob
   containing an eight-byte nonzero time and verify refusal. Use the existing
   machine-protection entropy/format, not simulated ciphertext or durability.
2. Refusal is safe and persistent. A fresh protected outbox containing a valid
   eligible batch must remain closed and offer nothing while the invalid barrier
   exists. Preserve exact fresh payload/barrier bytes and create no payload
   quarantine. Same-instance retry remains refused. Removing only the owned
   invalid sidecar may recover the original eligible batch without mutation of
   its identity/hash or retention anchor.
3. Register all cases in normal execution and preserve every existing assertion.
   Isolate fixture names and clean exact owned sidecars. Report passing coverage
   honestly; only permit a conditional implementation edit if an actual failing
   assertion first demonstrates a production defect. Preserve the existing aged
   payload cleanup and valid-barrier tests.

## Verification and recovery

```bash
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake --build build/mingw --target nstu_exam_telemetry_outbox_tests -j 4
ctest --test-dir build/mingw --output-on-failure --timeout 60 -R '^nstu[.]exam_telemetry_outbox$'
./scripts/verify-gate.sh
./scripts/verify-gate.sh --strict
```

Use real commands unpiped with immutable source-bound receipts and the existing
DPAPI fixture helpers. Keep a private local operation result with preconditions,
test count, actual exits and hashes. Root owns fresh independent review, explicit
staging, publication and exact feature CI. No private original roadmap or
unrelated README/About edits enter the candidate tree. On interruption, resume
the live owner/command rather than creating another writer or resetting work.
