# Task Plan: DEMONSTRATION-MODES-UI-1

**Task ID**: `DEMONSTRATION-MODES-UI-1`  
**Phase**: Priority 2 / P2.3 Daily Teaching Workflows & Demonstration Modes (Client Agent Window & Teacher Ribbon UI)  
**Roadmap Reference**: `REVISED_ROADMAP.md` (P2.3: "Demonstration modes: Focus and Follow-Along are distinct; snapshot wall remains low-bandwidth. Validate stream resource release and disconnect cleanup.") and `tasks/product-execution-plan.md` (`TOOL-08/09`: "Reuse Focus broadcast with resource/input-lock cleanup, then separate resizable Follow-Along. Keep the class wall snapshot-first.").

---

## 1. Description & Architectural Rationale

This task implements the user interface and presentation lifecycle for Demonstration Modes (`TOOL-08/09`):
1. **Student Agent Window Management & Input Locking (`client/agent/main.cpp`)**:
   - `g_broadcast_mode`, `g_broadcast_lock_input`, and `g_demo_input_locked` tracking active demonstration state.
   - `apply_broadcast_input_lock(bool)` safely applies or releases `BlockInput`, co-existing safely with remote control.
   - In `service_pipe_thread`:
     - Decode `AgentMessageType::host_broadcast_mode` and post `kBroadcastModeChangedMessage` to the UI thread.
     - Handle `AgentMessageType::host_broadcast_stop` releasing input lock and hiding window.
   - In `broadcast_window_proc`:
     - **Focus Mode**: `WS_POPUP`, `HWND_TOPMOST`, fullscreen geometry. If `lock_input` is enabled, locks student input while viewing.
     - **Follow-Along Mode**: `WS_OVERLAPPEDWINDOW`, non-topmost, resizable floating window with titlebar. Student input is never locked.
     - `WM_CLOSE`: closes/hides the floating window without terminating agent.
     - `restore_control_window_order()` gated so non-focus mode does not force topmost z-order over student applications.
   - **Disconnect & Teardown Cleanup Guarantee**:
     - When `server_online == 0` or the pipe disconnects, input locks are unconditionally released and the broadcast window is hidden.
     - On agent shutdown (`WM_DESTROY`), input locks are released.
2. **Teacher Console Demonstration Controls (`server/app/main.cpp`)**:
   - In `DashboardState`, maintain `broadcast_mode` (default `DemonstrationMode::focus`) and `broadcast_lock_input` (default `true`).
   - In `toggle_teacher_broadcast`, dispatch active `broadcast_mode` and `broadcast_lock_input` to `control_plane.set_broadcast_mode`.
   - In `update_teacher_broadcast_mode`, update local state and propagate changes live to the control plane if broadcast is running.
   - In `draw_room_screen_ribbon`, expand the "Teacher screen" ribbon group:
     - Main Broadcast/Stop action button.
     - Mode toggle button ("Mode: Focus" vs "Mode: Follow-Along").
     - "Lock input" checkbox (disabled when in Follow-Along mode).

---

## 2. Acceptance Criteria

1. **Focus Mode Presentation**: Fullscreen topmost borderless popup window covering the virtual screen; applies `BlockInput` if `lock_input` is set.
2. **Follow-Along Mode Presentation**: Resizable floating window with title bar and standard window borders; student input is never locked.
3. **Fail-Safe Resource & Input Lock Cleanup**: Disconnecting from server, stopping broadcast, or agent termination unconditionally releases `BlockInput`.
4. **Teacher Console Ribbon Integration**: Teacher can switch modes and toggle input locking before or during active broadcast.
5. **Dual Verification Gates**: Zero compiler warnings, 67 of 67 test suites pass cleanly under `./scripts/verify-gate.sh` and `./scripts/verify-gate.sh --strict`.

---

## 3. File Allowlist & Baseline SHA-256 Hashes

| Path | Baseline SHA-256 | Description |
|---|---|---|
| `client/agent/main.cpp` | `E40D2B01002F89AF4F58EF0EDB16808FD27F8E6E86DEC84EF5BDADB1239CCF2C` | Window styling, input locking, mode transition, disconnect cleanup |
| `server/app/main.cpp` | `D5D4B6C55A7934218652EE32CF9BEC438F731C6F1469DF5A710C02DC6EC10DEE` | DashboardState fields, mode update helpers, ribbon UI controls |

Strictly 2 files modified ($\le 5$ files limit respected).

---

## 4. Dependencies & Non-Negotiable Invariants

- **Author attribution**: Must commit exclusively as `khoasoma <songtu438@gmail.com>` with no AI assistant trailers.
- **Preserved dirty paths**: Never stage, modify, or revert `README.md`, `README.vi.md`, `docs/assets/banner.png`.
- **Private files**: Never stage or commit `PRODUCT_ROADMAP_NSTU.md`, `artifacts/`, `.claude/`, or `HANDOFF.md`.
- **Preserve all assertions**: Every existing assertion in all 67 test suites must be preserved.
- **Snapshot wall isolation**: Snapshot wall polling remains low-bandwidth and unaffected by demonstration modes.

---

## 5. Verification Commands

```bash
# Fast verification gate (zero warnings, all 67 test suites pass)
./scripts/verify-gate.sh

# Strict verification gate (Release mode, -Werror, zero warnings)
./scripts/verify-gate.sh --strict
```
