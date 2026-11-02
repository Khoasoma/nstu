# Task Plan: Operator Surfaces & Role Separation (Teacher / Technician / Developer)

- **Task Identifier**: `OPERATOR-SURFACES-ROLE-SEPARATION-1`
- **Stream**: Roadmap P2.1 (Teacher / Technician / Developer surfaces)
- **Component**: `server`, `tests`, `ui`
- **Allowlisted Files**:
  - `server/include/nstu/operator_role.hpp`
  - `server/src/operator_role.cpp`
  - `server/CMakeLists.txt`
  - `server/app/main.cpp`
  - `tests/operator_role_tests.cpp`
  - `tests/CMakeLists.txt`
  - `tasks/task-plans/operator-surfaces-role-separation-1.md`

---

## 1. Context & Motivation

Per `REVISED_ROADMAP.md` (P2.1):
> *"Teacher default shows class state and common actions only.*
> *Technician mode exposes inventory, service/agent/UWF/network health, deployment, diagnostics and recovery.*
> *Developer diagnostics remain behind Advanced; do not delete capabilities when simplifying Teacher mode.*
> *Define permission checks server-side for disruptive actions."*

### Current Baseline & Gaps:
- The teacher console exposes technical diagnostics, raw D3D11 reports, and disruptive reboot-to-restore (UWF) configuration buttons directly in the standard teacher views without clear role demarcation.
- Teachers managing classes should have a focused, clean interface without the clutter of low-level graphics diagnostic modals or accidental triggers of disk overlay servicing.
- IT technicians maintaining the classroom fleet need dedicated visibility into fleet health (inventory, agent versions, network latency/loss, UWF protection phases) and recovery actions.
- Developers debugging internal pipelines need access to raw telemetry reports, D3D11 device queries, and advanced debugging tools.

---

## 2. Technical Design

### A. Pure Domain Role & Permission Model (`nstu::server::OperatorRole`)
In `server/include/nstu/operator_role.hpp` and `server/src/operator_role.cpp`:
1. **Operator Roles**:
   ```cpp
   enum class OperatorRole : int {
       teacher = 0,
       technician = 1,
       developer = 2,
   };
   ```
2. **Operator Actions**:
   ```cpp
   enum class OperatorAction : int {
       view_classroom = 0,
       toggle_lock = 1,
       broadcast = 2,
       chat = 3,
       manage_exam = 4,
       configure_uwf = 5,
       reboot_client = 6,
       view_fleet_health = 7,
       view_diagnostics_raw = 8,
       export_telemetry_report = 9,
       manage_settings = 10,
   };
   ```
3. **Authorization Evaluation**:
   - `[[nodiscard]] bool is_action_permitted(OperatorRole role, OperatorAction action) noexcept;`
     - `teacher`: permitted standard classroom workflows (`view_classroom`, `toggle_lock`, `broadcast`, `chat`, `manage_exam`, `manage_settings`). Disruptive maintenance (`configure_uwf`, `reboot_client`, `view_fleet_health`, `view_diagnostics_raw`, `export_telemetry_report`) is restricted.
     - `technician`: permitted all teacher actions plus fleet inventory & health (`view_fleet_health`), UWF management (`configure_uwf`, `reboot_client`), and diagnostic summaries.
     - `developer`: permitted all actions including raw D3D11 graphics diagnostics, raw telemetry markdown preview, and internal traces.
4. **Localization & Serialization**:
   - `[[nodiscard]] const char* to_string(OperatorRole role) noexcept;`
   - `[[nodiscard]] const char* role_label(OperatorRole role, bool vietnamese) noexcept;`

### B. Teacher Console UI Integration (`server/app/main.cpp`)
1. **Role Switcher in Menu Strip**:
   - In `draw_menu_strip`, add an operator surface segment / dropdown: `Teacher` | `Technician` | `Developer (Advanced)`.
   - Switching roles updates `DashboardState::current_role`.
2. **Teacher Mode Default**:
   - Clean, uncluttered UI showing class state and common actions only.
   - Guard disruptive UWF actions in `draw_selected_client`: when in Teacher mode, displays an informative badge/tooltip instead of disruptive arming triggers.
3. **Technician Mode**:
   - Adds a dedicated "Fleet Health" tab/surface (`draw_technician_fleet_health`) showing computer inventory, status, UWF phase (`proves_current_protection()`, configuring, awaiting restart, unsupported), latency, loss, and maintenance controls.
4. **Developer Mode**:
   - Exposes raw graphics diagnostics popup, public telemetry markdown report preview, and debug logs behind an "Advanced Diagnostics" surface.

---

## 3. Baseline Source Hashes

Before modification:
- `server/CMakeLists.txt`: `D9A2E0AA887FD97D78EBDD68C852065D9C73D8C4B3AE2718141E71B4D39FE423`
- `server/app/main.cpp`: `77E378A30ED1922CD47AB192CF063BE2677DACECB306700BE52FDF9B970FD3F6`
- `tests/CMakeLists.txt`: `6C263773ABC4488C302B95B3B9F6D6B54BD6A0BEA4D3770350B6EB5F86D1D211`

---

## 4. Acceptance Criteria

1. [ ] Pure operator role and permission types defined in `server/include/nstu/operator_role.hpp` and `server/src/operator_role.cpp`.
2. [ ] `server/CMakeLists.txt` compiles `src/operator_role.cpp` into `nstu_server_state`.
3. [ ] `DashboardState` supports `OperatorRole current_role = OperatorRole::teacher`.
4. [ ] `draw_menu_strip` provides role switching between Teacher, Technician, and Developer modes.
5. [ ] Teacher mode hides or guards disruptive operations; Technician mode surfaces fleet health; Developer mode retains raw diagnostics.
6. [ ] `tests/operator_role_tests.cpp` verifies 100% of permission matrix combinations, label localizations, and role transitions.
7. [ ] Local `./scripts/verify-gate.sh` and `./scripts/verify-gate.sh --strict` pass with 0 warnings.
8. [ ] Windows CI passes all jobs on feature branch and upon fast-forward merge to `main`.

