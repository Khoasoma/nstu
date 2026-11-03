# Task Plan: FILE-DISTRIBUTION-SERVER-1

**Task ID**: `FILE-DISTRIBUTION-SERVER-1`  
**Phase**: Priority 2 / P2.3 Daily Teaching Workflows & File Distribution  
**Roadmap Reference**: `REVISED_ROADMAP.md` (P2.3: "File Distribution: teacher-to-client first; bounded size, safe staging, path traversal rejection, integrity check, progress/retry; never auto-execute received files") and `tasks/product-execution-plan.md` (`TOOL-06/07`).

---

## 1. Description & Architectural Rationale

Following the protocol foundation and wire codecs established in `FILE-DISTRIBUTION-CORE-1`, this task implements the server-side distribution engine, operator role permissions, active exam containment, and classroom roster bulk distribution actions.

Key requirements:
1. **Operator Role Authorization (`server/include/nstu/operator_role.hpp`, `server/src/operator_role.cpp`)**:
   - `OperatorAction::distribute_file = 13`.
   - Teacher, Technician, and Developer roles are permitted to distribute classroom materials and diagnostics utilities.
   - Unauthenticated or invalid roles are strictly rejected.
2. **Server Control Plane Execution & State Machine (`server/include/nstu/control_plane.hpp`, `server/src/control_plane.cpp`)**:
   - `ServerControlPlane::distribute_file(std::uint64_t client_id, const std::filesystem::path& file_path, OperatorRole role, std::string* error)`:
     - Enforces `is_action_permitted(role, OperatorAction::distribute_file)`.
     - Rejects distribution to offline clients (`"Cannot distribute file: client is offline."`).
     - Enforces **active exam containment**: queries `exam_active(client_id)` to refuse file distribution to any student participating in an exam (`"Cannot distribute file: exam active on client."`), logging a security audit denial via `record_remote_session_audit(client_id, "distribute_file", "denied", "exam active on client")`.
     - Validates source file existence on disk, ensures regular file status, checks size <= 64 MiB (`kMaxFileSize`).
     - Computes SHA-256 digest using `nstu::security::sha256()`.
     - Generates unique monotonic 64-bit session ID.
     - Sends `CommandType::file_dist_start = 63` with filename, size, and SHA-256.
     - Streams `CommandType::file_dist_chunk = 64` in bounded 64 KiB segments.
     - Sends `CommandType::file_dist_finish = 65`.
     - Logs remote session audit entry upon successful dispatch or failure.
3. **Classroom Roster Bulk Actions (`server/include/nstu/classroom_roster.hpp`, `server/src/classroom_roster.cpp`)**:
   - `BulkActionKind::distribute_file = 8`.
   - In `execute_bulk_action`, dispatches distribution to all eligible online clients, passing the local file path via `extra_payload`. Offline clients are skipped. If a target client has an active exam, `control_plane.distribute_file` denies the action, logs the audit denial, and records an individual failure message (`"{hostname}: Cannot distribute file: exam active on client."`).
   - Updates `format_bulk_confirmation_title`:
     - EN: `"Confirm Bulk File Distribution"`
     - VI: `"Xác nhận phát tệp hàng loạt"`
   - Updates `format_bulk_confirmation_message`:
     - EN: `"Are you sure you want to DISTRIBUTE a file to {N} online computer(s) in {Room}?"`
     - VI: `"Bạn có chắc chắn muốn PHÁT TỆP tới {N} máy tính đang trực tuyến trong {Room} không?"`
   - Updates `format_bulk_action_result_status` using standard pattern with `action_str` = `"file distribution"` (EN) / `"phát tệp"` (VI):
     - EN: `"Bulk file distribution completed: {S} succeeded, {F} failed."`
     - VI: `"Hoàn tất phát tệp hàng loạt: {S} thành công, {F} thất bại."`
4. **Verification & Tests (`tests/operator_role_tests.cpp`, `tests/classroom_roster_tests.cpp`)**:
   - Updates role matrix test suites for action 13 across all roles.
   - Verifies bulk action confirmation title, message, and result status formatting in English and Vietnamese.
   - Comprehensive unit testing for `ServerControlPlane::distribute_file`: role authorization, offline rejection, active exam rejection, file validation, and chunked streaming with SHA-256 verification.
   - All 67 unit test suites pass with zero warnings.

---

## 2. Acceptance Criteria

1. **Role Matrix (`server/include/nstu/operator_role.hpp`, `server/src/operator_role.cpp`)**:
   - `OperatorAction::distribute_file = 13`.
   - Permitted: Teacher (`true`), Technician (`true`), Developer (`true`).
