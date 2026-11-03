# Task Result: FLEET-METADATA-CORE-1

**Task ID**: `FLEET-METADATA-CORE-1`  
**Phase**: Priority 3 / P3.1 Fleet Health & Diagnostics (Privacy Contract & Low-Frequency Metadata Wire Codecs)  
**Status**: COMPLETED & VERIFIED LOCALLY  

---

## 1. Summary of Work

1. **Formal Fleet Telemetry Privacy Contract (`docs/FLEET_TELEMETRY_PRIVACY_CONTRACT.md`)**:
   - Authored formal specification satisfying the cross-cutting privacy and retention gates of `REVISED_ROADMAP.md` and `tasks/product-execution-plan.md` (`FLEET-01/02`).
   - Catalogued every collected field with its primary operational rationale and low-frequency / change-driven trigger conditions.
   - Enforced non-negotiable negative privacy boundaries: zero keystroke logging, zero clipboard inspection, zero web browsing tracking, zero student personal identity extraction, zero process enumeration, zero CPU/RAM utilization polling.
   - Restricted fleet health viewer authorization to Technician (`OperatorRole::technician`) and Developer (`OperatorRole::developer`).
   - Codified volatile in-memory lifetime in `ClientRegistry` with zero disk persistence or journal writes, and immediate session termination purge.

2. **Protocol Command Definition (`common/include/nstu/protocol.hpp`)**:
   - Added `CommandType::fleet_metadata = 69` for low-frequency client version and student agent service connectivity telemetry.

3. **Binary Wire Codecs (`common/include/nstu/control_messages.hpp`, `common/src/control_messages.cpp`)**:
   - Declared and implemented `FleetMetadataMessage`:
     - `client_version`: ASCII string bounded to $\le 32$ bytes (`kMaximumClientVersionBytes`).
     - `agent_connected`: boolean flag indicating active student agent named pipe IPC.
   - Implemented fail-closed `encode_fleet_metadata` and `decode_fleet_metadata`:
     - Version negotiation (`kFleetMetadataVersion = 1`).
     - Rejection of empty version strings, version strings $> 32$ bytes, non-boolean flags ($> 1$), unknown versions, truncated buffers, and trailing unexpected bytes.

4. **Unit Test Suite (`tests/control_channel_tests.cpp`)**:
   - Added exhaustive tests covering:
     - Valid roundtrip with `agent_connected = true`.
     - Valid roundtrip with `agent_connected = false` and complex semver string.
     - Exact 32-byte boundary version string.
     - Encoder rejection of oversized version strings ($> 32$ bytes) and empty strings.
     - Decoder rejection of empty payload, truncated headers, invalid codec versions, invalid flag bytes ($> 1$), zero string lengths, length $> 32$, and trailing garbage bytes.

---

## 2. File Hashes & Allowlist Audit

| Path | Baseline SHA-256 | Modified SHA-256 |
|---|---|---|
| `docs/FLEET_TELEMETRY_PRIVACY_CONTRACT.md` | `NEW` | `EFBD20050247985132D9BB27D3D20306F18EF3C8A2C195452423D24683C8D89A` |
| `common/include/nstu/protocol.hpp` | `F081195996DB2B8BBD44A1961C6771BCDAEECA84BA40C11BD758C480A28A0ECB` | `E691E8AB4F1A95463630ED5E1E44834C39734B347F37C001876AB5DB6781328D` |
| `common/include/nstu/control_messages.hpp` | `6D949BFBFA401089534E3014C09BD8C6E0F743F4BB371699EB2C7AFFF810ECC9` | `90D6B49D7C19279BA72C940B552A6F04F89232D88D61DC71280630240240E793` |
| `common/src/control_messages.cpp` | `6067370A2B3E28708622BD29EAF7CEEF9306402312F18B7DA522BD06CA84F958` | `9282DFD12903F942E411E846FB3A618EE377068E354EC537254533095846C5B1` |
| `tests/control_channel_tests.cpp` | `967DD3E959CD3339CF9BBCFA775B7B1B748F9F0594C036169625FDD7618E1A0B` | `F5E010A2F25E02862329A36FB765A2549B05E319F6942C99BA562376AF66D684` |

Strictly 5 files modified/added ($\le 5$ files limit respected).

---

## 3. Verification Gate Receipts

### Fast Verification Gate (`./scripts/verify-gate.sh`)
- Vendor name check: PASS
- Forbidden paths check: PASS
- No weakened tests check: PASS
- Exam web recovery tests: 63 passed
- Build: PASS (0 compiler warnings)
- Tests: 67 of 67 test suites passed (100%)

### Strict Verification Gate (`./scripts/verify-gate.sh --strict`)
- Mode: Release + `NSTU_ENABLE_WERROR=ON`
- Vendor name check: PASS
- Forbidden paths check: PASS
- No weakened tests check: PASS
- Exam web recovery tests: 63 passed
- Build: PASS (0 compiler warnings under `-Werror`)
- Tests: 67 of 67 test suites passed (100%)

