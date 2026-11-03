# Task Plan: FLEET-METADATA-SERVER-UI-1

**Task ID**: `FLEET-METADATA-SERVER-UI-1`  
**Phase**: Priority 3 / P3.1 Fleet Health & Diagnostics (Server Control Plane, Registry, & Technician UI Integration)  
**Roadmap Reference**: `REVISED_ROADMAP.md` (P3.1: "Show device/version/service/agent/network/capture/UWF state with last-seen age and stale-data indicator. Avoid high-frequency polling and unnecessary CPU/RAM/application/site metadata.") and `tasks/product-execution-plan.md` (`FLEET-01/02`: "Stale-aware existing health/details first; add only justified low-frequency/change-driven version/service metadata under a privacy contract.").

---

## 1. Description & Architectural Rationale

Following the completion of `FLEET-METADATA-CORE-1` (which established `docs/FLEET_TELEMETRY_PRIVACY_CONTRACT.md` and `CommandType::fleet_metadata = 69` codecs), this task integrates low-frequency client version and agent service connectivity telemetry into the server control plane, `ClientRegistry`, and the Technician Fleet Health dashboard (`FLEET-01/02`):

1. **Client Registry State Storage (`server/include/nstu/client_registry.hpp`, `server/src/client_registry.cpp`)**:
   - Extends `ClientRecord`:
     - `std::string client_version`: client software version string (e.g. `"0.1.0"`).
     - `bool agent_connected`: boolean indicating whether the student user-session agent process is responsive via local IPC.
   - Adds method to `ClientRegistry`:
     - `[[nodiscard]] bool set_fleet_metadata(std::uint64_t id, std::string version, bool agent_connected);`
     - Thread-safe mutation updating `client_version` and `agent_connected` without clobbering other client state or resetting monotonic `last_seen`.
     - Preserves existing metadata on standard status reports.

2. **Server Control Plane Frame Ingestion (`server/src/control_plane.cpp`)**:
   - In `ControlPlane::handle_authenticated_command`:
     - Handles `protocol::CommandType::fleet_metadata`:
       - Decodes payload via `control::decode_fleet_metadata(command->payload)`.
       - If decoding succeeds, updates the client's record in `registry_` via `set_fleet_metadata`.
       - Disconnects on malformed payload or decode failure (fail-closed security).
     - Adds `case protocol::CommandType::fleet_metadata:` to inbound command dispatch.

3. **Technician Fleet Health Dashboard Integration (`server/app/main.cpp`)**:
   - In `draw_technician_fleet_health`:
     - **Column 0 (Computer Name)**:
       - Enriches hover tooltip: displays `client.client_version` (or `"Unknown"` / `"Chưa rõ"`) and `Agent: Connected` / `Disconnected` (`Đã kết nối` / `Mất kết nối`).
     - **Column 2 (Status)**:
       - If an online client has `agent_connected == false`, renders an amber `[No Agent]` / `[Thiếu Agent]` badge next to the status with a descriptive hover tooltip warning that the student agent process is not active (remote input lock, kiosk, and broadcast window will not function).
     - **Summary Cards**:
       - Adds agent health count to fleet summary metrics: e.g. displays online clients and active agent count.

4. **Unit Test Coverage (`tests/server_state_tests.cpp`)**:
   - Tests `ClientRegistry::set_fleet_metadata`:
     - Valid mutation updates `client_version` and `agent_connected`.
     - Idempotent and non-clobbering over subsequent standard `upsert` calls.
     - Rejection for unknown `id` (`set_fleet_metadata(999, ...)` returns `false`).
     - State preserved across registry snapshots.

---

## 2. Acceptance Criteria

1. **Registry Telemetry Storage**: `ClientRecord` stores `client_version` and `agent_connected` cleanly without memory leaks or race conditions.
2. **Control Plane Ingestion**: `CommandType::fleet_metadata` is decoded fail-closed and updates the client record in `ClientRegistry`.
3. **Technician UI Visibility**: Version and agent connectivity are clearly surfaced in `fleet_health` table tooltips and status badges without disrupting layout or columns.
4. **Zero Intrusive Polling**: Metadata updates remain event-driven / low-frequency under the formal privacy contract.
5. **Preserve All Assertions**: All existing assertions in all 67 test suites remain 100% intact.
6. **Dual Verification Gates**: Zero compiler warnings, 67 of 67 test suites pass cleanly under `./scripts/verify-gate.sh` and `./scripts/verify-gate.sh --strict`.

---

## 3. File Allowlist & Baseline SHA-256 Hashes

| Path | Baseline SHA-256 | Description |
|---|---|---|
| `server/include/nstu/client_registry.hpp` | `A22103BE2DFEFC1103F8949E474C8106063238A7CD1C45BC3141B435819A50B9` | Add `client_version`, `agent_connected`, and `set_fleet_metadata` signature |
| `server/src/client_registry.cpp` | `37FB71582ADA017D715749B5F639C60EE15ED215BC33A1AA2B81D3654F5174D8` | Implement `set_fleet_metadata` in `ClientRegistry` |
| `server/src/control_plane.cpp` | `BFA861EA9B94D3A270C861E624DECB57F6EDDE6504A5A3F38CC4A0A137FD1915` | Ingest `CommandType::fleet_metadata` in control plane |
| `server/app/main.cpp` | `A0901540C5D66B054AD6D20C925D4F75D7F7D55B418720670009CD6EF200E03D` | Surface version and agent status in fleet table tooltips & badges |
| `tests/server_state_tests.cpp` | `97C250AE1BC6BE585BCB3FE43D6C4E9EA8C48216E5AEFB3544CBCF73F3D9DC12` | Unit tests for `ClientRegistry::set_fleet_metadata` |

Strictly 5 files modified ($\le 5$ files limit respected).

---

## 4. Dependencies & Non-Negotiable Invariants

- **Author attribution**: Must commit exclusively as `khoasoma <songtu438@gmail.com>` with no AI assistant trailers.
- **Preserved dirty paths**: Never stage, modify, or revert `README.md`, `README.vi.md`, `docs/assets/banner.png`.
- **Private files**: Never stage or commit `PRODUCT_ROADMAP_NSTU.md`, `artifacts/`, `.claude/`, or `HANDOFF.md`.
- **Preserve all assertions**: Every existing assertion in all 67 test suites must be preserved.
- **Low-Overhead Telemetry**: Low-frequency / event-driven reporting only under the privacy contract.

---

## 5. Verification Commands

```bash
# Fast verification gate (zero warnings, all 67 test suites pass)
./scripts/verify-gate.sh

# Strict verification gate (Release mode, -Werror, zero warnings)
./scripts/verify-gate.sh --strict
```

