# STORAGE-RECOVERY-1 — missing durable recovery boundaries

The planning subagent inspected existing implementation and tests. No new
production defect is proven yet. Split the proposed matrix into independently
verifiable slices; do not write the entire matrix in one coding round.

| Slice | Responsibility | Primary scope |
|---|---|---|
| STORAGE-RECOVERY-1A | Blocked coordinator cleanup retries, including owned outbox payload temporaries. | Service tests; service implementation only for a proven failure. |
| STORAGE-RECOVERY-1B | Real protected expiry barriers with invalid lengths, zero stamp and tampering. | Outbox tests; outbox implementation only for a proven failure. |
| STORAGE-RECOVERY-1C | Both cleanup failure stages, safe mutations and path ownership. | Outbox tests; conditional outbox implementation. |
| STORAGE-RECOVERY-1D | Pristine interrupted coordinator initialization. | Service tests; conditional service implementation. |

Preserve existing coverage and assertions. Use real machine protection, production
atomic writes, Windows sharing locks and deterministic local time, not sleeps or
simulated durability. Each slice gets a frozen plan and source/operation baseline
before its one writer starts. The immediate scoped plan is
[storage-recovery-1a.md](storage-recovery-1a.md).

Barrier tests use protected seven/nine-byte plaintext, protected eight-byte zero,
and corrupted valid ciphertext. Verify preconditions with actual protection;
refuse offers, preserve fresh payload/barrier and avoid payload quarantine.
Existing raw malformed-barrier/31-day cleanup tests stay intact and are not
duplicated.

For cleanup mutations, distinguish payload-replacement failure from barrier
deletion failure after successful cleaned payload publication. Offers/enqueue
and path switches must not bypass pending expiry, but matching durable ACK
retirement and successful purge/clear may remain safe under their current
contracts. Do not require blanket mutation failure merely to make tests green.
Mixed retained/expired/loss fixtures must verify exact identities, bytes, loss
counts, rollback/restart and one-time committed expiry accounting.

For pristine bootstrap, derive protected fixtures from a real unbound coordinator
publication, including final state without outbox and a numeric state temporary
without final stores. Require disabled/unbound recovery, zero producer/generation,
sequence one, no pre-enable collection and first enabled batch sequence one.

No answer, wire format, public API, CMake, quota, retention or authorization-policy
changes. These repairs are independent of later Task12 acceptance but do not
tick historical acceptance or establish main/runtime readiness.
