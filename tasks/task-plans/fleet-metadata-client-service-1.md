# Task Plan: FLEET-METADATA-CLIENT-SERVICE-1

**Task ID**: `FLEET-METADATA-CLIENT-SERVICE-1`  
**Phase**: Priority 3 / P3.1 Fleet Health & Diagnostics (Client Service Metadata Reporting & Live Agent State Synchronization)  
**Roadmap Reference**: `REVISED_ROADMAP.md` (P3.1: "Show device/version/service/agent/network/capture/UWF state with last-seen age and stale-data indicator. Avoid high-frequency polling and unnecessary CPU/RAM/application/site metadata.") and `tasks/product-execution-plan.md` (`FLEET-01/02`: "Stale-aware existing health/details first; add only justified low-frequency/change-driven version/service metadata under a privacy contract.").

---

## 1. Description & Architectural Rationale

Following the completion of `FLEET-METADATA-CORE-1` (wire codecs and formal privacy contract) and `FLEET-METADATA-SERVER-UI-1` (server registry ingestion and technician UI surfacing), this task completes the end-to-end telemetry loop by implementing low-frequency, change-driven metadata reporting in the Windows client service (`client/service/main.cpp`):

1. **Initial Connection Metadata Dispatch (`client/service/main.cpp`)**:
   - In `client_service_main` / control session initialization loop:
     - Defines `NSTU_PROJECT_VERSION` fallback (defaulting to `"0.1.0"` if not already defined).
     - Upon establishing an authenticated TCP control channel, immediately encodes and queues a `CommandType::fleet_metadata = 69` message carrying `NSTU_PROJECT_VERSION` and `g_agent_connected.load()`.
     - Placed alongside `CommandType::wol_report` and `CommandType::freeze_report` to ensure immediate registration without waiting for periodic timers.

2. **Event-Driven Agent Connectivity State Synchronization (`client/service/main.cpp`)**:
   - In `pipe_worker` (local named pipe IPC worker handling student agent connections):
     - When a student agent connects (`g_agent_connected = true`), encodes and queues `CommandType::fleet_metadata` reporting `agent_connected = true`.
     - When a student agent disconnects (`g_agent_connected = false`), encodes and queues `CommandType::fleet_metadata` reporting `agent_connected = false`.
     - Strictly event-driven: zero polling or timer-based metadata generation.

3. **Integration Unit Testing (`tests/client_control_tests.cpp`)**:
   - Tests end-to-end dispatch of `CommandType::fleet_metadata` from a client control session into `ServerControlPlane`.
   - Asserts that upon receiving the outbound metadata command, the server updates `ClientRegistry` with the client's build version and `agent_connected` state.
   - Asserts that subsequent updates (e.g. agent disconnect) accurately reflect in the registry.

---

## 2. Acceptance Criteria

1. **Authenticated Connection Delivery**: The client service reports `client_version` and initial `agent_connected` state upon authenticated connection to the teacher server.
2. **Event-Driven IPC Reflection**: Student agent connect and disconnect events dynamically dispatch updated `fleet_metadata` to the teacher server.
3. **Zero Polling Overhead**: Fleet metadata frames are emitted only on connection or discrete IPC state transitions, strictly complying with `docs/FLEET_TELEMETRY_PRIVACY_CONTRACT.md`.
4. **Preserve All Assertions**: All existing assertions in all 67 test suites remain 100% intact.
5. **Dual Verification Gates**: Zero compiler warnings, 67 of 67 test suites pass cleanly under `./scripts/verify-gate.sh` and `./scripts/verify-gate.sh --strict`.

---

## 3. File Allowlist & Baseline SHA-256 Hashes

| Path | Baseline SHA-256 | Description |
|---|---|---|
| `client/service/main.cpp` | `B26B49F0D7E8C399C026457BEC9B6A81CCD0A8EFF346F397E0CD7935CF8AA58F` | Dispatch `fleet_metadata` on connection and agent pipe connect/disconnect |
| `tests/client_control_tests.cpp` | `E5902810FBFC9953A1D7F35B881C48344674CC00FFB68D3C5D757ED67B78122D` | Integration test verifying client fleet metadata delivery to server registry |

Strictly 2 files modified ($\le 5$ files limit respected).

---

## 4. Dependencies & Non-Negotiable Invariants

- **Author attribution**: Must commit exclusively as `khoasoma <songtu438@gmail.com>` with no AI assistant trailers.
- **Preserved dirty paths**: Never stage, modify, or revert `README.md`, `README.vi.md`, `docs/assets/banner.png`.
- **Private files**: Never stage or commit `PRODUCT_ROADMAP_NSTU.md`, `artifacts/`, `.claude/`, or `HANDOFF.md`.
- **Preserve all assertions**: Every existing assertion in all 67 test suites must be preserved.
- **Privacy Guarantee**: No intrusive inspection, process listing, or high-frequency polling.

---

## 5. Verification Commands

```bash
# Fast verification gate (zero warnings, all 67 test suites pass)
./scripts/verify-gate.sh

# Strict verification gate (Release mode, -Werror, zero warnings)
./scripts/verify-gate.sh --strict
```