2. **Server Control Plane Integration (`server/include/nstu/control_plane.hpp`, `server/src/control_plane.cpp`)**:
   - Added method `distribute_file`:
     ```cpp
     [[nodiscard]] bool distribute_file(
         std::uint64_t client_id,
         const std::filesystem::path& file_path,
         OperatorRole role = OperatorRole::teacher,
         std::string* error = nullptr);
     ```
   - Enforces role authorization, online client status, regular file check, 64 MiB size limit, and exam containment (`exam_active(client_id)`).
   - Generates monotonic session IDs and frames data using `encode_file_dist_start`, `encode_file_dist_chunk`, and `encode_file_dist_finish`.
3. **Classroom Roster Bulk Distribution (`server/include/nstu/classroom_roster.hpp`, `server/src/classroom_roster.cpp`)**:
   - `BulkActionKind::distribute_file = 8`.
   - `format_bulk_confirmation_title`:
     - EN: `"Confirm Bulk File Distribution"`
     - VI: `"Xác nhận phát tệp hàng loạt"`
   - `format_bulk_confirmation_message`:
     - EN: `"Are you sure you want to DISTRIBUTE a file to {N} online computer(s) in {Room}?"`
     - VI: `"Bạn có chắc chắn muốn PHÁT TỆP tới {N} máy tính đang trực tuyến trong {Room} không?"`
   - `format_bulk_action_result_status`:
     - EN: `"Bulk file distribution completed: {S} succeeded, {F} failed."`
     - VI: `"Hoàn tất phát tệp hàng loạt: {S} thành công, {F} thất bại."`
4. **Verification**:
   - Fast and strict verification gates pass 100% with zero compiler warnings across all 67 test suites.

---

## 3. File Allowlist & Baseline SHA-256 Hashes

| Path | Baseline SHA-256 | Description |
|---|---|---|
| `server/include/nstu/operator_role.hpp` | `B1D9EF5D4C675762DEBD403935DDD37D3BA66434239AF6CFF32354DE1DCFC6EC` | Add `OperatorAction::distribute_file = 13` |
| `server/src/operator_role.cpp` | `CA3F8D0EA901F0F0E735ED91FD99A258D2577FC47A89333B5F470F0083915AF1` | Configure permission matrix for `distribute_file` |
| `server/include/nstu/classroom_roster.hpp` | `3D465EA202FF611E93133FBD3F8C7FF04F04A85247CB62349D706F7B6FDE894F` | Add `BulkActionKind::distribute_file = 8` |
| `server/src/classroom_roster.cpp` | `C4316F21FF976972ECA7F1509B8BC07F4C00A6FD1E948B5CED1A92C81721F10F` | Handle bulk file distribution and bilingual formatters |
| `server/include/nstu/control_plane.hpp` | `934FF1ECA1DB0FBD4B96CCAC473D841D96A5CE6B5A2DC221BFC30D6ED031B017` | Declare `ServerControlPlane::distribute_file` |
| `server/src/control_plane.cpp` | `3AA77643FCB9C889C0E3562F45672B131B9840B8226026E76DF2AEFE4B81ED87` | Implement `ServerControlPlane::distribute_file` with checks & chunk streaming |
| `tests/operator_role_tests.cpp` | `71B7AA72CBAD24BCA019B66CA46CC9279A74871A9396EEBAFE8370EE1D788B73` | Test `OperatorAction::distribute_file` across all roles |
| `tests/classroom_roster_tests.cpp` | `E1161873D388730EB9BDB31458F23F08270F9E5F8628233BFCB8536CC457F0F9` | Test bulk distribution action, message formatting, and control plane distribution |

---

## 4. Dependencies & Non-Negotiable Invariants

- **Author attribution**: Must commit exclusively as `khoasoma <songtu438@gmail.com>` with no AI assistant trailers.
- **Preserved dirty paths**: Never stage, modify, or revert `README.md`, `README.vi.md`, `docs/assets/banner.png`.
- **Private files**: Never stage or commit `PRODUCT_ROADMAP_NSTU.md`, `artifacts/`, `.claude/`, or `HANDOFF.md`.
- **Preserve all assertions**: Every existing unit test and assertion across all 67 test suites must pass without modification.
- **Active Exam Containment**: Never distribute files to a student client while an exam is active (`exam_active(client_id)`).
- **Offline Semantics**: File distribution requires an active authenticated session; offline clients must be skipped.
- **Never Auto-Execute**: Files are delivered as passive payloads only.

---

## 5. Verification Commands

```bash
# Focused tests
cmake --build build/mingw --target nstu_operator_role_tests
./build/mingw/tests/nstu_operator_role_tests.exe
cmake --build build/mingw --target nstu_classroom_roster_tests
./build/mingw/tests/nstu_classroom_roster_tests.exe

# Fast verification gate
./scripts/verify-gate.sh

# Strict verification gate
./scripts/verify-gate.sh --strict
```
