# Task Plan: WAKE-ON-LAN-CORE-1

**Task ID**: `WAKE-ON-LAN-CORE-1`  
**Phase**: Priority 2 / P2.3 Daily Teaching Workflows & Power Actions  
**Roadmap Reference**: `REVISED_ROADMAP.md` (P2.3: "Power actions: server-side authorization, count/confirmation, offline semantics, force-close disabled by default; Wake-on-LAN only with honest support detection") and `tasks/product-execution-plan.md` (`TOOL-03/04/05`).

---

## 1. Description & Architectural Rationale

Following the completion of `POWER-ACTIONS-CORE-1` and `POWER-ACTIONS-UI-1`, the remaining essential component of the classroom power management suite is **Wake-on-LAN (WOL)**. In computer laboratory environments, workstations are powered down or in deep sleep outside class hours. Instructors and technicians require the ability to turn on laboratory workstations remotely via standard UDP magic packets before class commences.

Crucially, per `REVISED_ROADMAP.md`, Wake-on-LAN must operate with **honest support detection**:
- Never fake, simulate, or assume Wake-on-LAN support.
- Track explicit `WolSupportState` (`unknown`, `supported`, `unsupported`, `missing_mac`).
- If a client machine lacks a recorded MAC address or is known to have unsupported NIC hardware/drivers, the server must report honest failure reasons rather than pretending to wake or broadcasting into a void.
- Enforce offline semantics: only offline clients are eligible targets for Wake-on-LAN; online clients are already active and skipped.
- Enforce exam active invariant: wake commands are disarmed if an assessment is marked active on the target identity.
- Enforce P2.1 role permissions: Teacher, Technician, and Developer are permitted to wake offline machines (`OperatorAction::wake_on_lan`).

This task implements the core protocol, codecs, network broadcasting abstractions, server control plane logic, classroom roster bulk action, and unit test suite. UI controls and client-side IP Helper network adapter probes will follow in `WAKE-ON-LAN-UI-1`.

---

## 2. Acceptance Criteria

1. **Protocol & MAC Abstraction (`common/include/nstu/wake_on_lan.hpp`, `common/src/wake_on_lan.cpp`)**:
   - `MacAddress`: 6-byte octet container with equality operator, `is_zero()`, and `is_broadcast()`.
   - `parse_mac_address`: Parses standard MAC string formats (`00:11:22:33:44:55`, `00-11-22-33-44-55`, case-insensitive hex). Rejects invalid characters, incorrect length, wrong separators, or trailing bytes.
   - `format_mac_address`: Formats `MacAddress` into standard lowercase or uppercase colon/hyphen separated string.
   - `create_magic_packet`: Builds standard 102-byte Magic Packet (6 bytes of `0xFF` followed by 16 repetitions of the target MAC address).
   - `verify_magic_packet`: Validates that a byte buffer contains a valid 102-byte magic packet matching an expected MAC address.
   - `send_wake_on_lan_packet`: Broadcasts 102-byte magic packet via UDP to configured broadcast address (default `255.255.255.255:9`) using `SO_BROADCAST`.
   - `WolSupportState` enum (`unknown = 0`, `supported = 1`, `unsupported = 2`, `missing_mac = 3`) with `to_string` and bilingual `wol_support_label`.
2. **Wire Protocol & Codecs (`common/include/nstu/protocol.hpp`, `common/include/nstu/control_messages.hpp`, `common/src/control_messages.cpp`)**:
   - `CommandType::wol_report = 62` (client -> server).
   - `WolReportMessage`: contains `MacAddress mac` and `WolSupportState support`.
   - `encode_wol_report` and `decode_wol_report`: 8-byte aligned binary codec (6 bytes MAC, 1 byte support state, 1 byte reserved), with strict payload validation.
3. **Role Separation (`server/include/nstu/operator_role.hpp`, `server/src/operator_role.cpp`)**:
   - Add `OperatorAction::wake_on_lan = 12`.
   - `is_action_permitted`: `Teacher`, `Technician`, and `Developer` roles are permitted to execute `wake_on_lan`.
4. **Client Registry (`server/include/nstu/client_registry.hpp`, `server/src/client_registry.cpp`)**:
   - `ClientRecord`: add `std::string mac_address{}` and `WolSupportState wol_support{WolSupportState::unknown}`.
   - Add `ClientRegistry::set_wol_report(uint64_t id, const MacAddress& mac, WolSupportState support)` updating MAC and WOL capability.
   - MAC and WOL status persist when client transitions between online and offline.
5. **Server Control Plane (`server/include/nstu/control_plane.hpp`, `server/src/control_plane.cpp`)**:
   - Handle incoming `CommandType::wol_report` in client command dispatcher, validating payload and updating registry.
   - `ServerControlPlane::wake_client(client_id, caller_role, &error)`:
     - Rejects unauthorized caller roles.
     - Rejects if exam is active on client.
     - Rejects if client is unknown.
     - Rejects if client is already online ("client is already online").
     - Rejects if client has no recorded MAC address ("cannot wake client: MAC address is unknown").
     - Rejects if client hardware is known to not support WOL ("cannot wake client: Wake-on-LAN is not supported by client hardware").
     - On valid offline client with known MAC, broadcasts 102-byte UDP magic packet to subnet.
6. **Classroom Roster & Bulk Action (`server/include/nstu/classroom_roster.hpp`, `server/src/classroom_roster.cpp`)**:
   - Add `BulkActionKind::wake_on_lan = 7`.
   - `evaluate_bulk_targets`: for `wake_on_lan`, offline clients are eligible; online clients are skipped.
   - `execute_bulk_action`: invokes `control_plane.wake_client` for selected targets, recording successes, failures, and honest error messages.
   - Bilingual confirmation title, message, and result status formatters.
