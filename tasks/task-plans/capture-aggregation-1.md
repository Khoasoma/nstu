# CAPTURE-AGGREGATION-1 — bounded browser summaries

## Objective, dependency and scope

Coalesce normalized answer transitions and bound pending observations/flush work
without changing the answer channel, native receiver or telemetry codecs.
Dependency: completed CAPTURE-CLOCK-1, published in `637bb18` with passing CI.
Product allowlist is exactly `exam/web/app.js` and
`tests/exam_web_recovery_tests.js`. Save a fresh source/plan baseline and obtain
independent plan review before dispatching its sole coding subagent.

## Existing behavior and limits

Each edit currently allocates a cumulative answer-change sample, and flush drains
the entire pending array. The manifest is bounded at 500 questions; packets are
1–32 samples and at most 32 KiB. Native forwarding has a 256-sample one-second
window and rejects elapsed regression. Periodic flush remains 30 seconds.
Startup and bounded retries are stored standalone packets with the original
anchor/baseline. No native acceptance ACK exists for browser posts.

The existing manifest question-ID allowance of 96 bytes versus the native
telemetry parser's 64-byte allowance is a separate known compatibility gap.
Answer synchronization permits 128 bytes. Do not enlarge native/codec acceptance
or change answer manifests within this two-file unit; keep that gap visible for
an independent admission rather than claiming unsupported IDs are accepted.

## Proposed internal bounds — frozen before implementation

- At most 500 pending per-question answer summaries.
- At most 1000 retained first-view/first-answer slots, one of each per question.
- At most 256 ordinary detail records for dwell/focus/visibility/discontinuity.
- Two fixed loss accumulators, one terminal record and one stored startup.
  Total pending serialized sample data is at most 2 MiB.
- Each ordinary flush sends at most four packets; at most four ordinary packets
  in a rolling 1001 raw milliseconds, each still at most 32 samples/32 KiB.
- At most one additional terminal packet per generation, at most 32 samples,
  for submission/disable/clock failure. This finite exception is part of the
  bound and cannot drain an unlimited tail.

These are browser-internal limits, not claims of native acceptance. Asynchronous
delivery bunching may still invoke native quota rejection; successful local
posting is not a durable delivery or acceptance result.

## Acceptance groups

1. Coalescing preserves observed transition totals and retained chronology.
   Keep `count` as the cumulative observed per-question transition count; use
   the previously unused `numericValue` as the delta represented by a summary.
   Two transitions/flush/three transitions yields `(2,2)` then `(5,3)`. Identical
   input and unfinished IME composition add nothing. Preserve first observations
   and retained dwell, and send a stable chronological prefix so later packets
   never regress elapsed time. A mutable summary takes its latest change time.
2. Memory, packet work and rate stay bounded with explicit loss. Coalescing
   itself is lossless. Capacity shedding uses `loss_queue_full`; terminal
   truncation caused by exhausted sending capacity uses `loss_rate_limit`.
   Count omitted normalized transitions individually. Saturating counts are
   explicitly marked lower bounds, never silent clamps. Failed ordinary posts
   consume budget, stop that flush at the first failure and retain unsent
   observations/accounting. Later attempts reselect the globally chronological
   prefix after any mutable summary updates; never pin obsolete queue positions.
3. Clock, startup retry, privacy and answers are preserved. Reset cancels deferred
   timers and clears generation-owned aggregates. Startup/retry packets remain
   standalone and retain anchor/baseline. Payload contains only existing typed
   fields, never answer/notes/clipboard text. Enabled/disabled/overloaded/
   clock-failed runs yield identical non-telemetry messages, answer storage,
   recovery and finalize results.

## Implementation steps and tests first

1. Add deterministic behavior regressions and preserve actual failing output.
2. Replace per-edit sample allocation with bounded per-question accumulators,
   keeping existing answer normalization and first-answer semantics.
3. Add bounded detail admission, fixed loss accounting, serialized-byte checks
   and chronological selection. Do not use a second general event framework.
4. Replace unbounded drain with packet/rate limits and one coalesced deferred
   timer. Keep startup separate and implement the explicitly bounded terminal
   path. Preserve pending ordinary data when a startup duplicate is rejected.
5. Run Node syntax/suite, relevant native forwarding coverage and both unpiped
   local gates; fresh independent review precedes publication and exact SHA CI.

Required regressions:

- Alternate two values 20,000 times at fixed raw time: one pending summary,
  totals/delta 20,000, one first-answer; navigation retains correct view/dwell.
- Two flush intervals yield cumulative/delta `(2,2)` and `(5,3)`; identical input,
  IME interim input, clear/reselect and restored answers preserve semantics.
- Interleaved question changes, views and dwell produce globally nondecreasing
  elapsed values across packets, even when an older mutable summary is updated.
