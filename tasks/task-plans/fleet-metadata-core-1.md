# Task Plan: FLEET-METADATA-CORE-1

**Task ID**: `FLEET-METADATA-CORE-1`  
**Phase**: Priority 3 / P3.1 Fleet Health & Diagnostics (Privacy Contract & Low-Frequency Metadata Wire Codecs)  
**Roadmap Reference**: `REVISED_ROADMAP.md` (P3.1 & Cross-Cutting Privacy Gates: "Show device/version/service/agent/network/capture/UWF state with last-seen age and stale-data indicator. Avoid high-frequency polling and unnecessary CPU/RAM/application/site metadata. List fields, purpose, collection condition, viewer roles, retention and deletion.") and `tasks/product-execution-plan.md` (`FLEET-01/02`: "Stale-aware existing health/details first; add only justified low-frequency/change-driven version/service metadata under a privacy contract.").

---

## 1. Description & Architectural Rationale

This task delivers the foundational privacy specification and binary wire codecs for low-frequency client version and agent service connectivity metadata (`FLEET-01/02`):

1. **Formal Fleet Telemetry Privacy Contract (`docs/FLEET_TELEMETRY_PRIVACY_CONTRACT.md`)**:
   - Explicitly fulfills the cross-cutting privacy and retention gate in `REVISED_ROADMAP.md` before deploying version/service metadata.
   - Documents the complete enumeration of collected fields (`hostname`, `address`, `delivery`, `latency_ms`, `packet_loss_per_mille`, `streaming`, `snapshotting`, `viewing_broadcast`, `uwf`, `last_seen`, `client_version`, `agent_connected`).
   - Specifies the diagnostic purpose of each field (software drift detection, network troubleshooting, exam readiness).
   - Establishes collection conditions: low-frequency (initial handshake, or change-driven on agent connect/disconnect); never high-frequency polling.
   - Enforces viewer role isolation: accessible exclusively to Technician (`OperatorRole::technician`) and Developer (`OperatorRole::developer`).
   - Codifies retention guarantees: volatile in-memory lifetime in `ClientRegistry`, zero persistence to disk or journals, instant purge upon disconnection.
   - Lists strict negative exclusions: zero keystrokes, zero browser history, zero user documents, zero process enumeration, zero CPU/RAM utilization polling.

2. **Protocol Command Definition (`common/include/nstu/protocol.hpp`)**:
   - Adds `CommandType::fleet_metadata = 69`:
     - Client $\rightarrow$ Server: carries low-frequency / change-driven client build version and agent service connectivity status.

3. **Binary Codec Implementation (`common/include/nstu/control_messages.hpp`, `common/src/control_messages.cpp`)**:
   - Declares and defines `struct FleetMetadataMessage`:
     - `std::string client_version`: semantic version string (e.g., `"0.1.0"`, max 32 ASCII characters).
     - `bool agent_connected`: boolean indicating whether the student agent named pipe IPC is currently active.
   - Implements fail-closed `encode_fleet_metadata` and `decode_fleet_metadata`:
     - LE encoding: version byte (`kFleetMetadataVersion = 1`), `agent_connected` flag byte (0 or 1), 2-byte string length (`uint16_t`), and UTF-8 / ASCII version string bytes.
     - Bounds checking: maximum 32 bytes for version string. Rejects empty strings, strings exceeding 32 bytes, non-boolean flags, unknown versions, or trailing unexpected payload bytes.

4. **Unit Test Coverage (`tests/control_channel_tests.cpp`)**:
   - Comprehensive test assertions for `encode_fleet_metadata` and `decode_fleet_metadata`:
     - Round-trip serialization/deserialization for valid version strings and agent connection states (`true`/`false`).
     - Rejection of unknown codec versions.
     - Rejection of non-boolean flag values (> 1).
     - Rejection of version strings exceeding maximum length bound (32 bytes).
     - Rejection of empty version strings.
     - Rejection of truncated buffers and trailing garbage bytes.

---

## 2. Acceptance Criteria

1. **Formal Privacy Contract**: `docs/FLEET_TELEMETRY_PRIVACY_CONTRACT.md` is complete, enumerating all fields, purposes, low-frequency collection rules, viewer role restrictions, retention lifetimes, and negative privacy guarantees.
2. **Fail-Closed Binary Codecs**: `encode_fleet_metadata` and `decode_fleet_metadata` reject any buffer with invalid flags, unknown versions, length violations, or trailing bytes.
3. **Low-Overhead / Low-Frequency Principle**: Codec payload is strictly bounded ($\le 40$ bytes total), preventing network overhead.
4. **Preserve All Assertions**: All existing assertions across all 67 test suites remain 100% intact.
5. **Dual Verification Gates**: Zero compiler warnings, 67 of 67 test suites pass cleanly under `./scripts/verify-gate.sh` and `./scripts/verify-gate.sh --strict`.

---

## 3. File Allowlist & Baseline SHA-256 Hashes

| Path | Baseline SHA-256 | Description |
|---|---|---|
| `docs/FLEET_TELEMETRY_PRIVACY_CONTRACT.md` | `NEW` | Formal privacy contract and retention specification for fleet telemetry |
| `common/include/nstu/protocol.hpp` | `F081195996DB2B8BBD44A1961C6771BCDAEECA84BA40C11BD758C480A28A0ECB` | Define `CommandType::fleet_metadata = 69` |
| `common/include/nstu/control_messages.hpp` | `6D949BFBFA401089534E3014C09BD8C6E0F743F4BB371699EB2C7AFFF810ECC9` | Declare `FleetMetadataMessage`, encoder and decoder |
| `common/src/control_messages.cpp` | `6067370A2B3E28708622BD29EAF7CEEF9306402312F18B7DA522BD06CA84F958` | Implement `encode_fleet_metadata` and `decode_fleet_metadata` |
| `tests/control_channel_tests.cpp` | `967DD3E959CD3339CF9BBCFA775B7B1B748F9F0594C036169625FDD7618E1A0B` | Unit tests for fleet metadata codec validation and bounds |

Strictly 5 files modified/added ($\le 5$ files limit respected).

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

