# CAPTURE-QUESTION-ID-1 — existing question-ID compatibility

## Objective and exact scope

Native browser telemetry rejects IDs longer than 64 bytes, while native manifest
validation accepts 96 decoded bytes and the existing shared answer/telemetry
codec limit is 128 bytes. Replace only the parser's hard-coded comparison with
`nstu::exam::kMaximumQuestionIdBytes`, already available through the existing
include. Preserve its error, allowlist, revision, UTF-8 and generation checks.

Product files: `client/src/exam_host.cpp` and
`tests/exam_host_protocol_tests.cpp`. No new constant, helper, public API, CMake
registration, schema, browser, answer code, shared codec or format change.
Dependency: completed clock/aggregation repair and current source baseline
`a871850`. The planning subagent inspected actual parser/manifest/codec callers;
Root saves this contract before assigning its sole coder.

## Acceptance groups

1. Admit existing valid IDs without truncation. Native package fixtures accept
   schema-valid ASCII IDs of65/96 bytes and reject97. Parse listed65/96 IDs,
   preserve fields/revision and round-trip through existing IPC serialization.
   A synthetic128-byte parser boundary is supported by the shared contract,
   not a claim that128 bytes is valid manifest input.
2. Preserve bounded validation. Reject listed129-byte IDs and multibyte UTF-8
   exceeding128 bytes despite fewer characters; reject malformed UTF-8, escaped
   NUL, unlisted valid long IDs, zero/overflowing revision and empty-ID/nonzero
   revision. Keep existing assertions and pure-parser empty-allowlist behavior.
3. Preserve live binding. Use actual ExamHostTelemetryState with injected
   deterministic steady time and IDs. Publish/start the clock, then accept
   listed65/96 samples. Browser-claimed native source becomes browser source;
   omitted/zero/stale IDs fail live forwarding. Reload42->43 rejects42 and
   accepts43 only after its valid startup.

## Tests first and minimal implementation

Add three focused functions for manifest ID bounds, parser ID bounds and live
binding in the existing native test executable. Record both65/96 parser outcomes
against original source before positive assertions, proving that failures come
from the existing ID check. Start the live clock successfully before testing IDs.
Then change the one comparison, run focused tests and the full gates. Do not
silently remove or disable any old assertion; if an old oversized fixture needs
the updated boundary, adapt its input and retain the rejection assertion with
new positive boundary coverage.

Native JsonReader already performs strict Windows UTF-8 conversion; shared
telemetry independently rejects invalid UTF-8/NUL and uses the128-byte limit.
Answer synchronization's text validation is length/NUL-based, not strict UTF-8;
do not claim otherwise or change answer behavior within this repair.

## Verification and recovery

```bash
export PATH=/c/msys64/ucrt64/bin:$PATH
cmake --build build/mingw --target nstu_exam_host_protocol_tests -j 4
ctest --test-dir build/mingw --output-on-failure --timeout 60 -R '^nstu[.]exam_host_protocol$'
cmake --build build/mingw --target nstu_exam_host_protocol_tests nstu_exam_telemetry_tests nstu_exam_sync_tests -j 4
ctest --test-dir build/mingw --output-on-failure --timeout 60 -R '^nstu[.](exam_host_protocol|exam_telemetry|exam_sync)$'
node --check exam/web/app.js
node tests/exam_web_recovery_tests.js
./scripts/verify-gate.sh
./scripts/verify-gate.sh --strict
```

Use immutable operation/command receipts and source hashes. Root owns the
exclusive writer claim, independent final review, staging/publication and exact
feature-SHA CI. Preserve the private original roadmap and unrelated README/About
edits. Main remains gated by unfinished acceptance, server durability and runtime
qualification; this narrow parser repair cannot clear those requirements.