- Flood details beyond capacity, inspect actual bounded state through the test
  VM's instrumentation, and verify loss totals, sizes and the four-packet budget.
  Advance raw time to 1001 ms and drain deterministically.
- Exhaust capacity before submission/disable/clock failure: one finite terminal
  packet, explicit omitted-tail loss, usable answer finalization.
- Startup retry while data is pending preserves stored startup and ordinary
  observations. Repeat exact recovery/finalize equivalence under pressure and
  post failure with privacy canaries.

## Frozen review corrections before coding

- Keep answer-summary `numericValue` exclusively the represented transition
  delta. On this non-dwell kind only, use the previously unused
  `durationMilliseconds` as a defined two-bit summary flag mask: bit0 means
  cumulative `count` overflow/lower-bound; bit1 means delta overflow/lower-bound.
  Exact UINT32_MAX without overflow has a zero flag. On loss_summary only,
  durationMilliseconds bit0 marks a saturated/lower-bound loss count; numericValue
  remains zero. Other sample kinds retain their existing duration semantics.
  Cumulative, pending delta and loss saturate independently; all numeric fields
  remain uint32-compatible. Freeze and document these per-kind meanings before
  implementation; no codec or field-layout change is allowed.
- Explicit disable uses the existing clock_discontinuity kind with reason
  discontinuity_suspend as its terminal record: the collection segment becomes
  unobserved at the last valid elapsed time. This does not claim a wall-clock
  fault or cheating. Submission and clock failure retain their existing typed
  terminal records. No new sample kind, reason enum or codec field is introduced.
  If collection already ended through clock failure, later disable does not
  create a second terminal attempt for that generation.
- A terminal packet reserves up to three slots for its terminal record and the
  two nonzero loss summaries before choosing an earliest chronological prefix
  of retained observations. Calculate omitted-tail loss first: an omitted answer
  summary counts its pending delta, not its cumulative count or merely one record.
  An omitted non-summary observation counts one. Preserve saturation/lower-bound
  flags when exact loss is no longer representable. Emit retained observations
  in order, then loss/terminal records at the last valid elapsed time.
- Stop an ordinary flush at its first failed post, consume that attempted packet
  budget, and retain unsent observations/accounting without sending its remaining suffix
  in that flush. Every later ordinary attempt reselects the global chronological
  prefix from current bounded pending records, including updated summary times;
  it does not pin an obsolete prefix position. Retry only in ordinary capacity. Define the finite terminal exception as one attempt per
  generation, including failure. Once attempted, permanently retire its omitted
  older observations as declared loss; never replay them after its newer time.
  A failed terminal remains bounded local unsent accounting, not a claim that
  native received the loss or terminal record. Answers/finalization continue.

Additional required tests: seed counts/deltas/losses near UINT32_MAX to distinguish
exact maxima from independent overflow; overload terminal selection with a
multi-transition summary and >32 records and verify reserved slots/loss units;
fail an early ordinary packet to prove no suffix overtakes it; fail each terminal
path to prove one attempt, bounded unsent accounting, no older replay and unchanged
answer/finalize results. Also test summary@10 post failure, detail@20, a new
transition updating the same summary to30, then successful retry: output is
nondecreasing, both transitions represented exactly once, and capacity unchanged.

## Code-review rate repair before publication

Ordinary packet capacity must use a current valid raw clock for each individual
post attempt, including pruning the rolling window and recording the attempt.
Do not stamp all packets with the single pre-flush clock: posting can advance
raw time, making old entries expire too early. Stop ordinary posting if the raw
clock becomes unavailable/regresses; no frozen known-clock snapshot may grant
ordinary capacity after that failure. Preserve the one finite terminal attempt
and last measurable mapped time, and prevent recursive ordinary draining while
handling a clock failure during flush.

Required RED: 256 pending details, first four post calls advance raw time by1ms,
then flush at2001. Actual attempts1000/1001/1002/1003 must not all expire at2001;
never exceed four ordinary attempts in any rolling1001ms. Add unavailable and
regressing raw-clock probes during posting as needed to verify bounded terminal
handling and unchanged answer/finalize behavior.

## Commands and interruption recovery

```bash
export PATH=/c/msys64/ucrt64/bin:$PATH
node --check exam/web/app.js
node tests/exam_web_recovery_tests.js
ctest --test-dir build/mingw --output-on-failure --timeout 60 -R '^nstu[.]exam_host_protocol$'
./scripts/verify-gate.sh
./scripts/verify-gate.sh --strict
```

Root creates the operation intent, snapshots, exclusive writer claim and receipt
runner before coding. The subagent records command sessions and progress locally;
on disconnect, resume the live worker or reconcile recorded results before retry.
Never reset or discard partial work, lower assertions or publish the private
original roadmap. This task's completion does not accept unfinished server
journaling/ACK, peer or real-runtime qualification for main integration.
