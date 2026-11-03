# Task Result: WAKE-ON-LAN-UI-1

**Task ID**: `WAKE-ON-LAN-UI-1`  
**Phase**: Priority 2 / P2.3 Daily Teaching Workflows & Power Actions  
**Roadmap Reference**: `REVISED_ROADMAP.md` (P2.3: "Power actions: server-side authorization, count/confirmation, offline semantics, force-close disabled by default; Wake-on-LAN only with honest support detection") and `tasks/product-execution-plan.md` (`TOOL-03/04/05`).

---

## 1. Implementation Summary

1. **Hardware Detection & Client Service Integration (`common/include/nstu/wake_on_lan.hpp`, `common/src/wake_on_lan.cpp`, `client/service/main.cpp`)**:
   - Implemented `LocalHardwareInfo detect_local_hardware_wol() noexcept` using Windows IP Helper API (`GetAdaptersAddresses`).
   - Filters for active physical adapters (`OperStatus == IfOperStatusUp`, ignoring loopback and tunnel interfaces).
   - Validates 6-byte non-zero MAC address, marking Ethernet (`IF_TYPE_ETHERNET_CSMACD = 6`) or Wi-Fi (`IF_TYPE_IEEE80211 = 71`) as `WolSupportState::supported`, and other types as `unsupported`. If no adapter is found, returns `WolSupportState::missing_mac`.
   - In `client/service/main.cpp`, upon session authentication, detects hardware and dispatches `CommandType::wol_report = 62` via `queue_outbound_message`.
2. **Teacher Console UI (`server/app/main.cpp`)**:
   - **Workspace Toolbar Bulk Action**: Added "Wake" / "Bật máy" button in `draw_workspace_toolbar` to initiate bulk wake for selected offline machines.
   - **Bulk Confirmation Modal**: In `draw_bulk_action_confirmation_popup`, handled `BulkActionKind::wake_on_lan`. Correctly accounts for offline computers as targets and skips already-awake online computers, enforcing `OperatorAction::wake_on_lan` role authorization.
   - **Single-Client Detail Panel**: In `draw_client_detail`, displays MAC address and status badge (`● Supported` / `▲ Unsupported` / `○ Missing MAC`).
   - Added single-client "Wake" / "Bật máy" button disabled with hover tooltips when:
     - Client is online ("Client is already online" / "Máy tính đang hoạt động").
     - Exam is active on client ("Cannot wake: exam active" / "Không thể bật máy: đang thi").
     - MAC is unknown ("Cannot wake client: MAC address is unknown" / "Thiếu địa chỉ MAC").
     - Hardware does not support WOL ("Wake-on-LAN is not supported by client hardware" / "Phần cứng không hỗ trợ").
3. **Unit Tests (`tests/wake_on_lan_tests.cpp`)**:
   - Added `test_detect_local_hardware_wol()` verifying stability, memory safety, and non-zero MAC contract for supported hardware.

---

## 2. File Allowlist & Hashes

| Path | Pre-Implementation SHA-256 | Post-Implementation SHA-256 |
|---|---|---|
| `common/include/nstu/wake_on_lan.hpp` | `63fdf57171834d449b5dac0fc2f9977a951b640c616ac0bdb7d9da2b5db6eb21` | `F6BF197FD515A5AF623A888F41865A1E943452AD6A1C3686F8F9460524AFA155` |
| `common/src/wake_on_lan.cpp` | `e4d5e5db13aaf9bfd4b40bf05e64b446e3fad0dece13e1669099d4f4aa620259` | `327525ADC2A79E57A1FD0B924AF127442188E380DAEEAADBCA1D407093E56449` |
| `client/service/main.cpp` | `59a5e44ce518960457c9d256c9e81125cc329ee10de3511619b1a6aae5d9d54b` | `09E7A7439F6E04A18FAD7ED80EAE6B5F892E1D34DD7D875FFB7BA6972A1A8CE2` |
| `server/app/main.cpp` | `235bb879254ee8f686cf367e136059c33ccde9c1e84d28ce0b143ceb140bf99c` | `CC908420485D3B4AE754FF726E5CE9CE845898E2AD896A9D94ADCABE843AAA4C` |
| `tests/wake_on_lan_tests.cpp` | `f9674bbcd597084d01326f0c36c3fa5016b85e7afc914a034d2943707b934bf5` | `DE38D0B8CD72894AD956C629D7A7081CCD2111CBED62504F5C824974506D73E0` |

---

## 3. Verification Receipts

- Fast Gate: `./scripts/verify-gate.sh` -> PASS (all 66/66 test suites passed, zero compiler warnings).
- Strict Gate: `./scripts/verify-gate.sh --strict` -> PASS (Release + `-Werror`, all 66/66 test suites passed, zero compiler warnings).
- Clean working directory outside allowlisted files. No assertions removed or weakened.

