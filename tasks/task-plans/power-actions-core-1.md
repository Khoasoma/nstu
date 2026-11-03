# Task Plan: POWER-ACTIONS-CORE-1

## 1. Objective & Scope
This task implements the core protocol, codecs, server-side authorization, and roster bulk plumbing for **Power Actions** (Reboot & Shutdown) per `REVISED_ROADMAP.md` (Priority 2 / P2.3: "Power actions: server-side authorization, count/confirmation, offline semantics, force-close disabled by default; Wake-on-LAN only with honest support detection") and P2.1 role separation:
1. **Wire Protocol & Codecs**:
   - Define command `CommandType::power_action = 61` in `common/include/nstu/protocol.hpp`.
   - Define `PowerActionKind` (`reboot = 1`, `shutdown = 2`) in `common/include/nstu/control_messages.hpp`.
   - Define `PowerActionMessage` struct with `action` and `force` (default `false`).
   - Implement `encode_power_action` and `decode_power_action` in `common/src/control_messages.cpp`:
     - Fixed 2-byte binary layout: 1 byte action kind + 1 byte flags (bit 0 = force, bits 1–7 reserved and must be 0).
     - Strict validation: rejects buffers != 2 bytes, invalid action kinds, or reserved flag bits.
2. **Server-Side Authorization & Control Plane**:
   - Add `shutdown_client = 11` to `OperatorAction` in `server/include/nstu/operator_role.hpp`.
   - Update permission matrix in `server/src/operator_role.cpp`:
     - Teacher role: disruptive maintenance (`configure_uwf`, `reboot_client`, `shutdown_client`) remains disarmed/unpermitted (`false`), preserving P2.1 role separation and `INDEV_STATUS.md` line 34 baseline.
     - Technician & Developer roles: permitted both `reboot_client` and `shutdown_client` (`true`).
   - In `tests/operator_role_tests.cpp`, add tests verifying `shutdown_client` permission semantics across Teacher, Technician, and Developer without removing existing assertions.
   - Declare and implement `ServerControlPlane::power_action` in `server/include/nstu/control_plane.hpp` and `server/src/control_plane.cpp`:
     ```cpp
     [[nodiscard]] bool power_action(
         std::uint64_t client_id,
         control::PowerActionKind action,
         bool force = false,
         OperatorRole caller_role = OperatorRole::technician,
         std::string* error = nullptr);
     ```
     - **Exam Safety Invariant**: If `exam_active(client_id) == true`, reject immediately with an error to protect in-progress exams from accidental or unauthorized power cuts.
     - **Role Authorization**: Verify `is_action_permitted(caller_role, ...)` for the requested action (`reboot_client` or `shutdown_client`).
     - **Force-Close Policy**: If `force == true`, strictly require elevated role (`OperatorRole::technician` or `OperatorRole::developer`).
     - Send command `CommandType::power_action = 61` to authenticated client.
     - Record audit event.
3. **Classroom Roster & Bulk Action Integration**:
   - Add `BulkActionKind::reboot = 5` and `BulkActionKind::shutdown = 6` to `server/include/nstu/classroom_roster.hpp`.
   - Update `execute_bulk_action` in `server/src/classroom_roster.cpp`:
     - Dispatch power actions to online targets with `caller_role` support.
     - **Offline Semantics**: Offline computers are safely skipped and reported in `target_summary.skipped_offline`.
   - Bilingual localization (EN/VI) in `classroom_roster.cpp`:
     - `format_bulk_confirmation_title` for Reboot and Shutdown.
     - `format_bulk_confirmation_message` detailing action, count, room name, and unsaved work warnings.
     - `format_bulk_action_result_status` formatting execution outcomes.
4. **Verification & Test Suite**:
   - Create `tests/power_action_tests.cpp` covering:
     - Binary codec round-trip for all valid combinations (reboot/shutdown, graceful/force).
     - Truncated, oversized, invalid action enum, and reserved bit rejection.
     - Role-based server authorization and force-close permission gating.
     - Exam active containment guard.
     - Bilingual confirmation and result formatting.
   - Register target `nstu_power_action_tests` in `tests/CMakeLists.txt`.
   - Fast gate (`verify-gate.sh`) and strict gate (`verify-gate.sh --strict`) passing with zero warnings and 100% test pass rate (65/65).

---

