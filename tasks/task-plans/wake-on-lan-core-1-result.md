# Task Result: WAKE-ON-LAN-CORE-1

**Task ID**: `WAKE-ON-LAN-CORE-1`  
**Phase**: Priority 2 / P2.3 Daily Teaching Workflows & Power Actions  
**Roadmap Reference**: `REVISED_ROADMAP.md` (P2.3: "Power actions: server-side authorization, count/confirmation, offline semantics, force-close disabled by default; Wake-on-LAN only with honest support detection") and `tasks/product-execution-plan.md` (`TOOL-03/04/05`).  
**Status**: APPROVED & READY FOR MAIN INTEGRATION

---

## 1. Summary of Work

- **Network & MAC Abstraction (`common/include/nstu/wake_on_lan.hpp`, `common/src/wake_on_lan.cpp`)**:
  - Implemented `MacAddress` struct (6-byte octet container with `is_zero()`, `is_broadcast()`, and equality operators).
  - Implemented `parse_mac_address` supporting both colon (`00:11:22:33:44:55`) and hyphen (`00-11-22-33-44-55`) formats, strict case-insensitive hex parsing, rejecting invalid length, mixed separators, and non-hex characters.
  - Implemented `format_mac_address` supporting custom separators and uppercase/lowercase formats.
  - Implemented `create_magic_packet` constructing standard 102-byte Magic Packets (6 bytes of `0xFF` preamble + 16 repetitions of MAC address).
  - Implemented `verify_magic_packet` validating packet length, preamble, and repeated MAC octets.
  - Implemented `send_wake_on_lan_packet` broadcasting UDP magic packets to `255.255.255.255:9` using `SO_BROADCAST`, `WinsockRuntime`, and RAII `SocketCloser`.
  - Implemented `WolSupportState` enum (`unknown`, `supported`, `unsupported`, `missing_mac`) with `to_string` and bilingual English/Vietnamese `wol_support_label`.
- **Wire Protocol & Codecs (`common/include/nstu/protocol.hpp`, `common/include/nstu/control_messages.hpp`, `common/src/control_messages.cpp`)**:
  - Added `CommandType::wol_report = 62` (client -> server).
  - Defined `WolReportMessage` with `MacAddress mac` and `WolSupportState support`.
  - Implemented 8-byte binary codecs `encode_wol_report` and `decode_wol_report` with strict size, reserved byte, and enum bounds validation.
- **Operator Role Permissions (`server/include/nstu/operator_role.hpp`, `server/src/operator_role.cpp`)**:
  - Added `OperatorAction::wake_on_lan = 12`.
  - Permitted `Teacher`, `Technician`, and `Developer` roles to wake offline computers.
- **Client Registry (`server/include/nstu/client_registry.hpp`, `server/src/client_registry.cpp`)**:
  - Added `mac_address` and `wol_support` to `ClientRecord`.
  - Implemented `ClientRegistry::set_wol_report` for recording hardware MAC and WOL capability.
  - Updated `ClientRegistry::upsert` to preserve existing MAC address and WOL state across reconnects and status refreshes.
- **Server Control Plane (`server/include/nstu/control_plane.hpp`, `server/src/control_plane.cpp`)**:
  - Added incoming `CommandType::wol_report` handling in command dispatch and mutation rendezvous.
  - Implemented `ServerControlPlane::wake_client(client_id, caller_role, &error)`:
    - Enforces exam active protection (`exam_active(client_id)`).
    - Enforces P2.1 operator role permissions.
    - Validates client existence.
    - Rejects online clients (`"Client is already online."`).
    - Reports honest hardware support state (`"Wake-on-LAN is not supported by client hardware."`).
    - Reports honest missing MAC state (`"Cannot wake client: MAC address is unknown."`).
    - Broadcasts UDP magic packet and records remote session audit.
- **Classroom Roster & Bulk Action (`server/include/nstu/classroom_roster.hpp`, `server/src/classroom_roster.cpp`)**:
  - Added `BulkActionKind::wake_on_lan = 7`.
  - Inverted target filtering in `execute_bulk_action` so offline clients are targeted and online clients are skipped.
  - Implemented bilingual confirmation titles, confirmation dialog messages, and result status formatters.
- **Unit Test Suite (`tests/wake_on_lan_tests.cpp`, `tests/CMakeLists.txt`)**:
  - Created dedicated test executable `nstu_wake_on_lan_tests`.
  - All 8 comprehensive test cases pass 100%.

---

## 2. Verification Receipts

- **Fast Verification Gate (`./scripts/verify-gate.sh`)**:
  - Zero banned identifiers.
  - Forbidden paths not staged.
  - No weakened assertions.
  - Node/browser recovery tests: 63/63 passed.
  - Build: succeeded with 0 compiler warnings.
  - Unit tests: 66/66 passed (100%).
- **Strict Verification Gate (`./scripts/verify-gate.sh --strict`)**:
  - Release build with `-Werror` equivalent: succeeded with 0 compiler warnings.
  - Unit tests: 66/66 passed (100%).
- **Independent Code Review**:
  - Conducted by scoped subagent (ID: `729db10d-058f-4a50-a05d-849c3c287e7c`).
  - Formal verdict: **VERDICT: APPROVED**.
