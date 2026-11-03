# Task Plan: WAKE-ON-LAN-UI-1

**Task ID**: `WAKE-ON-LAN-UI-1`  
**Phase**: Priority 2 / P2.3 Daily Teaching Workflows & Power Actions  
**Roadmap Reference**: `REVISED_ROADMAP.md` (P2.3: "Power actions: server-side authorization, count/confirmation, offline semantics, force-close disabled by default; Wake-on-LAN only with honest support detection") and `tasks/product-execution-plan.md` (`TOOL-03/04/05`).

---

## 1. Description & Architectural Rationale

In `WAKE-ON-LAN-CORE-1`, we established the protocol foundations: MAC address abstraction, AMD 102-byte magic packet generation/verification, UDP broadcast socket implementation, `CommandType::wol_report = 62` binary codec, `WolSupportState` enum, `OperatorAction::wake_on_lan` role gating, `ClientRegistry` state retention, `ServerControlPlane::wake_client` with exam and honest support guards, and `ClassroomRoster` bulk wake actions.

This task (`WAKE-ON-LAN-UI-1`) completes the vertical slice by delivering:
1. **Client Service Hardware Detection (`common/include/nstu/wake_on_lan.hpp`, `common/src/wake_on_lan.cpp`, `client/service/main.cpp`)**:
   - `detect_local_hardware_wol()`: Queries Windows IP Helper API (`GetAdaptersAddresses`) to identify operational network adapters (`IfOperStatusUp`), retrieves physical 6-byte MAC address, and reports honest hardware support state (`supported`, `unsupported`, or `missing_mac`).
   - In `client/service/main.cpp`, detects local hardware information and dispatches `CommandType::wol_report = 62` to the server over the authenticated control channel.
2. **Teacher Console UI (`server/app/main.cpp`)**:
   - **Workspace Toolbar Bulk Action**: Adds "Wake" / "Bật máy" button in `draw_workspace_toolbar` to initiate bulk wake for selected offline machines.
   - **Confirmation Modal**: In `draw_bulk_action_confirmation_popup`, handles `BulkActionKind::wake_on_lan`, displaying target count of offline computers.
   - **Single-Client Detail Panel**: In `draw_client_detail`, displays MAC address and status badge (`● Supported` / `▲ Unsupported` / `○ Missing MAC`). When a client is offline, provides "Wake via LAN" / "Bật máy qua mạng" button, disabled with hover tooltips if MAC is unknown, WOL is unsupported, or an exam is active. When client is online, button is disabled with "Client is already online" tooltip.
3. **Verification**: Fast and strict verification gates passing 100% with zero compiler warnings.

---

## 2. Acceptance Criteria

1. **Local Hardware Detection (`common/include/nstu/wake_on_lan.hpp`, `common/src/wake_on_lan.cpp`)**:
   - `struct LocalHardwareInfo` containing `MacAddress mac` and `WolSupportState wol_support`.
   - `detect_local_hardware_wol()`:
     - On Windows, uses `GetAdaptersAddresses` (AF_INET, skipping anycast/multicast/dns).
     - Loops over adapters, selecting active physical adapters (`OperStatus == IfOperStatusUp`, `IfType != IF_TYPE_SOFTWARE_LOOPBACK` and `IfType != IF_TYPE_TUNNEL`).
     - Extracts 6-byte physical MAC address from `adapter->PhysicalAddress`.
     - Validates non-zero MAC. If Ethernet (`IF_TYPE_ETHERNET_CSMACD = 6`) or Wi-Fi (`IF_TYPE_IEEE80211 = 71`), sets `WolSupportState::supported`; otherwise `unsupported` or `missing_mac`.
     - Returns populated `LocalHardwareInfo`.
2. **Client Service Integration (`client/service/main.cpp`)**:
   - Upon successful server authentication (`authenticated_session_`), invokes `detect_local_hardware_wol()`.
   - If a valid hardware report is obtained, encodes `WolReportMessage` and dispatches `CommandType::wol_report = 62` to the server.
