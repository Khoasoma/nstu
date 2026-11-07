# PAIR01A — additive local confirmation codec

Status: PLAN_READY. Owner/build owner: delivery_pairing. Initial HEAD/worktree/verification/invariants: `autonomous-pair01-phases.md`. This phase may begin only after independent PLAN_ACCEPT bound to its current source hashes.

## Exact allowlist (4 product files)

| Path | Initial SHA256 | Owned changes |
|---|---|---|
| `client/include/nstu/agent_protocol.hpp` | `69C79C505DF69DF0C1D49001D9517135C5E0F0F3F4A506DAABFA6211F295AD2E` | Append kinds 42 request, 43 reply, 44 end; bounded payload types/codecs only. |
| `client/src/agent_protocol.cpp` | `F66F38F5A77E3E81960405A0DA110B6E7A8E1383BFB08C2B12C86E81AF42D738` | New strict payload codecs, valid type maximum only; old payload behavior unchanged. |
| `tests/service_pairing_confirmation_tests.cpp` | ABSENT | Owned codec tests, initial test runner; later phases extend this target. |
| `tests/CMakeLists.txt` | `D9D9EA805D2FCCEEEEEBC71DFFA05F5F813ECCC28AEB22D7CBA1066E66345D9F` | Only `nstu_service_pairing_confirmation_tests` / `nstu.service_pairing_confirmation`, beside existing pairing tests inside client `if(TARGET nstu_server_state)`, link existing client/server libs and existing warning/assert helpers. Root granted only this target. |

Documentation writes: this plan, revised master, superseded original status, `autonomous-pair01-phase-results.md`; no other paths.

## Design and compatibility

Preserve local frame version/name/header and every existing kind value (1–41)/payload byte. Append-only local kinds 42–44; unmatched old binaries fail closed on unknown kinds/no response, never fallback approval. No TCP changes. Request: fresh nonzero random 16-byte correlation token, exactly six ASCII SAS digits, printable server name bounded by existing 128-byte bound, absolute local Win32 monotonic expiry. Reply: token, explicit one-byte choice, 0/1 presented flag, bounded actual teacher digits (six for confirmed, empty allowed for refusal). End: exact token only. A maximum 180-second confirmation window is validated by consumers against local monotonic now; decoders validate nonzero expiry plus bounded wire size (not machine-current time). Choices use explicit wire values rather than C++ enum ordinal assumptions. Token identifies a prompt/connection attempt, not a new trust/key policy.

## Acceptance (3 groups)

1. Old encoded pairing/selection/status and frame fixture bytes remain unchanged; no existing numeric assignments moved. New complete messages round-trip with exact hand-derived fixtures.
2. Zero tokens, malformed choice/flags/code/text, truncation, oversize and trailing bytes reject; unknown kinds remain rejected. No secret material appears in payloads.
3. Initial behavioral RED uses a hand-built kind42 frame against current `decode_agent_message`, which presently rejects it. New codec cases are written before their implementation; then GREEN runs the new suite and unchanged `nstu.client_ipc`/pairing suites. Record failure and passing exits/hashes; no product main caller changes in this phase.

Risks: mismatched local binaries cannot complete pairing until updated together, preserving refusal. Additive API source compatibility only; no old payload/schema/wire mutation. Exit: narrow independent code review plus scoped tests; this phase alone is not a complete deliverable and cannot integrate.
