# Telemetry follow-up plan — October 27, 2026

`todo.md` remains the task and acceptance authority. This plan records the current
code review and concrete next work. Completed local fixes do not complete the
telemetry feature or substitute for its required independent acceptance.

## Completed in this review

- Persist a protected expiry barrier before outbox cleanup so a failed payload
  replacement and later clock rollback cannot revive an observed expired batch.
- Clean coordinator crash temporaries at startup and maintenance. On blocked
  restart, scrub both payload stores while retaining the durable refusal marker.
- Publish initial coordinator state before creating the outbox. Refuse corrupt
  state and orphan batches rather than infer a new producer.
- Keep agent telemetry in a separate bounded queue; protected messages drain
  first and IPC retries preserve their unsent order.
- Baseline recovered answers without manufacturing edits, start dwell after an
  authenticated begin, and resume a known dwell segment after discontinuity.
- Reject malformed native telemetry numeric/string fields and unchecked narrowing.
- Reject a deadline send whose final write finishes at the absolute deadline.
- Map browser observations to a native steady-clock generation anchor with
  explicit initial uncertainty, strict live collector binding and bounded
  startup retry. Direct closure handles clock discontinuity without unknown dwell.
- Coalesce per-question browser changes with cumulative/delta counts, bounded
  detail/byte/packet buffers, explicit loss and finite terminal attempts. Refresh
  raw time per packet and preserve chronology on failed-post retry.
- Allocate exact native collector IDs from a random process origin and shared
  monotonic counter; host restart and document reload cannot reuse a live
  process ID. Unavailable allocation disables collection without blocking exams.

These fixes have regression coverage. The review report is
[review-2026-10-27.md](review-2026-10-27.md).

## Next review repair units

| Order | Finding and scope | Required proof |
|---|---|---|
| 1 | Storage hardening tests for protected zero/wrong-length expiry barriers, blocked barrier deletion, ACK/enqueue/purge/clear during cleanup failure and interrupted pristine initialization. | All mutation paths refuse unsafe recovery; cleanup retry preserves quotas and revocations. |

Use separate tests-first units and fresh read-only review for each repair. Preserve
the answer channel and original assertions. These repairs do not authorize
checking historical acceptance boxes.

Native question-ID compatibility is now repaired with the existing shared
128-byte limit; manifest admission remains96. Valid65/96-byte parser/live inputs
round-trip without truncation, with original UTF-8/NUL/allowlist/generation checks.

STORAGE-RECOVERY-1A now scrubs owned numeric outbox/coordinator temporaries on
blocked restart and preserves deletion-failure retry/refusal. Focused lock rows
4/4, service/outbox2/2, Root fast/strict49/49 and fresh review passed. Remaining
barrier, mutation and pristine-bootstrap slices are planned separately in
[storage-recovery-1.md](task-plans/storage-recovery-1.md).

STORAGE-RECOVERY-1B is published as `0b535d1`: four real protected malformed/tampered
barrier cases passed, preserving payload bytes, retry refusal, exact recovery and
the original retention anchor. Fresh supporting review and independent Root
fast/strict gates passed 49/49 with zero warnings; exact-revision Windows CI is
tracked in local recovery evidence. No production defect or RED was fabricated.
The next matrix is split in [storage-recovery-1c.md](task-plans/storage-recovery-1c.md),
starting with [mixed cleanup and path isolation](task-plans/storage-recovery-1c-1.md).

STORAGE-RECOVERY-1C-1 is published as `7ad7960`. Both real lock-stage cases passed,
including exact mixed durable state, rollback/restart and alternate-path isolation.
Fresh supporting review, independent Root fast/strict 49/49 with zero warnings
and all three exact-revision Windows CI jobs passed. Production code is unchanged.
The next scoped unit is
[exact ACK retirement under a blocked barrier](task-plans/storage-recovery-1c-2.md).

STORAGE-RECOVERY-1C-2 is published as `049a11e`: both exact accepted/duplicate ACK
cases passed, preserving R2 and full loss/retention state while gates stayed
closed until barrier cleanup. Fresh supporting review, independent Root
fast/strict 49/49 with zero warnings and all three exact-revision Windows CI jobs
passed. Production code is unchanged. Removal is split into
[1C-3A success](task-plans/storage-recovery-1c-3a.md) and a following 1C-3B mixed
replacement-failure/revocation unit before pristine initialization.

## Resume the existing checklist

1. Obtain the required fresh independent reviews for Tasks 5–11 on the current
   source hashes. The October 27 Opus route probe timed out after 45 seconds;
   it produced no verdict. Codex review is supporting evidence only.
2. Establish Checkpoint B with the real old/new peer matrix and production
   disconnect/suspend, explicit stop/revoke, shutdown and restart evidence.
3. **Task 12:** prove higher-priority progress, one telemetry offer per second,
   one per loop, receive/wait after an attempt and a 500 ms whole-frame bound.
   Replace the misleading 1 MiB socket fixture with a valid bounded frame and
   synchronized partial progress; the current 64 KiB command limit rejects that
   fixture before socket I/O. Test inbound stop/status/answers and durable retry
   after matching reconnect, not merely a preordered FIFO.
4. **Task 13:** implement the independent journal, rotation, bounded dedup,
   producer admission/generation cap, 30-day expiry and non-resurrecting floor.
5. **Task 14:** integrate bounded asynchronous persistence and durable ACKs
   outside control locks; test full/stalled workers and authorization.
6. **Tasks 15–16:** prove answer equivalence/privacy and integrate browser checks
   into the local gate, then update truthful product documentation.
7. **Task 17:** qualify real WebView2 and isolated VM behavior with redacted
   evidence. MSVC Release `/W4 /WX` CI must pass for the pushed revision.

Task dependencies and acceptance evidence remain required. Server ingest,
journaling and ACK persistence are unfinished; collection and client storage
alone are not end-to-end telemetry delivery. Current network lockdown still
does not establish exam safety.