3. **Teacher Console UI (`server/app/main.cpp`)**:
   - **Toolbar Bulk Button**: "Wake" / "Bật máy" button in `draw_workspace_toolbar` alongside Reboot and Shutdown, opening confirmation modal for `BulkActionKind::wake_on_lan`.
   - **Confirmation Modal**: Properly renders `BulkActionKind::wake_on_lan` with bilingual title and formatted message detailing offline target count.
   - **Single-Client Detail View**:
     - Displays formatted MAC address (or "Unknown MAC" / "Thiếu địa chỉ MAC").
     - Displays WOL hardware support badge with distinct glyphs (`● Supported`, `▲ Unsupported`, `○ Missing MAC`).
     - Renders "Wake via LAN" / "Bật máy qua mạng" button for offline clients.
     - Controls properly disabled with bilingual hover tooltips when:
       - Client is online ("Client is already online" / "Máy tính đang hoạt động").
       - Exam is active on client ("Cannot wake: exam active" / "Không thể bật máy: đang thi").
       - MAC address is missing ("Missing MAC address" / "Thiếu địa chỉ MAC").
       - Hardware does not support WOL ("Hardware does not support Wake-on-LAN" / "Phần cứng không hỗ trợ").
4. **Verification**:
   - Local verification gates (`./scripts/verify-gate.sh` and `./scripts/verify-gate.sh --strict`) pass 100% with zero compiler warnings.
   - Dedicated unit tests verify `detect_local_hardware_wol()` execution.

---

## 3. File Allowlist & Baseline SHA-256 Hashes

| Path | Baseline SHA-256 | Description |
|---|---|---|
| `common/include/nstu/wake_on_lan.hpp` | `63fdf57171834d449b5dac0fc2f9977a951b640c616ac0bdb7d9da2b5db6eb21` | Declare `LocalHardwareInfo` and `detect_local_hardware_wol()` |
| `common/src/wake_on_lan.cpp` | `e4d5e5db13aaf9bfd4b40bf05e64b446e3fad0dece13e1669099d4f4aa620259` | Implement `detect_local_hardware_wol()` using `GetAdaptersAddresses` |
| `client/service/main.cpp` | `59a5e44ce518960457c9d256c9e81125cc329ee10de3511619b1a6aae5d9d54b` | Probe local network adapter and dispatch `wol_report` to server |
| `server/app/main.cpp` | `235bb879254ee8f686cf367e136059c33ccde9c1e84d28ce0b143ceb140bf99c` | Dear ImGui UI: toolbar bulk button, client detail badges, single wake |
| `tests/wake_on_lan_tests.cpp` | `f9674bbcd597084d01326f0c36c3fa5016b85e7afc914a034d2943707b934bf5` | Unit test coverage for local hardware probe |

---

## 4. Dependencies & Non-Negotiable Invariants

- **Author attribution**: Must commit exclusively as `khoasoma <songtu438@gmail.com>` with no AI assistant trailers.
- **Preserved dirty paths**: Never stage, modify, or revert `README.md`, `README.vi.md`, `docs/assets/banner.png`.
- **Private files**: Never stage or commit `PRODUCT_ROADMAP_NSTU.md`, `artifacts/`, `.claude/`, or `HANDOFF.md`.
- **Preserve all assertions**: Every existing unit test and assertion across all 66 test suites must pass without modification.
- **Honest support detection**: Fails honestly and displays clear diagnostic badges when MAC is missing or hardware is unsupported.
- **Exam protection**: Cannot wake or disrupt any client marked active in an assessment.

---

## 5. Verification Commands

```bash
# Focused test execution
cmake --build build/mingw --target nstu_wake_on_lan_tests
build/mingw/tests/nstu_wake_on_lan_tests.exe

# Fast verification gate (zero warnings, all 66 unit tests pass)
./scripts/verify-gate.sh

# Strict verification gate (Release mode, zero warnings, strict checks)
./scripts/verify-gate.sh --strict
```