## 2. File Allowlist & Source Baselines (SHA-256)
| File | Baseline SHA-256 | Post-Implementation SHA-256 |
|---|---|---|
| `common/include/nstu/protocol.hpp` | `bdeac6f28575e943a964f6e836cc72b3a7e7340d32b0c246e9ee81ac10f4424c` | `5a2432b043d848437f6412edc46be03ef5da6d9c03584bf491b35dc249df3b31` |
| `common/include/nstu/control_messages.hpp` | `330d776cefbdc70ee3769f43b1c4e9015a10c9ebdd3ad740d37280201e4d37ee` | `a5b1847b9c379c41c8b287d213ab476b1e4118ad9b90b7579554b1088cdf4676` |
| `common/src/control_messages.cpp` | `3a3a7c9c74b5b58a5738a208d86e659dac154743e116ecc2567239e452c2d6de` | `be4b1e4078be600dd432d57b072b4121771fdbb9e9735eae073317846aa90295` |
| `server/include/nstu/operator_role.hpp` | `5c05e3cf4ab09b798b6d092c2dfa6369ff126aaeb30e28c1ad3058b8bd2fb70d` | `20d524120b8dda100737a43d3af8a1bab4b90e93939c64b5c8ad3378e4d42e4d` |
| `server/src/operator_role.cpp` | `093f77fa90eaacdbfd4a944010403850dcce08bd8c7bd451e58bf2685ab6a362` | `65264427b63babe6a85229e7f108be59622b0609ebe6993a9bbd4476ad8dc4a3` |
| `server/include/nstu/control_plane.hpp` | `5138e36b88116afa80e86670a8e6690f66c3ed360b7a953907a5fc8ab2911c7a` | `452a42c851b6f815cf386e531749f878216208e122531b161f220043d8af7853` |
| `server/src/control_plane.cpp` | `16a01f03acda20a01e316d27eb8d2e76521a539948054ce14fbe779546278a5c` | `f13c4cc8e304658d646033c6bcfac584fe644586a7fd4cabd6de5b0c26514ce7` |
| `server/include/nstu/classroom_roster.hpp` | `c2c4cb18be0a22f2414a940cd4ac1ea1fd99dc6642ac7a342767e9879398ed75` | `2f0bfe48d0463ff9969d073d30eec477dba8801fbb7eb9b5465cd358a104b53a` |
| `server/src/classroom_roster.cpp` | `b18c63469baef11cbccab6956e6363c9918fe14e45ec23ec35542dc0d8c87283` | `821da1347673bc6cfdbc86acbf8e933db2955af0b21c6c321ad2295e4dac2620` |
| `tests/operator_role_tests.cpp` | `82f394778fb5c9eea93ee17710830d9edd8ab39ffb3dc2e143785d04c178981c` | `71b7aa72cbad24bca019b66ca46cc9279a74871a9396eebafe8370ee1d788b73` |
| `tests/CMakeLists.txt` | `68863613e266aebf888d6832fb9d53ad8a7092cfa7477846ea042b4b83326f9e` | `14591422dc6fffd5feb1aa890bbc538ac44ccf6174c5a402b830f3d51c1ca447` |
| `tests/power_action_tests.cpp` | New File | `cd674ff8a1944634235c9f71bfa658387685a1377431489cbd13c34c23d2b821` |
| `tasks/task-plans/power-actions-core-1.md` | Tracked | Tracked |

---

## 3. Acceptance Criteria
1. `CommandType::power_action = 61` defined in `protocol.hpp`.
2. `PowerActionKind` (`reboot = 1`, `shutdown = 2`) and `PowerActionMessage` defined in `control_messages.hpp`.
3. Codecs `encode_power_action` and `decode_power_action` strictly encode/decode 2-byte payloads, rejecting invalid sizes, invalid action types, and non-zero reserved flag bits.
4. `OperatorAction::shutdown_client = 11` defined in `operator_role.hpp`, with Teacher disallowed, and Technician/Developer permitted. `tests/operator_role_tests.cpp` asserts this behavior cleanly without weakening any existing assertions.
5. `ServerControlPlane::power_action` declared with explicit signature and parameter defaults, enforcing:
   - Client connection check.
   - Exam active rejection: refuses to reboot/shut down clients with an ongoing exam.
   - Server-side role authorization.
   - Force-close gating: `force == true` requires Technician or Developer role.
   - Command dispatch and audit trail recording.
6. `BulkActionKind::reboot = 5` and `BulkActionKind::shutdown = 6` integrated into `classroom_roster.hpp` and `classroom_roster.cpp` with bilingual EN/VI confirmation titles, warnings, and result statuses. Offline computers are cleanly skipped.
7. `tests/power_action_tests.cpp` passes 100% and tests all codec edge cases, server authorization rules, exam containment guards, and localization strings.
8. Zero compiler warnings under strict C++20 `-Wall -Wextra -Werror` and MSVC `/W4 /WX`.
9. All 65/65 tests pass in `./scripts/verify-gate.sh` and `./scripts/verify-gate.sh --strict`.

---

## 4. Test & Verification Plan
- Build target: `cmake --build build/mingw --target nstu_power_action_tests nstu_operator_role_tests`.
- Run test directly: `./build/mingw/tests/nstu_power_action_tests.exe`.
- Run fast gate: `./scripts/verify-gate.sh`.
- Run strict gate: `./scripts/verify-gate.sh --strict`.
- Invoke independent non-author code review subagent.
