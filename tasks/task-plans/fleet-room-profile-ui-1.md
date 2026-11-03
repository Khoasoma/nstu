# Task Plan: FLEET-ROOM-PROFILE-UI-1

**Task ID**: `FLEET-ROOM-PROFILE-UI-1`  
**Phase**: Priority 3 / P3.1 Fleet Health & Privacy Gates (Room Profile UI Modal, File/Clipboard Export, Validated Import & Live Roster Integration)  
**Roadmap Reference**: `REVISED_ROADMAP.md` (P3.1 and Privacy Gates: "Generic support bundles exclude credentials, enrollment secrets, exam answers... Portable secret-free room profiles separately from previewed, consented, canary-tested support sanitization/export.") and `tasks/product-execution-plan.md` (`FLEET-05`: "Portable secret-free room profiles separately from previewed, consented, canary-tested support sanitization/export.").

---

## 1. Description & Architectural Rationale

This task implements the user interface and server state integration for portable secret-free room profiles, building upon the core data model and validation engine established in `FLEET-ROOM-PROFILE-CORE-1`:

1. **Dashboard State Extension (`server/app/main.cpp`)**:
   - `room_profile_modal_open`: Boolean modal display state.
   - `room_profile_modal_just_opened`: One-shot modal opening trigger to ensure `ImGui::OpenPopup` is called once and immediately disarmed, preserving Escape dismissal.
   - `room_profile_export_text`: Cached canonical JSON representation of the current classroom profile.
   - `room_profile_import_buffer`: Text buffer ($\le 64$ KiB) for importing / pasting external room profile JSON.
   - `room_profile_status_message` and `room_profile_status_is_error`: Feedback banner state for validation results.

2. **Room Profile Modal (`draw_room_profile_modal` in `server/app/main.cpp`)**:
   - **Export Current Profile Section**:
     - Displays summary metrics: Room Name, Seat count (from active `ClientRecord` list), Web Presets count.
     - Scrollable read-only text preview of exported JSON.
     - Action buttons: "Copy to Clipboard" (via `ImGui::SetClipboardText`) and "Save to Documents" (writing to `%USERPROFILE%\Documents\nstu-room-profile-<name>.json` using Win32 `GetEnvironmentVariableA` to avoid CRT security deprecations).
   - **Import & Apply Profile Section**:
     - Multiline text input for pasting profile JSON.
     - "Validate & Apply Profile" button:
       - Runs `nstu::fleet::import_room_profile_json`.
       - Rejects immediately if secret canary trips (`forbidden_secret_detected`) or validation fails.
       - On success: updates `state.room_name_input`, calls `control_plane.set_server_name(...)`, persists to `state.room_name_path` (`server-room-name.txt`) if available, and sets green success status banner.

3. **Technician Surface Wiring (`draw_technician_fleet_health` in `server/app/main.cpp`)**:
   - Adds `[Room Profile...]` button in the fleet health toolbar beside `[Export Support Bundle...]`.
   - Tooltip: "Export current classroom layout and presets, or import a secret-free room configuration."
   - Strictly role-gated with `is_action_permitted(state.current_role, OperatorAction::view_fleet_health)` (accessible to Technician and Developer; hidden/disabled for Teacher).

4. **Server State Unit Test Suite (`tests/server_state_tests.cpp`)**:
   - Validates live room profile construction from `DashboardState` and `ClientRecord` inventory.
   - Tests export formatting and clipboard/file payload fidelity.
   - Tests fail-closed canary rejection when importing malicious JSON containing secrets.
   - Tests room name application and control plane synchronization upon successful import.
   - Tests operator role permission boundaries.

---

## 2. Acceptance Criteria

1. **Surface Integration**: `[Room Profile...]` button is visible and active for Technician and Developer roles, and hidden/disabled for Teachers in `draw_technician_fleet_health`.
2. **Export Usability**: Exports current room configuration (name, seats, presets) cleanly; provides copy-to-clipboard and save-to-file options with proper feedback.
3. **Fail-Closed Import Protection**: Rejects payloads containing secret keywords (`key`, `secret`, `token`, `password`, `private`, `entropy`, `ticket`, `student_answer`, etc.) with clear, prominent error messaging.
4. **Live Synchronization**: Applying a valid profile immediately updates the server name in `DashboardState`, persists to disk if configured, and updates `control_plane.server_name()`.
5. **Preserve All Assertions**: All existing assertions across all 69 test suites remain 100% intact.
6. **Dual Verification Gates**: Zero compiler warnings, 69 of 69 test suites pass cleanly under `./scripts/verify-gate.sh` and `./scripts/verify-gate.sh --strict`.

---

## 3. File Allowlist & Baseline SHA-256 Hashes

| Path | Baseline SHA-256 | Description |
|---|---|---|
| `server/app/main.cpp` | `999072EC1380FB8DD6F07F95F52C1DFFF23D4801E661515045D59587C8731CF8` | Room profile modal, export/import UI, technician toolbar button |
| `tests/server_state_tests.cpp` | `E04CCF03BF6D72C1A26B815C26E290E4F3C58DAF4C61429A28FC20375C01A301` | Server state unit tests for room profile export/import integration |

Strictly 2 files modified ($\le 5$ files limit respected).

---

## 4. Dependencies & Non-Negotiable Invariants

- **Author attribution**: Must commit exclusively as `khoasoma <songtu438@gmail.com>` with no AI assistant trailers.
- **Preserved dirty paths**: Never stage, modify, or revert `README.md`, `README.vi.md`, `docs/assets/banner.png`.
- **Private files**: Never stage or commit `PRODUCT_ROADMAP_NSTU.md`, `artifacts/`, `.claude/`, or `HANDOFF.md`.
- **Zero compiler warnings**: Must compile cleanly under MSVC `/W4 /WX` and GCC `-Wall -Wextra -Wpedantic -Werror`.
- **Secret-Free Invariant**: Zero credentials, tokens, keys, entropy, or exam answers in room profiles.

---

## 5. Verification Commands

```bash
# Fast gate
./scripts/verify-gate.sh

# Strict gate
./scripts/verify-gate.sh --strict
```

