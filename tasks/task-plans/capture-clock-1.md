# CAPTURE-CLOCK-1 — generation-relative browser timing

## Objective and current gap

Map browser observations to the native collector generation without pretending
asynchronous message delivery is exact clock synchronization. Browser samples
currently use absolute rounded `performance.now()`, while native samples start
at document navigation completion. Slow loading and late enable therefore give
different origins to observations sharing one collector ID.

Base: `6709d33`; read `tasks/plan.md` and current capture source before execution.
This is a corrective unit preceding bounded browser aggregation, not acceptance
of the unfinished Phase 4 integration.

## Acceptance groups

1. Slow load, late enable and reload produce generation-relative elapsed values;
   zero baselines are valid, duplicate enable is idempotent and stale generations
   cannot establish the mapping.
2. Initial delivery uncertainty is explicit and native-owned. Missing/malformed
   anchors or unavailable/regressing clocks refuse unsafe browser collection;
   no inferred dwell or cross-source ordering crosses unknown time.
3. Existing answer/recovery/finalize behavior, codec bytes, service sequencing,
   privacy and queue priority are preserved. A broken telemetry clock cannot
   stop an otherwise valid exam.

## File allowlist and ownership

- `client/include/nstu/exam_host.hpp`
- `client/src/exam_host.cpp`
- `exam/web/app.js`
- `tests/exam_host_protocol_tests.cpp`
- `tests/exam_web_recovery_tests.js`

One scoped subagent is the product writer. Root owns plans/recovery state and
Git operations. A fresh independent reviewer must not have authored this diff.
No common codec, service/server, IPC framing, answer ledger or CMake changes.

## Contract

- Use one native `steady_clock` generation origin `G`. Change only telemetry
  timing; existing unrelated rate/liveness timers need not move. Native samples,
  enable anchor and receipt measurement must share this origin and clock.
- Immediately before the first successful matching WebView enable post, sample
  native elapsed `A = floor_ms(S-G)` and put `collectorElapsedMilliseconds: A`
  in local enable JSON. This is not a new TCP/IPC/wire field.
- Browser accepts only a finite nonnegative safe-integer anchor and an available
  finite nonnegative raw performance clock. Capture raw receipt baseline `P`.
  Event elapsed is `A + round(performance.now() - P)` with checked nonnegative
  safe arithmetic. Calculate the difference before rounding. Dwell and clock
  discontinuity use raw browser intervals, not an external wall clock fallback.
- Browser immediately emits `collector_started` at `A`. Native binds the first
  valid startup to the pending generation/anchor before ordinary browser data
  can establish a trusted mapping. Live forwarding must inspect an explicitly
  present, safe, matching nonzero raw collectorId before native identity rebinding.
  Enforce this on every live browser packet, including startup; an omitted/zero
  ID cannot bind a newly reloaded generation at a repeated zero anchor. Existing
  standalone pure-parser omission behavior may remain for compatibility tests;
  it must not be used as live generation authorization. On receipt at native elapsed `R`, overwrite
  startup `numeric_value` with `R-A+2` milliseconds: a conservative initial
  calibration uncertainty radius including the delivery/receipt interval and
  two milliseconds of signed quantization slack. Reject backwards
  receipt/arithmetic overflow; do not clamp it into apparently trusted time.
- Browser elapsed values are mapped estimates, not literal lower endpoints.
  At initial calibration, allow signed slack around the estimate: an initial
  uncertainty radius U represents [max(0, estimate-U), estimate+U]. Cross-source
  order inside that interval is unknown. This radius concerns initial delivery
  and quantization, not a bound on subsequent drift or browser clock coarsening. A discontinuity ends the
  known segment; no dwell or timing inference bridges it. Do not claim exact
  synchronization or exact real-WebView behavior from a fake-clock unit test.
- Keep pending anchor state generation-bound. Clear it on stop/reload/failure.
  Post failure cannot admit ordinary browser samples or activate mapping. A
  duplicate matching enable preserves its original origin/baseline/startup.
- Missing/invalid anchor, unsafe timestamp addition, clock absence or regression
  disables browser collection while answers remain usable. Emit bounded typed
  discontinuity/loss where valid measurement remains; never substitute Date.now
  or fabricate duration to keep collection apparently working.

