# Task Plan: DEMONSTRATION-MODES-CLIENT-IPC-1

**Task ID**: `DEMONSTRATION-MODES-CLIENT-IPC-1`  
**Phase**: Priority 2 / P2.3 Daily Teaching Workflows & Demonstration Modes (Client IPC & Service Relay)  
**Roadmap Reference**: `REVISED_ROADMAP.md` (P2.3: "Demonstration modes: Focus and Follow-Along are distinct; snapshot wall remains low-bandwidth. Validate stream resource release and disconnect cleanup.") and `tasks/product-execution-plan.md` (`TOOL-08/09`: "Reuse Focus broadcast with resource/input-lock cleanup, then separate resizable Follow-Along. Keep the class wall snapshot-first.").

---

## 1. Description & Architectural Rationale

This task implements the client service and inter-process communication (IPC) channel layer for Demonstration Modes (`TOOL-08/09`):
1. **Agent Protocol Definition (`client/include/nstu/agent_protocol.hpp`)**:
   - `AgentMessageType::host_broadcast_mode = 41`: Service-to-agent message carrying encoded `BroadcastModeMessage`.
2. **IPC Protocol Codec & Type Bounds (`client/src/agent_protocol.cpp`)**:
   - Update `valid_type` upper bound to `AgentMessageType::host_broadcast_mode`.
   - Ensures `encode_agent_message` and `decode_agent_message` accept and safely deserialize the new message type.
3. **Win32 Client Service Command Relay (`client/service/main.cpp`)**:
   - In `handle_server_command`, decode and validate `protocol::CommandType::host_broadcast_mode` fail-closed via `control::decode_broadcast_mode(command.payload)`.
   - Forward valid payloads to the agent session queue as `AgentMessageType::host_broadcast_mode`.
4. **IPC Unit Tests (`tests/client_ipc_tests.cpp`)**:
   - Update bounds testing in `tests/client_ipc_tests.cpp` to verify that out-of-bounds type 42 is rejected.
   - Add comprehensive tests for `AgentMessageType::host_broadcast_mode`: serialization, deserialization, queue push, and queue take.

---

## 2. Acceptance Criteria

1. **IPC Protocol**: `AgentMessageType::host_broadcast_mode = 41` defined and validated in `client/src/agent_protocol.cpp`.
2. **Service Command Relaying**: Client service verifies valid 4-byte broadcast mode payloads and relays them to the agent session queue.
3. **IPC Unit Tests**: `tests/client_ipc_tests.cpp` tests message encoding, decoding, and bounded queueing.
4. **No Regression**: All 67 test suites pass cleanly with zero warnings under fast and strict gates.

---

## 3. File Allowlist & Baseline SHA-256 Hashes

| Path | Baseline SHA-256 | Description |
|---|---|---|
| `client/include/nstu/agent_protocol.hpp` | `A76F73FC48AE5CC598B87D4FAC727BF9E8B60387F0C60E45454968D1AD2CB8F8` | Declare `AgentMessageType::host_broadcast_mode = 41` |
| `client/src/agent_protocol.cpp` | `589EB8AAB71EBE330D0236C69D2081F4005DA90B810B76DAD93BF5155CEEB684` | Update `valid_type` upper bound check |
| `client/service/main.cpp` | `2CA817AB7720CCA8D30D05E3DF4BA9684B97DED3F15B1E269F94C8290C723418` | Forward `host_broadcast_mode` commands to agent queue |
| `tests/client_ipc_tests.cpp` | `2D092655E909C0C2C80A58A949157FE98AF93CF2E96621C2AFA6CAE912AF17A9` | Unit tests for demonstration mode IPC message |

---

## 4. Dependencies & Non-Negotiable Invariants

- **Author attribution**: Must commit exclusively as `khoasoma <songtu438@gmail.com>` with no AI assistant trailers.
- **Preserved dirty paths**: Never stage, modify, or revert `README.md`, `README.vi.md`, `docs/assets/banner.png`.
- **Private files**: Never stage or commit `PRODUCT_ROADMAP_NSTU.md`, `artifacts/`, `.claude/`, or `HANDOFF.md`.
- **Preserve all assertions**: Every existing assertion in all 67 test suites must be preserved.

---

## 5. Verification Commands

```bash
# Fast verification gate (zero warnings, all 67 test suites pass)
./scripts/verify-gate.sh

# Strict verification gate (Release mode, -Werror, zero warnings)
./scripts/verify-gate.sh --strict
```

