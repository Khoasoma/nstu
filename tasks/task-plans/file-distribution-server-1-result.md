# Task Result: FILE-DISTRIBUTION-SERVER-1

**Task ID**: `FILE-DISTRIBUTION-SERVER-1`  
**Phase**: Priority 2 / P2.3 Daily Teaching Workflows & File Distribution  
**Roadmap Reference**: `REVISED_ROADMAP.md` (P2.3: "File Distribution: teacher-to-client first; bounded size, safe staging, path traversal rejection, integrity check, progress/retry; never auto-execute received files") and `tasks/product-execution-plan.md` (`TOOL-06/07`).

---

## 1. Implementation Summary

1. **Operator Role Authorization (`server/include/nstu/operator_role.hpp`, `server/src/operator_role.cpp`)**:
   - Added `OperatorAction::distribute_file = 13`.
   - Permitted roles: Teacher (`true`), Technician (`true`), Developer (`true`).
   - Unauthenticated callers are strictly denied.
2. **Server Control Plane Execution & State Machine (`server/include/nstu/control_plane.hpp`, `server/src/control_plane.cpp`)**:
   - Implemented `ServerControlPlane::distribute_file(std::uint64_t client_id, const std::filesystem::path& file_path, OperatorRole caller_role, std::string* error)`:
     - Enforces `is_action_permitted(caller_role, OperatorAction::distribute_file)`.
     - Rejects distribution to offline clients (`"Cannot distribute file: client is offline."`).
     - Enforces **active exam containment**: checks `exam_active(client_id)` to refuse file distribution to any student participating in an active exam (`"Cannot distribute file: exam active on client."`), logging a security audit denial via `record_remote_session_audit(client_id, "distribute_file", "denied", "exam active on client")`.
     - Validates source file existence on server disk, ensures regular file status, checks size <= 64 MiB (`kMaxFileSize`).
     - Computes 32-byte SHA-256 digest using `nstu::security::sha256()`.
     - Generates monotonic session IDs and frames data using `encode_file_dist_start` (63), streams `encode_file_dist_chunk` (64) in bounded 64 KiB segments, and finishes with `encode_file_dist_finish` (65).
     - Logs remote session audit entry upon successful dispatch or failure.
3. **Classroom Roster Bulk Actions (`server/include/nstu/classroom_roster.hpp`, `server/src/classroom_roster.cpp`)**:
   - Added `BulkActionKind::distribute_file = 8`.
   - Handled bulk dispatch in `execute_bulk_action`, skipping offline clients and passing the file path to `control_plane.distribute_file`.
   - Added bilingual title formatter (`format_bulk_confirmation_title`):
     - EN: `"Confirm Bulk File Distribution"`
     - VI: `"Xác nhận phát tệp hàng loạt"`
   - Added bilingual confirmation message formatter (`format_bulk_confirmation_message`):
     - EN: `"Are you sure you want to DISTRIBUTE a file to {N} selected computer(s) in room '{Room}'?"`
     - VI: `"Bạn có chắc chắn muốn PHÁT TỆP tới {N} máy tính đang chọn trong phòng '{Room}' không?"`
   - Added bilingual result status formatter (`format_bulk_action_result_status`) with `action_str` = `"file distribution"` (EN) / `"phát tệp"` (VI):
     - EN: `"Bulk file distribution completed: {S} succeeded, {F} failed."`
     - VI: `"Hoàn tất phát tệp hàng loạt: {S} thành công, {F} thất bại."`
4. **Verification & Tests (`tests/operator_role_tests.cpp`, `tests/classroom_roster_tests.cpp`)**:
   - Updated operator role tests for action 13 across Teacher, Technician, and Developer.
   - Added `test_bulk_action_distribute_file_formatters_and_execution` testing titles, confirmation messages, result status strings, and control plane guards (offline rejection, client not found, non-existent file).

---

## 2. File Allowlist & Hashes

| Path | Pre-Implementation SHA-256 | Post-Implementation SHA-256 |
|---|---|---|
| `server/include/nstu/operator_role.hpp` | `B1D9EF5D4C675762DEBD403935DDD37D3BA66434239AF6CFF32354DE1DCFC6EC` | `ABEFDD0C8791350D80B5CD5C540BECA8F48ED67203736A78DF9DE1635F6D681B` |
| `server/src/operator_role.cpp` | `CA3F8D0EA901F0F0E735ED91FD99A258D2577FC47A89333B5F470F0083915AF1` | `B5B0A8F9A2AEB5AAFA6EA0887966C373E1978A7B87C52B1FB83286118C23A285` |
| `server/include/nstu/classroom_roster.hpp` | `3D465EA202FF611E93133FBD3F8C7FF04F04A85247CB62349D706F7B6FDE894F` | `931D262D910D1CADC3A109F9BD3A1344F3D47A153EC40029B2E37E1A685DC0C5` |
| `server/src/classroom_roster.cpp` | `C4316F21FF976972ECA7F1509B8BC07F4C00A6FD1E948B5CED1A92C81721F10F` | `A82180672BB5EF3B8968AD4240C3F147405D0938638859FF2BD37B847472C421` |
| `server/include/nstu/control_plane.hpp` | `934FF1ECA1DB0FBD4B96CCAC473D841D96A5CE6B5A2DC221BFC30D6ED031B017` | `837B5C6AED56B649F3DDAD00A48E3315F064DA39AA7C84B1FFC98CF3730E2EC8` |
| `server/src/control_plane.cpp` | `3AA77643FCB9C889C0E3562F45672B131B9840B8226026E76DF2AEFE4B81ED87` | `42A1ADDBDACE30A54C793466596DFA7B19051247FB3DB1B17915811F6896C735` |
| `tests/operator_role_tests.cpp` | `71B7AA72CBAD24BCA019B66CA46CC9279A74871A9396EEBAFE8370EE1D788B73` | `644E49FB634D756CE8149EE4984B2241CB62F05FCAB768257728CFB38A834B73` |
| `tests/classroom_roster_tests.cpp` | `E1161873D388730EB9BDB31458F23F08270F9E5F8628233BFCB8536CC457F0F9` | `55E42BCAADF1BA169C19E37BDB5F2E5C0C0B02CA6007D20169B4B05749BE9C2A` |

---

## 3. Verification Receipts

- Focused Unit Tests (`nstu_operator_role_tests.exe` & `nstu_classroom_roster_tests.exe`): **PASS**.
- Fast Verification Gate (`./scripts/verify-gate.sh`): **PASS** (100% tests passed, 0 failures across 67 test suites, zero compiler warnings).
- Strict Verification Gate (`./scripts/verify-gate.sh --strict`): **PASS** (Release mode with `-Werror`, 100% tests passed across 67 test suites, zero compiler warnings).
- Clean working directory outside allowlisted files. No assertions removed or weakened.

