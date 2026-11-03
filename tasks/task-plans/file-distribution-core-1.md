# Task Plan: FILE-DISTRIBUTION-CORE-1

**Task ID**: `FILE-DISTRIBUTION-CORE-1`  
**Phase**: Priority 2 / P2.3 Daily Teaching Workflows: File Distribution (Core Wire Codecs, Security Sanitization & Chunking Math)  
**Roadmap Reference**: `REVISED_ROADMAP.md` (P2.3: "File Distribution: teacher-to-client first; bounded size, safe staging, path traversal rejection, integrity check, progress/retry; never auto-execute received files") and `tasks/product-execution-plan.md` (`TOOL-06/07`).

---

## 1. Description & Architectural Rationale

File Distribution enables teachers to send classroom materials (worksheets, lab instructions, reference documents, source code starters) to students over the authenticated control channel.

Security and resilience are paramount:
1. **Path Traversal & Device Name Defense**: Untrusted filenames from the wire or teacher input could attempt directory escape (`../../Windows/System32/evil.exe`) or exploit legacy Windows DOS device names (`CON`, `PRN`, `AUX`, `NUL`, `COM1-9`, `LPT1-9`). Strict sanitization and validation must reject any illegal characters (`/`, `\`, `:`, `*`, `?`, `"`, `<`, `>`, `|`, control bytes, non-printable characters), relative directory dots, or reserved device names at the wire codec boundary.
2. **Safe Staging & Containment**: Client files must be placed in a dedicated, isolated student staging directory (e.g. `ReceivedFiles` or user downloads). Under no circumstances may received files ever be staged into system directories, startup locations, or automatically executed (`never auto-execute received files`).
3. **Bounded Size & Memory Protection**: Transfer sizes are hard-capped at 64 MiB (`kMaxFileSize = 67,108,864` bytes) and chunked at 64 KiB (`kMaxChunkSize = 65,536` bytes) to prevent unbounded buffer allocations, memory exhaustion, or disk flooding.
4. **End-to-End Cryptographic Integrity**: Each distribution manifest carries a 32-byte SHA-256 digest. Transferred files are verified against the declared SHA-256 hash using `nstu::security::sha256` and `constant_time_equal` before final commit. Corrupted or mismatched files are discarded immediately.
5. **Typed Chunked Wire Protocol**: Commands `file_dist_start = 63`, `file_dist_chunk = 64`, `file_dist_finish = 65`, `file_dist_status = 66`, and `file_dist_cancel = 67` provide full asynchronous chunking, status tracking, and cancellation.

This task (`FILE-DISTRIBUTION-CORE-1`) establishes the core foundation:
- Protocol command definitions (`common/include/nstu/protocol.hpp`).
- File distribution codecs, data structures, safe filename validation, sanitization, staging containment helper, and status enums (`common/include/nstu/file_distribution.hpp`, `common/src/file_distribution.cpp`).
- Build system integration (`common/CMakeLists.txt`, `tests/CMakeLists.txt`).
- Comprehensive unit test suite (`tests/file_distribution_tests.cpp`).

---

## 2. Acceptance Criteria

1. **Protocol Commands (`common/include/nstu/protocol.hpp`)**:
   - `CommandType::file_dist_start = 63`
   - `CommandType::file_dist_chunk = 64`
   - `CommandType::file_dist_finish = 65`
   - `CommandType::file_dist_status = 66`
   - `CommandType::file_dist_cancel = 67`
2. **Filename Sanitization & Traversal Defense (`common/include/nstu/file_distribution.hpp`, `common/src/file_distribution.cpp`)**:
   - `is_safe_filename(std::string_view name)` returns `true` if and only if:
     - 1 <= length <= 255 bytes.
     - Contains no `/`, `\`, `:`, `*`, `?`, `"`, `<`, `>`, `|`, `\0`, or ASCII control characters (< 0x20 or == 0x7F).
     - Does not equal `"."` or `".."` and does not begin or end with a space or dot.
     - Is not a reserved Windows DOS device name (`CON`, `PRN`, `AUX`, `NUL`, `COM1`–`COM9`, `LPT1`–`LPT9`, case-insensitive, with or without any extension).
   - `sanitize_filename(std::string_view name)` strips directory components, cleans prohibited characters, trims leading/trailing spaces/dots, and returns a safe filename (falling back to `"received_file.bin"` if empty or reserved).
   - `compute_staging_path(const std::filesystem::path& base_dir, std::string_view filename)` validates filename safety and ensures the computed path is strictly contained within `base_dir`.
3. **Data Types & Codecs (`common/include/nstu/file_distribution.hpp`, `common/src/file_distribution.cpp`)**:
   - `FileDistStatus` enum: `in_progress = 0`, `completed = 1`, `hash_mismatch = 2`, `write_error = 3`, `cancelled = 4`, `rejected_exam_active = 5`, `rejected_unsafe_path = 6`, `rejected_quota_exceeded = 7`.
   - `status_label(FileDistStatus, bool vietnamese)` providing bilingual EN/VI status strings.
   - `FileDistStartMessage` codec: encodes/decodes session_id, file_size, sha256 (32 bytes), and filename. Rejects payloads with size > 64 MiB, filename > 255 bytes, or unsafe filenames.
   - `FileDistChunkMessage` codec: encodes/decodes session_id, offset, and binary chunk data. Rejects chunk sizes > 64 KiB or malformed lengths.
   - `FileDistFinishMessage` codec: encodes/decodes session_id (8 bytes).
   - `FileDistStatusMessage` codec: encodes/decodes session_id, bytes_received, and status enum (24 bytes with reserved zero padding).
   - `FileDistCancelMessage` codec: encodes/decodes session_id and reason_code (16 bytes).
4. **Verification & Tests (`tests/file_distribution_tests.cpp`)**:
   - Dedicated unit test target `nstu_file_distribution_tests` covering:
     - All safe and unsafe filename patterns (traversal attempts like `../../evil`, `C:\Windows\System32`, `con.txt`, `aux`, `nul.tar`, `foo/bar`, `..`).
     - Filename sanitization on real-world inputs.
     - Staging path computation and containment.
     - Round-trip serialization and deserialization for all 5 message types.
     - Strict rejection of malformed, truncated, oversized, or malicious payloads.
     - SHA-256 verification and mismatch detection using `nstu::security::sha256`.
     - Bilingual status labels (EN/VI).
   - Fast and strict verification gates pass 100% with zero compiler warnings.

---

## 3. File Allowlist & Baseline SHA-256 Hashes

| Path | Baseline SHA-256 | Description |
|---|---|---|
| `common/include/nstu/protocol.hpp` | `E333D5F48D8FA6883CC69444AC3183DC615E040EB50E4B0A513CDC82C2DED470` | Define `CommandType::file_dist_*` values 63–67 |
| `common/include/nstu/file_distribution.hpp` | `NEW_FILE` | Declare message structs, codecs, sanitization, and status enums |
| `common/src/file_distribution.cpp` | `NEW_FILE` | Implement codecs, sanitization logic, and staging helpers |
| `common/CMakeLists.txt` | `A5AB28445BF1D33F563FB0428735F30F7040E9C2427ACF9A3EAB0141ACA65829` | Add `src/file_distribution.cpp` to `nstu_common` target |
| `tests/file_distribution_tests.cpp` | `NEW_FILE` | Comprehensive unit tests for codecs, sanitization, and integrity |
| `tests/CMakeLists.txt` | `7CE9AEC0EA3E7AA36E70C915F175329B1ECD681A5CA6EA1E145ECD468F921CB7` | Register `nstu_file_distribution_tests` executable and test |

---

## 4. Dependencies & Non-Negotiable Invariants

- **Author attribution**: Must commit exclusively as `khoasoma <songtu438@gmail.com>` with no AI assistant trailers.
- **Preserved dirty paths**: Never stage, modify, or revert `README.md`, `README.vi.md`, `docs/assets/banner.png`.
- **Private files**: Never stage or commit `PRODUCT_ROADMAP_NSTU.md`, `artifacts/`, `.claude/`, or `HANDOFF.md`.
- **Preserve all assertions**: Every existing unit test and assertion across all 66 test suites must pass without modification.
- **Path Traversal Zero-Tolerance**: Any attempt to write outside the staging directory or supply relative path segments must be strictly rejected at decode time.
- **Never Auto-Execute**: Transferred files are passive data only; never invoke shell or launch processes on them.
- **Memory & Quota Bounding**: Upper bounds strictly enforced: 64 MiB max file, 64 KiB max chunk, 255 bytes max filename.

---

## 5. Verification Commands

```bash
# Focused test execution
cmake --build build/mingw --target nstu_file_distribution_tests
./build/mingw/tests/nstu_file_distribution_tests.exe

# Fast verification gate (zero warnings, all unit tests pass)
./scripts/verify-gate.sh

# Strict verification gate (Release mode, zero warnings, strict checks)
./scripts/verify-gate.sh --strict
```

