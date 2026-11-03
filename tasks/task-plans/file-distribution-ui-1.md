# Task Plan: FILE-DISTRIBUTION-UI-1

**Task ID**: `FILE-DISTRIBUTION-UI-1`  
**Phase**: Priority 2 / P2.3 Daily Teaching Workflows & File Distribution (Client Service Receiver & Teacher Console UI)  
**Roadmap Reference**: `REVISED_ROADMAP.md` (P2.3: "File Distribution: teacher-to-client first; bounded size, safe staging, path traversal rejection, integrity check, progress/retry; never auto-execute received files") and `tasks/product-execution-plan.md` (`TOOL-06/07`).

---

## 1. Description & Architectural Rationale

This task completes the File Distribution vertical slice by implementing the client service receiving engine and the teacher console UI.

Key components:
1. **Client Service File Distribution Receiver (`client/service/main.cpp`)**:
   - Handles incoming commands from the teacher control channel:
     - `CommandType::file_dist_start = 63`:
       - If `g_exam_lockdown_controller.is_armed()`, responds with status `rejected_exam_active` and ignores transfer.
       - Decodes `FileDistStartMessage`. Validates `is_safe_filename()` and `file_size <= kMaxFileSize`. If invalid, responds with `rejected_unsafe_path` or `rejected_quota_exceeded`.
       - Resolves safe public staging directory: `%PUBLIC%\Documents\NSTU_Received` (or `%LOCALAPPDATA%\NSTU\ReceivedFiles` fallback) and creates it if needed.
       - Computes isolated staging path using `compute_staging_path`.
       - Opens temporary file `<staging_path>.nstu_part` for binary streaming.
       - Replies with `file_dist_status = 66` (`in_progress`, 0 bytes received).
     - `CommandType::file_dist_chunk = 64`:
       - Matches active session ID, writes chunk data to the temporary `.nstu_part` file, advances `bytes_received`, and replies with `in_progress`.
     - `CommandType::file_dist_finish = 65`:
       - Flushes and closes temporary file.
       - Calculates SHA-256 of the complete received temporary file using `nstu::security::sha256()`.
       - Compares against declared `sha256` digest via `constant_time_equal()`.
       - If hash matches: commits file by renaming `.nstu_part` to the final filename, and replies with `completed`.
       - If hash mismatches: immediately deletes the `.nstu_part` temporary file, and replies with `hash_mismatch`.
     - `CommandType::file_dist_cancel = 67`:
       - Aborts transfer, closes and deletes `.nstu_part` file.
   - **Non-Negotiable Invariant ("Never Auto-Execute")**: The client service NEVER executes or launches processes on received files; they reside purely as inert passive files in the staging folder.
2. **Teacher Console Dear ImGui UI (`server/app/main.cpp`)**:
   - **Workspace Toolbar Bulk Action**: Adds "Send File" / "Phát tệp" button in `draw_workspace_toolbar` to initiate file distribution across selected clients.
   - **Selected Client Action**: Adds "Send File" / "Phát tệp" button in `draw_selected_client`, disabled with hover tooltips if client is offline or exam is active.
   - **Bulk Confirmation Modal**: In `draw_bulk_action_confirmation_popup`, handles `BulkActionKind::distribute_file`:
     - Displays file path input field with real-time validation (checks file existence, regular file status, and <= 64 MiB limit).
     - Displays target count (online clients, excluding offline).
     - Executes `execute_bulk_action` with `distribute_file` upon human confirmation.
3. **Verification**:
   - Fast and strict verification gates pass 100% with zero compiler warnings across all 67 test suites.

---

## 2. Acceptance Criteria

1. **Client Service Receiver (`client/service/main.cpp`)**:
   - Armed exam rejection: immediately sends `rejected_exam_active` and aborts if exam lockdown is active.
   - Safe staging directory: creates and utilizes `%PUBLIC%\Documents\NSTU_Received` (or fallback).
   - Atomic staging: writes to `.nstu_part`, verifies SHA-256 upon finish, renames to final destination on success, and deletes temporary file on hash mismatch or cancellation.
   - Rejects unsafe filenames and quota-exceeding payloads.
   - Zero auto-execution: received files are never launched or executed.
2. **Teacher Console UI (`server/app/main.cpp`)**:
   - Toolbar bulk button "Send File" / "Phát tệp" opens confirmation modal with `BulkActionKind::distribute_file`.
   - Single-client detail view has "Send File" / "Phát tệp" button with offline/exam disabled states and tooltips.
   - Confirmation modal handles `BulkActionKind::distribute_file`, validating local path, file existence, and 64 MiB limit before enabling "Confirm & Execute".
3. **Verification**:
   - Fast gate (`./scripts/verify-gate.sh`) and strict gate (`./scripts/verify-gate.sh --strict`) pass with zero compiler warnings and all 67 test suites passing.

---

## 3. File Allowlist & Baseline SHA-256 Hashes

| Path | Baseline SHA-256 | Description |
|---|---|---|
| `client/service/main.cpp` | `09E7A7439F6E04A18FAD7ED80EAE6B5F892E1D34DD7D875FFB7BA6972A1A8CE2` | Receive file distribution commands and safely stage files |
| `server/app/main.cpp` | `CC908420485D3B4AE754FF726E5CE9CE845898E2AD896A9D94ADCABE843AAA4C` | Teacher console UI: toolbar button, client action, confirmation modal |

---

## 4. Dependencies & Non-Negotiable Invariants

- **Author attribution**: Must commit exclusively as `khoasoma <songtu438@gmail.com>` with no AI assistant trailers.
- **Preserved dirty paths**: Never stage, modify, or revert `README.md`, `README.vi.md`, `docs/assets/banner.png`.
- **Private files**: Never stage or commit `PRODUCT_ROADMAP_NSTU.md`, `artifacts/`, `.claude/`, or `HANDOFF.md`.
- **Preserve all assertions**: Every existing unit test and assertion across all 67 test suites must pass without modification.
- **Path Traversal Defense**: All written filenames must be strictly validated by `is_safe_filename` and contained by `compute_staging_path`.
- **Never Auto-Execute**: Received files must never be launched or executed.
- **Exam Containment**: File transfer is refused and aborted if an exam is active.

---

## 5. Verification Commands

```bash
# Fast verification gate (zero warnings, all 67 test suites pass)
./scripts/verify-gate.sh

# Strict verification gate (Release mode, zero warnings, strict checks)
./scripts/verify-gate.sh --strict
```

