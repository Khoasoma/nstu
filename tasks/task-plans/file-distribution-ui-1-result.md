# Task Result: FILE-DISTRIBUTION-UI-1

**Task ID**: `FILE-DISTRIBUTION-UI-1`  
**Phase**: Priority 2 / P2.3 Daily Teaching Workflows & File Distribution (Client Service Receiver & Teacher Console UI)  
**Roadmap Reference**: `REVISED_ROADMAP.md` (P2.3: "File Distribution: teacher-to-client first; bounded size, safe staging, path traversal rejection, integrity check, progress/retry; never auto-execute received files") and `tasks/product-execution-plan.md` (`TOOL-06/07`).

---

## 1. Summary of Changes

This task completes the File Distribution vertical slice by implementing the client service receiving engine and the teacher console Dear ImGui UI.

### 1. Client Service File Receiver (`client/service/main.cpp`)
- Added `#include "nstu/file_distribution.hpp"`.
- Implemented `get_safe_staging_directory()` returning `%PUBLIC%\Documents\NSTU_Received` (with fallback to `%LOCALAPPDATA%\NSTU\ReceivedFiles` or `%TEMP%\NSTU_Received`).
- Maintained `ActiveFileReceiveSession` and mutex `g_file_receive_mutex`.
- Implemented `send_file_dist_status()` helper encoding `FileDistStatusMessage` and dispatching via `queue_outbound_message()`.
- Handled incoming commands in `handle_server_command()`:
  - `CommandType::file_dist_start = 63`:
    - Checks exam lockdown status: immediately sends `FileDistStatus::rejected_exam_active` and aborts if armed.
    - Validates filename safety via `nstu::file_dist::is_safe_filename()`: sends `FileDistStatus::rejected_unsafe_path` if invalid.
    - Enforces 64 MiB maximum file quota (`kMaxFileSize`): sends `FileDistStatus::rejected_quota_exceeded` if exceeded.
    - Resolves staging path via `nstu::file_dist::compute_staging_path()`.
    - Creates staging `.nstu_part` file for atomic streaming.
    - Emits security audit events and responds with `FileDistStatus::in_progress` (0 bytes received).
  - `CommandType::file_dist_chunk = 64`:
    - Validates active session ID and offset alignment.
    - Aborts if exam becomes armed during transfer.
    - Streams chunk binary data to `.nstu_part` file and advances `bytes_received`.
    - Responds with `FileDistStatus::in_progress` and updated progress.
  - `CommandType::file_dist_finish = 65`:
    - Flushes and closes `.nstu_part` temporary file.
    - Verifies complete file length.
    - Calculates SHA-256 digest of staged file using `nstu::security::sha256()`.
    - Compares digest against sender's expected hash using `nstu::security::constant_time_equal()`.
    - If valid: atomically renames `.nstu_part` to target filename and replies with `FileDistStatus::completed`.
    - If mismatch: immediately deletes `.nstu_part` and replies with `FileDistStatus::hash_mismatch`.
  - `CommandType::file_dist_cancel = 67`:
    - Aborts transfer session, closes file handle, deletes `.nstu_part` file, and responds with `FileDistStatus::cancelled`.
  - Control channel disconnect handling:
    - Cleans up and deletes any in-flight `.nstu_part` file when server connection drops.
- **Non-Negotiable Invariant ("Never Auto-Execute")**: Received files reside passively in the staging folder. No process launching or shell execution is performed.

### 2. Teacher Console Dear ImGui UI (`server/app/main.cpp`)
- Added `#include "nstu/file_distribution.hpp"`.
- Added `std::array<char, 1024> file_dist_path_input{""};` in `DashboardState`.
- In `draw_selected_client`:
  - Added "Send File" / "Phát tệp" button.
  - Disabled with bilingual tooltips when the client is offline or under an active exam.
  - Sets `state.checked_client_ids = {selected_client->id}`, `pending_bulk_action = BulkActionKind::distribute_file`, and opens confirmation popup.
- In `draw_workspace_toolbar`:
  - Added "Send File" / "Phát tệp" button when clients are selected.
  - Sets `state.pending_bulk_action = BulkActionKind::distribute_file` and opens confirmation modal.
- In `draw_bulk_action_confirmation_popup`:
  - Handled `BulkActionKind::distribute_file`:
    - File path input field with real-time validation (checks file existence, regular file status, 64 MiB limit, and filename safety).
    - Formats and displays file size in MiB and bytes.
    - Role permission check: verifies `OperatorAction::distribute_file` permission (Teacher, Technician, Developer).
    - Binds `file_dist_path_input` to `execute_bulk_action()` upon operator confirmation.

---

## 2. File Hashes & Changes

| Path | Baseline SHA-256 | New SHA-256 | Status |
|---|---|---|---|
| `client/service/main.cpp` | `09E7A7439F6E04A18FAD7ED80EAE6B5F892E1D34DD7D875FFB7BA6972A1A8CE2` | `2CA817AB7720CCA8D30D05E3DF4BA9684B97DED3F15B1E269F94C8290C723418` | Modified |
| `server/app/main.cpp` | `CC908420485D3B4AE754FF726E5CE9CE845898E2AD896A9D94ADCABE843AAA4C` | `D5D4B6C55A7934218652EE32CF9BEC438F731C6F1469DF5A710C02DC6EC10DEE` | Modified |

---

## 3. Verification Receipts

1. **Fast Verification Gate (`./scripts/verify-gate.sh`)**:
   - Banned names: PASS
   - Forbidden paths check: PASS
   - Assertion preservation: PASS
   - Browser / Node suite: PASS (63/63 passed)
   - MinGW build: PASS (zero compiler warnings)
   - Unit test suites: PASS (67/67 suites passed 100%)

2. **Strict Verification Gate (`./scripts/verify-gate.sh --strict`)**:
   - Release build with `NSTU_ENABLE_WERROR=ON`: PASS (zero compiler warnings)
   - Unit test suites: PASS (67/67 suites passed 100%)

