# Task Plan: POWER-ACTIONS-UI-1

## 1. Objective & Scope
This task implements the client service Windows shutdown/reboot execution and teacher console UI controls for **Power Actions** per `REVISED_ROADMAP.md` (Priority 2 / P2.3: "Power actions: server-side authorization, count/confirmation, offline semantics, force-close disabled by default; Wake-on-LAN only with honest support detection") and P2.1 role separation:

1. **Working Tree Baseline Hygiene**:
   - Verify working tree integrity against committed baseline `825c9a7` prior to implementation (`git status` clean).
   - Ensure clean baseline hashes for all allowlisted files.

2. **Client Service Power Dispatch (`client/service/main.cpp`)**:
   - Handle inbound wire command `CommandType::power_action = 61` in `on_authenticated_command`.
   - **Exam Lockdown Containment**: If `g_exam_lockdown_controller.is_armed()`, reject/drop the power action command, outputting debug diagnostics (`"NSTU power action rejected: exam lockdown is armed\n"`) and protecting ongoing student assessments from disruption.
   - Decode payload with `decode_power_action`. Drop if malformed (`"NSTU invalid power action command payload\n"`).
   - Implement `execute_power_action(const nstu::control::PowerActionMessage& message) noexcept`:
     - Dynamically acquire `SeShutdownPrivilege` on the service process token via `OpenProcessToken` (`TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY`), `LookupPrivilegeValueW`, and `AdjustTokenPrivileges`.
     - Execute system power action via Windows API `InitiateSystemShutdownExW`.
     - **Graceful Mode (`force == false`)**:
       - 5-second countdown timeout (`dwTimeout = 5`).
       - `bForceApps = FALSE`, allowing running applications to save work and prompt the user.
       - Message string: `lpMessage = const_cast<wchar_t*>(L"NSTU remote power action requested. Please save open work.")`.
     - **Forced Mode (`force == true`)**:
       - 0-second immediate timeout (`dwTimeout = 0`).
       - `bForceApps = TRUE`, forcefully closing hung or lingering processes.
       - Message string: `lpMessage = nullptr`.
     - **Action Mapping**:
       - Reboot: `bRebootAfterShutdown = TRUE`.
       - Shutdown: `bRebootAfterShutdown = FALSE`.
     - **Shutdown Reason Codes**:
       - `SHTDN_REASON_MAJOR_OPERATINGSYSTEM | SHTDN_REASON_MINOR_RECONFIG | SHTDN_REASON_FLAG_PLANNED`.
     - Restore previous token privileges if applicable and close token handle via `CloseHandle`.

3. **Server Console UI & Confirmation UX (`server/app/main.cpp`)**:
   - **Bulk Actions Toolbar**:
     - In the roster toolbar (`!state.checked_client_ids.empty()`), adjacent to "Launch Web", add:
       - "Reboot" / "Khởi động lại" (`BulkActionKind::reboot`)
       - "Shutdown" / "Tắt máy" (`BulkActionKind::shutdown`)
     - Clicking either button sets `state.pending_bulk_action` and sets `state.bulk_confirmation_open = true`.
   - **Single-Client Action Panel**:
     - In the selected client detail view (`draw_client_detail`), adjacent to "Launch Web", add "Reboot" / "Khởi động lại" and "Shutdown" / "Tắt máy" buttons targeting `selected_client->id`.
     - Active assessment guard: Check `control_plane.exam_active(selected_client->id)`. If true, wrap buttons in `ImGui::BeginDisabled()` and display hover tooltip:
       - EN: `"Power actions are disabled during active assessments."`
       - VI: `"Không thể thao tác nguồn khi đang có bài thi."`
   - **Confirmation Modal & Role Gating (P2.1 Enforced)**:
     - In `draw_bulk_confirmation_modal`:
       - Check `is_action_permitted(state.current_role, ...)` for `BulkActionKind::reboot` (`OperatorAction::reboot_client`) and `BulkActionKind::shutdown` (`OperatorAction::shutdown_client`).
       - If caller is in `OperatorRole::teacher`:
         - Render colored warning text banner:
           - EN: `"Permission Denied: Power actions require Technician or Developer role."`
           - VI: `"Từ chối quyền: Thao tác nguồn yêu cầu quyền Kỹ thuật viên hoặc Nhà phát triển."`
         - Disable the "Confirm & Execute" button via `can_execute = has_online_targets && url_valid && power_role_authorized`.
       - Pass `state.current_role` to `execute_bulk_action`.
       - Render formatted results to `state.control_status` via `format_bulk_action_result_status`.

4. **Verification & Gates**:
   - Fast gate (`verify-gate.sh`) and strict gate (`verify-gate.sh --strict`) pass 100% with zero compiler warnings and all 65/65 tests passing.
   - Independent non-author code review subagent verification.

---

## 2. File Allowlist & Source Baselines (SHA-256)
| File | Baseline SHA-256 | Post-Implementation SHA-256 |
|---|---|---|
| `client/service/main.cpp` | `d39812ca3da773af1953ec08c67a85135234557cd0350275888ebeed698d201b` | `59a5e44ce518960457c9d256c9e81125cc329ee10de3511619b1a6aae5d9d54b` |
| `server/app/main.cpp` | `f23063067c2d5e11ac2ec144916d748019fb422f2ba4ffcf82508e439bc60e5c` | `235bb879254ee8f686cf367e136059c33ccde9c1e84d28ce0b143ceb140bf99c` |
| `tasks/task-plans/power-actions-ui-1.md` | Tracked | Tracked |

---

## 3. Acceptance Criteria
1. `client/service/main.cpp` handles `CommandType::power_action = 61`, rejects dispatch when exam lockdown is armed, decodes `PowerActionMessage`, and initiates graceful or forced shutdown/reboot via `InitiateSystemShutdownExW` using `SeShutdownPrivilege` with explicit message parameters.
2. `server/app/main.cpp` provides toolbar bulk buttons for Reboot and Shutdown.
3. `server/app/main.cpp` provides single-client action buttons for Reboot and Shutdown in client detail panel, disarmed with hover tooltips when the client is in an active exam.
4. The confirmation modal displays bilingual title, room name, target count, unsaved work warning, and enforces P2.1 role gating (disallowing confirmation with explanatory notice if caller is Teacher).
5. `execute_bulk_action` is called passing `state.current_role`.
6. Zero compiler warnings under strict C++20 `-Wall -Wextra -Werror` and MSVC `/W4 /WX`.
7. All 65/65 unit and integration tests pass cleanly in local verification gates (`./scripts/verify-gate.sh` and `./scripts/verify-gate.sh --strict`).

---

## 4. Test & Verification Plan
- Build target: `cmake --build build/mingw --target nstu-service nstu-server`.
- Run fast gate: `./scripts/verify-gate.sh`.
- Run strict gate: `./scripts/verify-gate.sh --strict`.
- Independent non-author code review subagent verification.
