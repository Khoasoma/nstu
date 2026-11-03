# Task Result: FILE-DISTRIBUTION-CORE-1

**Task ID**: `FILE-DISTRIBUTION-CORE-1`  
**Phase**: Priority 2 / P2.3 Daily Teaching Workflows: File Distribution (Core Wire Codecs, Security Sanitization & Chunking Math)  
**Roadmap Reference**: `REVISED_ROADMAP.md` (P2.3: "File Distribution: teacher-to-client first; bounded size, safe staging, path traversal rejection, integrity check, progress/retry; never auto-execute received files") and `tasks/product-execution-plan.md` (`TOOL-06/07`).

---

## 1. Implementation Summary

1. **Protocol Commands (`common/include/nstu/protocol.hpp`)**:
   - Defined `CommandType` entries:
     - `file_dist_start = 63`: file metadata header (session_id, file_size, sha256 digest, filename).
     - `file_dist_chunk = 64`: binary payload chunk (session_id, offset, data).
     - `file_dist_finish = 65`: transfer completed signal (session_id).
     - `file_dist_status = 66`: client progress report / ACK (session_id, bytes_received, status).
     - `file_dist_cancel = 67`: bidirectional cancellation (session_id, reason_code).
2. **Security Sanitization & Traversal Defenses (`common/include/nstu/file_distribution.hpp`, `common/src/file_distribution.cpp`)**:
   - `is_safe_filename(std::string_view filename) noexcept`:
     - Disallows empty or oversized (>255 byte) filenames.
     - Disallows leading or trailing spaces and dots (preventing NTFS auto-trim bypasses).
     - Disallows path separators (`/`, `\`), directory tokens (`.`, `..`), control characters (< 0x20, 0x7F), and Windows forbidden characters (`:`, `*`, `?`, `"`, `<`, `>`, `|`). The colon rejection explicitly blocks NTFS Alternate Data Streams (e.g. `file.exe:$DATA`).
     - Extracts the stem before the first dot and performs case-insensitive rejection of all 22 Windows DOS reserved device names (`CON`, `PRN`, `AUX`, `NUL`, `COM1-9`, `LPT1-9`).
   - `sanitize_filename(std::string_view filename)`:
     - Strips any preceding directory components, normalizes forbidden characters to `'_'`, trims whitespace/dots, prepends `"safe_"` to reserved device stems, and provides a safe fallback `"received_file.bin"`.
   - `compute_staging_path(const std::filesystem::path& base_dir, std::string_view filename)`:
     - Validates filename safety and enforces lexical containment inside `base_dir` using `lexically_relative`.
3. **Data Types & Binary Codecs (`common/include/nstu/file_distribution.hpp`, `common/src/file_distribution.cpp`)**:
   - Defined `FileDistStatus` enum with bilingual labels (`in_progress`, `completed`, `hash_mismatch`, `write_error`, `cancelled`, `rejected_exam_active`, `rejected_unsafe_path`, `rejected_quota_exceeded`).
   - Enforced bounding constants: `kMaxFileSize = 64 MiB`, `kMaxChunkSize = 64 KiB`, `kMaxFilenameLength = 255`.
   - Implemented binary encoders and decoders with strict validation, length checking, and signedness-clean comparisons.
4. **Verification & Tests (`tests/file_distribution_tests.cpp`, `tests/CMakeLists.txt`)**:
   - Comprehensive test suite covering 10 distinct test areas: safe/unsafe filenames, traversal attempts, NTFS ADS, DOS device names, sanitization, staging containment, all 5 message codecs, status labels, and SHA-256 verification/mismatch checks.

---

## 2. File Allowlist & Hashes

| Path | Pre-Implementation SHA-256 | Post-Implementation SHA-256 |
|---|---|---|
| `common/include/nstu/protocol.hpp` | `E333D5F48D8FA6883CC69444AC3183DC615E040EB50E4B0A513CDC82C2DED470` | `AC061A0A9324686393B7C6417B0DA323DD6F9ED0E311E02F0DB9BABED1C2049F` |
| `common/include/nstu/file_distribution.hpp` | `NEW_FILE` | `2F78F2B6DC08671AEC25C0A88E78C7E411472EA5675F3EE3BEEAA5196DAEBE29` |
| `common/src/file_distribution.cpp` | `NEW_FILE` | `FC6D84A93B89CEF40CFB3665537B8D28975081E1C1F5C128C70EA02BA95503B8` |
| `common/CMakeLists.txt` | `A5AB28445BF1D33F563FB0428735F30F7040E9C2427ACF9A3EAB0141ACA65829` | `7AF602761C4AB8AED35C201CCACA59E812710615F1425EEB8B86B98BC4A17D5E` |
| `tests/CMakeLists.txt` | `7CE9AEC0EA3E7AA36E70C915F175329B1ECD681A5CA6EA1E145ECD468F921CB7` | `BFC7D18D7D3FF1AFB2110F387E923E1D0FCEC363470CD6F08D1DE8FD5AB7480B` |
| `tests/file_distribution_tests.cpp` | `NEW_FILE` | `616136C20F49143BC4DFA2A570C48066471368B1AB82251851E8CA32CC473763` |

---

## 3. Verification Receipts

- Focused Unit Tests (`nstu_file_distribution_tests.exe`): **PASS** (10/10 test categories passed).
- Fast Verification Gate (`./scripts/verify-gate.sh`): **PASS** (100% tests passed, 0 failures across 67 test suites, zero compiler warnings).
- Strict Verification Gate (`./scripts/verify-gate.sh --strict`): **PASS** (Release mode with `-Werror`, 100% tests passed across 67 test suites, zero compiler warnings).
- Clean working directory outside allowlisted files. No assertions removed or weakened.

