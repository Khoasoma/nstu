# Task Plan: Classroom List View & Safe Bulk Actions

- **Task Identifier**: `CLASSROOM-LIST-AND-BULK-ACTIONS-1`
- **Stream**: Roadmap P2.2 (Classroom/List view and safe bulk actions) / Order 10
- **Component**: `server`, `tests`, `ui`
- **Allowlisted Files**:
  - `server/include/nstu/classroom_roster.hpp`
  - `server/src/classroom_roster.cpp`
  - `server/CMakeLists.txt`
  - `server/app/main.cpp`
  - `tests/classroom_roster_tests.cpp`
  - `tests/CMakeLists.txt`
  - `tasks/task-plans/classroom-list-and-bulk-actions-1.md`

---

## 1. Context & Motivation

Per `REVISED_ROADMAP.md` (P2.2 / Order 10):
> *"Improve roster/status, search/filter, clear selection, online/offline distinction and bulk selection.*
> *Confirmation states target count, room and action; progress, failures and retry are visible.*
> *Benchmark 10, 30 and 50 clients; test scroll/input responsiveness and thumbnail load behavior."*

### Current Baseline:
- The teacher console currently only provides a thumbnail screen wall (`draw_room_screen_wall`) in the main classroom workspace.
- In larger classrooms (30-50 machines), screen thumbnails consume substantial screen space and GPU memory when teachers simply want to audit roster connectivity, UWF protection status, network latency/loss, or issue actions to a selected cohort of machines.
- Room-wide lock (`set_room_lock`) operates blindly on all non-offline clients in the room without targeted multi-selection or a confirmation dialog stating target count and room context.

---

## 2. Technical Design

### A. Core Roster State & Operations (`nstu::server::roster`)
Extract and centralize pure roster operations into `server/include/nstu/classroom_roster.hpp` and `server/src/classroom_roster.cpp`:
1. **Display Mode**:
   ```cpp
   enum class ClassDisplayMode : int {
       grid,
       list
   };
   ```
2. **Filtering & Status Aggregation**:
   - `RoomFilter` enum (`all`, `online`, `attention`, `locked`, `offline`).
   - `RoomCounts` structure counting online, attention, locked, and offline clients.
   - `bool client_matches_filter(const ClientRecord& client, RoomFilter filter, bool show_offline, std::string_view query);`
   - `std::vector<const ClientRecord*> filter_clients(...)` returning matching client references.
3. **Multi-Selection Management**:
   - `std::unordered_set<std::uint64_t>` tracking checked client IDs.
   - `select_all_matching()`: adds all visible clients matching current filters to selection.
   - `clear_selection()`: empties selection set.
   - `toggle_selection()`: toggles single client ID.
4. **Safe Bulk Actions**:
   - `enum class BulkActionKind : int { lock, unlock, refresh_snapshots };`
   - `BulkActionTargetSummary evaluate_bulk_targets(...)`: counts total selected, eligible online targets, and skipped offline clients.
   - `BulkActionResult execute_bulk_action(...)`: safely executes the requested action against eligible online clients using `ServerControlPlane`, tracking success and failure counts and error messages.
   - `std::string format_bulk_confirmation(...)`: formats English/Vietnamese modal message stating target count, room name, and action name.

### B. Teacher Console UI Integration (`server/app/main.cpp`)
1. **View Toggle**:
   - Add Grid / List toggle chips to `draw_workspace_toolbar`.
   - Grid mode renders `draw_room_screen_wall` with screen cards.
   - List mode renders `draw_room_client_table` with ImGui table columns:
     - Checkbox column (with checkbox header for select-all / clear toggle)
     - Hostname (with unread chat badge indicator)
     - IP Address
     - Status Badge
     - Latency & Packet Loss
     - UWF Protection Status (Active / Disabled / Unsupported)
2. **Multi-Selection Controls**:
   - "Select All" and "Clear Selection" buttons in workspace toolbar when clients exist.
   - Displays count of selected clients (e.g., `"Selected: 12/35"`).
3. **Safe Bulk Actions Bar & Modal Confirmation**:
   - Bulk action buttons ("Bulk Lock", "Bulk Unlock", "Bulk Refresh") enabled only when `!checked_client_ids.empty()`.
   - Clicking a bulk action opens modal popup `bulk-action-confirmation`.
   - Modal states:
     - Target client count (e.g. `"5 computers selected"`).
     - Classroom identifier / room name.
     - Concrete warning/description of action.
     - "Confirm" and "Cancel" buttons.
   - On confirmation, executes `execute_bulk_action` and posts detailed outcome to `DashboardState::control_status`.

---

## 3. Baseline Source Hashes

Before modification:
- `server/CMakeLists.txt`: `AB7A8403A00FA0621B5E071DAFD01141BE8A6B60267900965E48C8614E1132B6`
- `server/app/main.cpp`: `80BEC2039F74E683C889B55D9DAC7E3CA5A405CA305D4C295D2079030D9959A5`
- `tests/CMakeLists.txt`: `B0ACC2BC361FC3D6B1B70E7CF3986A4EC6EF982EB4EE4ABBD7716C1512B08660`

---

## 4. Acceptance Criteria

1. [ ] Pure roster logic and types defined in `server/include/nstu/classroom_roster.hpp` and `server/src/classroom_roster.cpp`.
2. [ ] `server/CMakeLists.txt` builds `src/classroom_roster.cpp` into `nstu_server_state`.
3. [ ] `DashboardState` supports `ClassDisplayMode display_mode` and `std::unordered_set<std::uint64_t> checked_client_ids`.
4. [ ] Toolbar renders Grid/List toggle chips, selection count, Select All / Clear Selection, and bulk action triggers.
5. [ ] Tabular view `draw_room_client_table` renders cleanly with selection checkboxes, hostname, IP, status, latency/loss, and UWF status.
6. [ ] Bulk action modal confirmation requires explicit confirmation and states exact target count, room, and action before invoking `ServerControlPlane`.
7. [ ] `tests/classroom_roster_tests.cpp` passes 100% of cases, including multi-select state transitions, filtering, bulk target evaluation, and 10/30/50 client performance benchmarks (<1ms execution).
8. [ ] Local `./scripts/verify-gate.sh` and `./scripts/verify-gate.sh --strict` pass with 0 warnings.
9. [ ] All GitHub Actions CI jobs pass on feature branch and upon fast-forward merge to `main`.