7. **Verification & Test Suite (`tests/wake_on_lan_tests.cpp`, `tests/CMakeLists.txt`)**:
   - CMake executable target `nstu_wake_on_lan_tests`.
   - Comprehensive test coverage for:
     - MAC parsing, formatting, edge cases, malformed strings.
     - Magic packet generation and verification.
     - Codec round-trip and malformed payload rejection.
     - Operator role permissions.
     - Client registry report handling and state retention across offline transitions.
     - `ServerControlPlane::wake_client` authorization, exam protection, online rejection, unknown client rejection, missing MAC rejection, unsupported hardware rejection, and successful magic packet broadcast.
     - Classroom roster bulk wake evaluation and execution with offline semantics.
     - Bilingual confirmation and result formatters.
   - 100% pass on fast gate (`./scripts/verify-gate.sh`) and strict gate (`./scripts/verify-gate.sh --strict`) with zero compiler warnings.

---

## 3. File Allowlist & Baseline SHA-256 Hashes

| Path | Baseline SHA-256 | Description |
|---|---|---|
| `common/include/nstu/wake_on_lan.hpp` | *NEW* | MAC address, magic packet, WOL support enums & broadcast declaration |
| `common/src/wake_on_lan.cpp` | *NEW* | Implementation of MAC codecs, magic packet builder, and UDP broadcast |
| `common/CMakeLists.txt` | `d432d01516df5205fc401f80c142a644b5e01d5a296a7be678cba89148fa3b35` | Add `wake_on_lan.cpp` and `wake_on_lan.hpp` to `nstu_common` |
| `common/include/nstu/protocol.hpp` | `5a2432b043d848437f6412edc46be03ef5da6d9c03584bf491b35dc249df3b31` | Add `CommandType::wol_report = 62` |
| `common/include/nstu/control_messages.hpp` | `a5b1847b9c379c41c8b287d213ab476b1e4118ad9b90b7579554b1088cdf4676` | Declare `WolReportMessage`, `encode_wol_report`, `decode_wol_report` |
| `common/src/control_messages.cpp` | `be4b1e4078be600dd432d57b072b4121771fdbb9e9735eae073317846aa90295` | Implement `encode_wol_report`, `decode_wol_report` |
| `server/include/nstu/operator_role.hpp` | `20d524120b8dda100737a43d3af8a1bab4b90e93939c64b5c8ad3378e4d42e4d` | Add `OperatorAction::wake_on_lan = 12` |
| `server/src/operator_role.cpp` | `65264427b63babe6a85229e7f108be59622b0609ebe6993a9bbd4476ad8dc4a3` | Permitted for Teacher, Technician, Developer |
| `server/include/nstu/client_registry.hpp` | `8e5a4ea80f76eb8386c7cd04e0485c72e75b0e853422b92ac27553c9f7062964` | Add `mac_address`, `wol_support` to `ClientRecord`, declare `set_wol_report` |
| `server/src/client_registry.cpp` | `17347a89baf917cacac5d5d41915e72af7667aa99a5af5982e30ef7bd0eb6558` | Implement `set_wol_report` |
| `server/include/nstu/control_plane.hpp` | `452a42c851b6f815cf386e531749f878216208e122531b161f220043d8af7853` | Declare `ServerControlPlane::wake_client` |
| `server/src/control_plane.cpp` | `f13c4cc8e304658d646033c6bcfac584fe644586a7fd4cabd6de5b0c26514ce7` | Implement `wake_client` and `CommandType::wol_report` handling |
| `server/include/nstu/classroom_roster.hpp` | `2f0bfe48d0463ff9969d073d30eec477dba8801fbb7eb9b5465cd358a104b53a` | Add `BulkActionKind::wake_on_lan = 7` |
| `server/src/classroom_roster.cpp` | `821da1347673bc6cfdbc86acbf8e933db2955af0b21c6c321ad2295e4dac2620` | Bulk wake evaluation, execution, and bilingual formatting |
| `tests/CMakeLists.txt` | `14591422dc6fffd5feb1aa890bbc538ac44ccf6174c5a402b830f3d51c1ca447` | Add `nstu_wake_on_lan_tests` target |
| `tests/wake_on_lan_tests.cpp` | *NEW* | Unit test suite for Wake-on-LAN and honest support detection |

---

## 4. Dependencies & Non-Negotiable Invariants

- **Author attribution**: Must commit exclusively as `khoasoma <songtu438@gmail.com>` with no AI assistant trailers.
- **Preserved dirty paths**: Never stage, modify, or revert `README.md`, `README.vi.md`, `docs/assets/banner.png`.
- **Private files**: Never stage or commit `PRODUCT_ROADMAP_NSTU.md`, `artifacts/`, `.claude/`, or `HANDOFF.md`.
- **Preserve all assertions**: Every existing unit test and assertion across all 65 test suites must pass without modification.
- **Honest reporting**: Waking a client without a MAC address or with unsupported hardware must fail honestly and explicitly.
- **Exam protection**: Cannot wake or disrupt any client marked active in an assessment.

---

## 5. Verification Commands

```bash
# Build and run focused test suite
cmake --build build -j --target nstu_wake_on_lan_tests
ctest --test-dir build -R nstu_wake_on_lan_tests --output-on-failure

# Fast verification gate (zero warnings, all unit tests pass)
./scripts/verify-gate.sh

# Strict verification gate (Release mode, zero warnings, strict checks)
./scripts/verify-gate.sh --strict
```

