# STORAGE-RECOVERY-1C — cleanup mutations and path ownership

The read-only storage planner inspected the actual outbox and existing tests.
Split its mixed recovery matrix into three reviewed increments rather than one
large test change. No production defect has been demonstrated.

| Unit | Scope | Dependency |
|---|---|---|
| STORAGE-RECOVERY-1C-1 | Mixed expired/retained/loss state at payload replacement and barrier deletion failure; alternate-path noninterference. | Completed 1B writer/review and frozen sources. |
| STORAGE-RECOVERY-1C-2 | Exact accepted/duplicate ACK retirement after cleaned payload publication while barrier deletion is blocked. | 1C-1 verified fixtures. |
| STORAGE-RECOVERY-1C-3 | Purge/clear under both cleanup stages, revocation fallback and old-path ownership. | 1C-1 verified fixtures; preserve existing generic purge tests. |

The immediate scoped plan is [storage-recovery-1c-1.md](storage-recovery-1c-1.md).
Each later unit receives its own frozen plan before coding. Primary product scope
is the outbox test file; implementation changes require a real failing regression
and a recorded scoped repair. Preserve all assertions and answer/wire/storage
contracts. Supporting reviews do not close historical acceptance or VM gates.

The two cleanup failures have different durable outcomes. A failed payload
replacement restores the old records/loss/bytes. A failed barrier deletion after
successful replacement leaves the cleaned payload committed. Public prune may
return zero with an error in either case; tests must inspect actual durable state.
Offers, enqueue and path switches remain gated. Exact durable ACK retirement and
successful purge/clear can safely remove records under their existing contracts.
