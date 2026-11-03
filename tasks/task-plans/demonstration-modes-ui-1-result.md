# Task Result: DEMONSTRATION-MODES-UI-1

**Task ID**: `DEMONSTRATION-MODES-UI-1`  
**Phase**: Priority 2 / P2.3 Daily Teaching Workflows & Demonstration Modes (Client Agent Window & Teacher Ribbon UI)  
**Status**: Implemented, verified locally, ready for independent code review  

---

## 1. Summary of Changes

This task implements the user interface and presentation lifecycle for Demonstration Modes (`TOOL-08/09`):
1. **Student Agent Window Management & Input Locking (`client/agent/main.cpp`)**:
   - Declared `kBroadcastModeChangedMessage = WM_APP + 9`.
   - Maintained `g_broadcast_mode`, `g_broadcast_lock_input`, and `g_demo_input_locked` atomic states.
   - Implemented `apply_broadcast_input_lock(bool)` to cooperatively manage Win32 `BlockInput`, preventing premature input unblocking when remote control (`g_remote_control_active`) is active.
   - Handled `AgentMessageType::host_broadcast_mode` and `host_broadcast_stop` on the pipe thread and dispatched updates to the UI thread.
   - In `broadcast_window_proc`:
     - **Focus Mode**: Switched to `WS_POPUP`, `HWND_TOPMOST`, fullscreen geometry across the virtual display, and applied `BlockInput` if `lock_input` was requested.
     - **Follow-Along Mode**: Switched to `WS_OVERLAPPEDWINDOW`, `HWND_NOTOPMOST`, standard floating resizable geometry, and ensured student input remained unlocked.
     - Handled `WM_CLOSE` by calling `ShowWindow(window, SW_HIDE)` to hide the floating window without terminating the agent process.
     - Gated `restore_control_window_order()` on `g_broadcast_mode.load() == DemonstrationMode::focus` to avoid forcing topmost z-order over student applications in Follow-Along mode.
   - **Fail-Safe Disconnect & Teardown Cleanup Guarantee**:
     - Released input locks unconditionally on `server_online == 0` and pipe disconnection, and hid the broadcast window.
     - Released input locks in `WM_DESTROY` during agent exit.
2. **Teacher Console Demonstration Controls (`server/app/main.cpp`)**:
   - Integrated `broadcast_mode` and `broadcast_lock_input` into `DashboardState`.
   - Propagated active demonstration mode and input locking state live to `ServerControlPlane` via `update_teacher_broadcast_mode` and `toggle_teacher_broadcast`.
   - In `draw_room_screen_ribbon`, expanded the "Teacher screen" ribbon group:
     - Main Broadcast/Stop action button.
     - Mode switcher button with hover tooltips ("Focus Mode" vs "Follow-Along").
     - "Lock input" checkbox, disabled in Follow-Along mode via `ImGui::BeginDisabled(is_follow)` / `ImGui::EndDisabled()`.

---

## 2. File Hashes & Allowlist Compliance

| Path | Baseline SHA-256 | Modified SHA-256 | Status |
|---|---|---|---|
| `client/agent/main.cpp` | `E40D2B01002F89AF4F58EF0EDB16808FD27F8E6E86DEC84EF5BDADB1239CCF2C` | `73A90CC8B0B35B3B81BAE1CF02D436B7286433EBE1C38B4326143F17DB41127C` | Modified |
| `server/app/main.cpp` | `D5D4B6C55A7934218652EE32CF9BEC438F731C6F1469DF5A710C02DC6EC10DEE` | `D6ADBC20039AAE5BFFCD99E166A95CED3080725269A8A44ED88C506377E184C2` | Modified |

Strictly 2 files modified ($\le 5$ files limit respected).

---

## 3. Verification Receipts

### Fast Verification Gate (`./scripts/verify-gate.sh`)
- Zero compiler warnings.
- 67 of 67 test suites passed (100%).
- 63 Node.js browser recovery test cases passed.
- All repository rules passed (no banned vendor identifiers, forbidden files uncommitted, zero assertion weakening).

### Strict Verification Gate (`./scripts/verify-gate.sh --strict`)
- Release configuration with `-Werror` and strict compiler warnings (`NSTU_ENABLE_WERROR=ON`).
- Zero compiler warnings.
- 67 of 67 test suites passed (100%).

---

## 4. Invariant & Safety Checklist

- [x] Fail-safe release of `BlockInput` on broadcast stop, disconnect, and agent exit.
- [x] Safe coexistence between remote control and demonstration input locking.
- [x] Focus mode fullscreen topmost presentation vs Follow-Along resizable floating window.
- [x] Follow-Along close button hides window without process termination.
- [x] Input lock disabled in Follow-Along mode.
- [x] Zero assertion removals across all 67 test suites.
- [x] No banned vendor identifiers introduced.

