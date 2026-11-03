# Task Result: DEMONSTRATION-MODES-CORE-1

**Task ID**: `DEMONSTRATION-MODES-CORE-1`  
**Phase**: Priority 2 / P2.3 Daily Teaching Workflows & Demonstration Modes (Protocol & Codecs)  
**Status**: Implemented, verified locally, ready for independent code review  

---

## 1. Summary of Changes

This task delivers the foundational binary wire protocol command, domain types, and codecs for teacher screen demonstration modes:
1. **Protocol Definition (`common/include/nstu/protocol.hpp`)**:
   - Added `CommandType::host_broadcast_mode = 68` for configuring demonstration mode and student input locking state.
2. **Domain Types & Codec Declarations (`common/include/nstu/control_messages.hpp`)**:
   - `DemonstrationMode` enum class: `focus = 0`, `follow_along = 1`.
   - `BroadcastModeMessage` struct: `mode` (DemonstrationMode), `lock_input` (bool).
   - Declared `encode_broadcast_mode` and `decode_broadcast_mode`.
3. **Codec Implementations (`common/src/control_messages.cpp`)**:
   - Implemented `encode_broadcast_mode`:
     - Validates `mode` enum bounds ($0 \dots 1$). Returns `{}` on invalid mode.
     - Serializes 2-byte LE version `kBroadcastModeVersion = 1`.
     - Serializes 1-byte `mode` and 1-byte `lock_input` ($0\text{x}00$ or $0\text{x}01$).
     - Exact wire length: 4 bytes.
   - Implemented `decode_broadcast_mode`:
     - Enforces exact payload size `payload.size() == 4` (rejecting both truncated payloads and trailing garbage).
     - Validates version == 1.
     - Validates mode byte $\le 1$.
     - Validates lock_input byte $\le 1$ ($0\text{x}00$ or $0\text{x}01$).
     - Returns `std::nullopt` on any deviation (fail-closed).
4. **Unit Tests (`tests/control_channel_tests.cpp`)**:
   - Validated round-trips for all 4 combinations (focus/follow_along, with/without input lock).
   - Validated wire layout (LE version bytes, mode offset, lock flag offset).
   - Validated length rejections (0, 1, 2, 3, and 5 bytes).
   - Validated version rejections (version 0, version 2).
   - Validated mode bounds rejections (mode 2, mode 255).
   - Validated bool bounds rejections (byte 2, byte 128).
   - Validated encoder rejection of invalid mode enum.

---

## 2. File Hashes & Allowlist Compliance

| Path | Baseline SHA-256 | Modified SHA-256 | Status |
|---|---|---|---|
| `common/include/nstu/protocol.hpp` | `AC061A0A9324686393B7C6417B0DA323DD6F9ED0E311E02F0DB9BABED1C2049F` | `F081195996DB2B8BBD44A1961C6771BCDAEECA84BA40C11BD758C480A28A0ECB` | Modified |
| `common/include/nstu/control_messages.hpp` | `73234204BC3E21D4BE914682B60D49DD53F645FDB9DF05304D4D2B447808ED31` | `6D949BFBFA401089534E3014C09BD8C6E0F743F4BB371699EB2C7AFFF810ECC9` | Modified |
| `common/src/control_messages.cpp` | `AB229F5AADC75CDAC00BE13BEDBC4BEBCFCAF0551E1552E786E3B2973DC3ACAF` | `6067370A2B3E28708622BD29EAF7CEEF9306402312F18B7DA522BD06CA84F958` | Modified |
| `tests/control_channel_tests.cpp` | `A9E5E8D99EC59BF8B02A289B612BDA90E3E122F32E12BEFB8FCEFA939299A034` | `967DD3E959CD3339CF9BBCFA775B7B1B748F9F0594C036169625FDD7618E1A0B` | Modified |

Exactly 4 files modified ($\le 5$ files limit respected).

---

## 3. Verification Receipts

### Fast Verification Gate (`./scripts/verify-gate.sh`)
- Zero compiler warnings.
- 67 of 67 test suites passed (100%).
- All repository rules checked and passed (no banned vendor identifiers, forbidden files uncommitted, zero assertion weakening).

### Strict Verification Gate (`./scripts/verify-gate.sh --strict`)
- Release configuration with `-Werror` and strict compiler warnings.
- Zero compiler warnings.
- 67 of 67 test suites passed (100%).

---

## 4. Invariant & Safety Checklist

- [x] Fail-closed codec parsing at trust boundaries.
- [x] Trailing byte rejection enforced via strict size checking.
- [x] Version negotiation and checking strictly enforced.
- [x] Backward compatibility preserved (snapshot frame codec untouched).
- [x] No existing assertions modified or removed across all 67 test suites.
- [x] Low-bandwidth snapshot wall monitoring unaffected.
- [x] No banned vendor identifiers introduced.

