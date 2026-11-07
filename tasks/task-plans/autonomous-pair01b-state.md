# PAIR01B — bounded confirmation state and production pairing seam

Status: DESIGN_PLAN_READY; current predecessor hashes prepared in `autonomous-pair01-phase-results.md`, awaiting A CODE_ACCEPT and phaseB admission. Current HEAD `edb79e872339e372de9ee662ed71e9a0cacef154` plus exact A four-file delta listed there. Owner/build owner: delivery_pairing. Initial HEAD/worktree/invariants: `autonomous-pair01-phases.md`. Dependency: A independent acceptance and exact predecessor receipt; separate PLAN_ACCEPT for this phase before product edits. Reviewer corrections or phase commit require revalidation before admission.

## Exact allowlist (5 product files)

| Path | Initial/predecessor binding | Owned changes |
|---|---|---|
| `client/include/nstu/service_pairing_confirmation.hpp` | ABSENT at initial HEAD | One-attempt bounded broker, validated connection identity, production pairing wrapper declaration. |
| `client/include/nstu/agent_pairing_confirmation.hpp` | ABSENT at initial HEAD | Production agent prompt/action state, presentation gate, explicit typed-code decision. |
| `client/src/service_pairing_confirmation.cpp` | ABSENT at initial HEAD | Both models plus wrapper implementation. One owned source entry. |
| `client/CMakeLists.txt` | `0F04741B9C7458E5548DC033B4D15C0BB59A72BBBDB6347E5D9D49E6A61DBD7E` | Only `src/service_pairing_confirmation.cpp` in initial `nstu_client_platform` source list, as root granted. |
| `tests/service_pairing_confirmation_tests.cpp` | Current A predecessor SHA256 `25E37CD151A7721304E25514121EAB8179F8C8721CB55CAD274461F5D3A3589E` | Append state and production wrapper regressions; retain A assertions. |

Documentation writes: this plan and `autonomous-pair01-phase-results.md` only. No service/agent main or teacher changes yet.

## Design

One service broker owns current random token, nonempty expected SAS, maximum 180-second deadline capped by configured timeout, validated WTS session and named-pipe connection generation, one terminal decision, mutex/CV. Connection change/cancel invalidates and wakes before replacement. Dispatch replies only for matching token/connection identity; mismatching same-attempt SAS, unpresented confirmation, refusal, malformed reply, timeout and no UI deny. Old token/duplicate decisions never authorize. Callback checks stop and deadline before/after each at-most-250-ms slice; returns waiting while live. Never hold state locks during outbound queue/pipe/network operations. End and stale-message filtering bind to token; late end cannot close new prompt. Fresh token uses existing `security::generate_random`; random failure refuses.

`pair_with_interactive_agent` is the production wiring seam: it constructs real `PairingAttemptOptions.student_confirmation` and display observer, calls unchanged `pair_with_server`, invalidates/queues end on every return, and hands back enrolled config only through that existing crypto function. It does not save keys. Agent model receives request without declaring it shown; only actual UI-thread presentation acknowledgment unlocks decisions. Input is empty until student types six digits from the teacher screen. Explicit Codes match + matching typed code + presented/live token produces one confirmed reply; mismatch/refuse/close/Escape/no UI/expiry/disconnect produce refusal. No code echoed automatically.

## Acceptance (3 groups)

1. Model tests exercise slice bound, unavailable/UI-failure/mismatch/refusal/expiry/cancel, stale/duplicate/superseded token, peer generation replacement and old end vs new request. Decisions remain fail closed.
2. Production-wrapper test uses real client/server crypto exchange and actual broker/agent codecs/model. Before C, an injected test human can read the same server SAS through test-owned transcript fixture; this is preparatory evidence only. First behavioral RED reproduces current production empty-options invocation (real `pair_with_server` observed SAS but no student callback), expecting enrollment after explicit student action and independent server approval; it fails with missing confirmation. GREEN changes the invoked production seam to the shared wrapper, and removing its callback causes regression. No test-only auto-confirm class methods.
3. Scoped suite and existing pairing/IPC suites pass, client platform compiles. No existing production caller behavior or handshake/server approval behavior changes yet. Never claim end-to-end product complete until C/D.

Risks: helpers alone do not fix the product; UI presentation truth requires actual C/D wiring and review. Thread signaling and queue filtering must preserve unrelated traffic. Exit: independent state/wrapper review with RED/GREEN receipts; next-phase hash binding. No integration.
