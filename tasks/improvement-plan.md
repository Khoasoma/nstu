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

These fixes have regression coverage. The review report is
[review-2026-10-27.md](review-2026-10-27.md).

## Next review repair units

| Order | Finding and scope | Required proof |
|---|---|---|
| 1 | **P2: collector identity reuse.** `client/src/exam_host.cpp`, its header and host tests. Allocate native identities across restarts of the same exam; preserve exact identity binding and JavaScript integer precision. | A same-exam restart/reload gets a different collector; stale samples from the previous generation are refused. Keep collection disabled if identity allocation fails. |
| 2 | **P2: clock origin mismatch.** Native host, browser collector and their tests. Establish a documented mapping between document-generation elapsed time and native elapsed time. | Slow document loading, late enable and reload do not merge timestamps with different origins; unknown intervals remain unknown. |
| 3 | **P2: unbounded browser summaries.** `exam/web/app.js` and browser tests. Coalesce normalized answer changes per question before flushing and bound pending observations. | A sustained edit flood uses bounded memory and packets, preserves observed change totals and navigation/dwell, records bounded loss, and contains no answer/notes/clipboard text. |
| 4 | Storage hardening tests for protected zero/wrong-length expiry barriers, blocked barrier deletion, ACK/enqueue/purge/clear during cleanup failure and interrupted pristine initialization. | All mutation paths refuse unsafe recovery; cleanup retry preserves quotas and revocations. |

Use separate tests-first units and fresh read-only review for each repair. Preserve
the answer channel and original assertions. These repairs do not authorize
checking historical acceptance boxes.

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
