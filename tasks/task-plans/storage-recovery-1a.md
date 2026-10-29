# STORAGE-RECOVERY-1A — blocked payload cleanup retry

## Objective and exact scope

Prove that a blocked coordinator restart scrubs every owned behavioral payload
store and temporary once deletion becomes possible, while retaining its durable
refusal marker. Current blocked startup/maintenance cleans coordinator temporaries
but may never reach outbox temporary cleanup; that is a candidate defect, not an
assumed implementation failure.

Primary file: `tests/exam_telemetry_service_tests.cpp`.
Conditional implementation file: `client/src/exam_telemetry_service.cpp`, only
after the new regression fails against current production code. Maximum two
product/test files. Do not change outbox implementation, formats, public APIs,
CMake, answers, quotas, expiry, authorization or revocation policy.

## Acceptance groups

1. Produce the blocked state through actual coordinator operations: first one
   durable batch, then a pending intent while replacement is locked, then the
   real stop fallback while state/outbox replacement is blocked. Destroy the
   instance and restart; it must remain unauthorized for old and new enables,
   collection and offers. Do not fabricate authorization or protection success.
2. Include protected payload copies in owned numeric `.coord_state.tmp-N`,
   `.coord_state.blocked.tmp-N` and outbox `.tmp-N` paths. Exercise sequential
   deletion locks on coordinator temporary, an owned outbox temporary itself,
   the outbox store and coordinator store. The outbox temp must remain while its
   deletion is locked and disappear after release/retry; ignored deletion errors
   cannot count as cleanup success.
   Refusal/cleanup failures preserve the obligation; releasing locks and retrying
   removes both final behavioral stores and all owned temporaries. Keep `.blocked`
   and the existing nonnumeric coordinator-temporary sentinel intact.
3. Register the regression in default test execution, and preserve all original
   assertions and restart/purge/expiry coverage. A narrowly necessary cleanup
   change must operate only on exact owning prefixes/validated numeric names,
   avoid deleting unrelated files, and never permit a bind or replay after cleanup.

## Tests first and implementation

Use existing Windows file-sharing lock and protected fixture helpers. Derive
copies from actual state/outbox files containing dummy test samples, including
blocked.tmp-N named copies. Do not copy the one-byte .blocked marker as behavioral
fixture data. Successful maintenance still returnsfalse and remains unauthorized.
Avoid following reparse targets or recursively deleting directories. Isolate
fixture names and remove only owned sidecars so a failed run cannot contaminate
retry results. Capture the original runtime failure for the outbox `.tmp-N`
retention case before modifying service code; other already-passing rows must
not be claimed as RED.

Then implement only the missing owned-temporary cleanup in blocked coordinator
startup/maintenance if the regression proves it is missing. Deletion-locked
files cause a visible cleanup refusal and remain eligible for later retry.
Preserve the durable marker, error reporting and closed/unauthorized lifecycle.
Retain the existing nonnumeric sentinel and add an unrelated prefix sentinel
if necessary to prove scoped cleanup.

Root owns the exclusive writer claim, operation receipts, fresh independent
review and Git publication. On disconnect, inspect live agent/session/process
receipts before resuming; never create a duplicate writer or reset partial work.

## Verification

```bash
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake --build build/mingw --target nstu_exam_telemetry_service_tests -j 4
ctest --test-dir build/mingw --output-on-failure --timeout 60 -R '^nstu[.]exam_telemetry_service$'
ctest --test-dir build/mingw --output-on-failure --timeout 60 -R '^nstu[.]exam_telemetry_(outbox|service)$'
./scripts/verify-gate.sh
./scripts/verify-gate.sh --strict
```

Run actual commands unpiped with unique source-bound receipts. Report exact
exits and logs; protected fixtures and Windows locks must succeed before using
their results. Fresh review and exact-revision MSVC CI follow completed repairs.
Private original roadmap, unrelated README/About edits and historical acceptance
remain untouched; this slice does not make the integration ready for main.
