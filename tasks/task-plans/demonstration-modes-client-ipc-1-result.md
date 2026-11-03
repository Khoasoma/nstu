# Task Result: DEMONSTRATION-MODES-CLIENT-IPC-1

**Task ID**: `DEMONSTRATION-MODES-CLIENT-IPC-1`  
**Phase**: Priority 2 / P2.3 Daily Teaching Workflows & Demonstration Modes (Client IPC & Service Relay)  
**Status**: Implemented, verified locally, ready for independent code review  

---

## 1. Summary of Changes

This task implements the client service command relay and named pipe IPC channel layer for Demonstration Modes:
1. **Agent Protocol Definition (`client/include/nstu/agent_protocol.hpp`)**:
   - Declared `AgentMessageType::host_broadcast_mode = 41`.
2. **IPC Codec Validation (`client/src/agent_protocol.cpp`)**:
   - Updated `valid_type` upper bound to `AgentMessageType::host_broadcast_mode`.
   - Ensures `encode_agent_message` and `decode_agent_message` validate and accept type 41.
3. **Client Service Command Relay (`client/service/main.cpp`)**:
   - In `handle_server_command`, added handling for `protocol::CommandType::host_broadcast_mode`.
   - Validates payload fail-closed via `control::decode_broadcast_mode`.
   - Relays valid payloads to local agent session queue as `AgentMessageType::host_broadcast_mode`.
4. **IPC Unit Tests (`tests/client_ipc_tests.cpp`)**:
   - Updated out-of-bounds check from 41 to 42 to match the new valid type limit.
   - Added unit test exercising `AgentMessageType::host_broadcast_mode`: serialization, deserialization, queue push, queue take, and payload decode.

---

## 2. File Hashes & Allowlist Compliance

| Path | Baseline SHA-256 | Modified SHA-256 | Status |
|---|---|---|---|
| `client/include/nstu/agent_protocol.hpp` | `A76F73FC48AE5CC598B87D4FAC727BF9E8B60387F0C60E45454968D1AD2CB8F8` | `69C79C505DF69DF0C1D49001D9517135C5E0F0F3F4A506DAABFA6211F295AD2E` | Modified |
| `client/src/agent_protocol.cpp` | `589EB8AAB71EBE330D0236C69D2081F4005DA90B810B76DAD93BF5155CEEB684` | `F66F38F5A77E3E81960405A0DA110B6E7A8E1383BFB08C2B12C86E81AF42D738` | Modified |
| `client/service/main.cpp` | `2CA817AB7720CCA8D30D05E3DF4BA9684B97DED3F15B1E269F94C8290C723418` | `B26B49F0D7E8C399C026457BEC9B6A81CCD0A8EFF346F397E0CD7935CF8AA58F` | Modified |
| `tests/client_ipc_tests.cpp` | `2D092655E909C0C2C80A58A949157FE98AF93CF2E96621C2AFA6CAE912AF17A9` | `88AEB2CEE1221F16AE86B58C4D8CCE34281BD28915E4C0C295BA915016D1146C` | Modified |

Strictly 4 files modified ($\le 5$ files limit respected).

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

- [x] Fail-closed validation before forwarding to agent queue.
- [x] Exact upper bound validation maintained.
- [x] Zero assertion removals across all 67 test suites.
- [x] No banned vendor identifiers introduced.