Microsoft documents GetTickCount64 as system-timer-resolution timing, typically
10–16 ms, and MSVC steady_clock as QueryPerformanceCounter-based. The native
telemetry clock moves to steady_clock to avoid presenting timer granularity as
an exact small delivery bound. Runtime drift/suspend behavior remains a real
WebView2 qualification item.

## Tests first and implementation slices

1. Add deterministic browser slow-load/late-enable/reload/zero-clock regressions
   and native anchor serialization/state tests; preserve actual failing output.
2. Add a small testable native serializer/state seam and one posting helper used
   by both navigation completion and late enable. Preserve existing source/tuple
   validation and WebView ready checks.
3. Validate browser anchors, map elapsed values and handle zero/invalid/raw-clock
   history. Existing harness enables may supply anchor zero explicitly while
   retaining their assertions; add missing-anchor refusal tests separately.
4. Bind startup and native uncertainty before forwarding. Test failed post,
   ordinary-before-startup, wrong collector, stale startup, backwards receipt,
   maximum-safe/overflow boundaries and browser-claimed uncertainty overwrite.
   Add missing/zero raw startup IDs after reload; neither binds the new clock.
   Fractional regression: A=0, P=12000.25, next raw time=12000.85 maps to 1;
   signed uncertainty must still include native floored elapsed 0.
5. Compare non-telemetry messages/storage for the same recovery/finalize scenario
   with collection off and anchored collection on; retain privacy canaries.

Native deterministic example: send elapsed 40, startup receipt 65, forwarded
startup elapsed 40 and uncertainty 27. Browser example: raw performance 12000,
anchor 0, advance 3000; startup elapsed 0 and later elapsed/dwell 3000.

## Verification and recovery

## Independent review corrections — before repair

Two inspected cases must pass before this unit is complete:

- Direct submit or authenticated disable must check wall/performance
  discontinuity before closing dwell. Test a 60-second wall jump with one
  second of raw performance progress, followed immediately by each closure:
  emit discontinuity and omit the unknown dwell, while answer/finalize works.
- A prior document can consume the 256-sample window, causing the new document's
  one-time startup to be dropped. Recovery must be bounded without resetting or
  bypassing the quota. Use a limited startup retry through the existing local
  enable message and native UI maintenance: retain the original anchor and raw
  browser baseline, retry at most once per second with a finite attempt budget,
  and schedule no new retry request or trusted startup forwarding for a mapping
  that already succeeded. Already queued requests may receive duplicate local
  replies; native drops those as startup duplicates. Retry replies must contain
  the stored startup alone, not pending ordinary data, so duplicate rejection
  cannot discard unrelated observations. A retry request
  is local WebView metadata, not a new TCP/IPC command. Validate its exact
  collector and anchor. Test full-window drop, recovery after window reset,
  retry throttling/budget, mapped duplicate idempotence and unchanged quota.
  Quota ownership may move into the shared production forwarding seam for direct
  state integration tests; retain the 256/s limit and its window across reload.

The same five-file allowlist applies. Write failing regressions before repairing
these paths; preserve the original result and review evidence. Root checkpoints
the amended plan and source hashes before the sole coder continues.

## Verification and recovery commands

Use Git Bash with UCRT64 first. Run Node syntax and browser suite, build affected
host tests, focused CTest, then unpiped fast and strict gates. Record each exit,
stdout/stderr, exact hashes and warning/test counts in the task operation receipt.
Fresh independent review follows fixes. Feature publication and exact SHA MSVC
CI follow only the completed unit; main remains subject to integration acceptance.

```bash
export PATH=/c/msys64/ucrt64/bin:$PATH
node --check exam/web/app.js
node tests/exam_web_recovery_tests.js
cmake --build build/mingw --target nstu_exam_host_protocol_tests -j 4
ctest --test-dir build/mingw --output-on-failure --timeout 60 -R '^nstu[.]exam_host_protocol$'
./scripts/verify-gate.sh
./scripts/verify-gate.sh --strict
```

On interruption, inspect the saved writer claim, agent/session and source hashes
before resuming. Preserve partial changes. Do not rerun a live build or create a
second coder. If a contract issue requires wider scope, revise this plan before
editing; do not silently weaken tests or change telemetry codec/answer formats.
