# Task Plan: WEBSITE-QUICK-LAUNCH-CORE-1

## 1. Context & Objectives
- **Task ID**: `WEBSITE-QUICK-LAUNCH-CORE-1`
- **Roadmap Ref**: `REVISED_ROADMAP.md` Priority 2 / P2.3: "Daily teaching workflows: Website and Quick Launch: validated URLs/launch definitions; never arbitrary shell execution in Teacher mode."
- **Branch**: `feature/website-quick-launch-core`
- **Base Commit**: `55d0c37`
- **Prerequisite**: `STUDENT-HELP-REQUEST-CORE-1` (`7477902`) and `STUDENT-HELP-REQUEST-UI-1` (`2e5155d`) merged to `main`.
- **Objectives**:
  1. Build a strict, pure URL validation and sanitization library (`common/include/nstu/url_validation.hpp` and `common/src/url_validation.cpp`) preventing arbitrary shell execution, dangerous URI schemes, control character injection, or buffer overflows.
  2. Introduce protocol wire command `CommandType::launch_url = 60` in `common/include/nstu/protocol.hpp`.
  3. Implement `LaunchUrlMessage` binary codecs in `common/include/nstu/control_messages.hpp` and `common/src/control_messages.cpp`.
  4. Implement `ServerControlPlane::launch_url` in `server/include/nstu/control_plane.hpp` and `server/src/control_plane.cpp`.
  5. Add `BulkActionKind::launch_url = 4` in `server/include/nstu/classroom_roster.hpp` and `server/src/classroom_roster.cpp` with bilingual (EN/VI) confirmation and status formatters.
  6. Deliver comprehensive unit test coverage in `tests/url_validation_tests.cpp` integrated into `tests/CMakeLists.txt`.

## 2. Baseline Source Hashes (SHA-256)
- `common/CMakeLists.txt`: `C731CA9876EA30C7F86D45D3C211743A0F877489561F36F6AF281B9C7E5E09BE`
- `common/include/nstu/protocol.hpp`: `5E0585DFF93E8AE13EC0971073A6304B9F14AF4AF6A72F25FB303CD6C9CA6F14`
- `common/include/nstu/control_messages.hpp`: `DC5FC44A54D966FE34FDFE3C025B8DC4EB1F4BCE4737E6D547718DCD72CA0DBB`
- `common/src/control_messages.cpp`: `AD036108D0295FB5970D2107C12439DCDFD7BFF7FA85D313EA9BB28E827C0DF4`
- `server/include/nstu/control_plane.hpp`: `9A382D05AD1817D949FAA608695D0C123594D1C71C0790A1633D8C00BF69B94D`
- `server/src/control_plane.cpp`: `D7EFF0D7C0FD37636483CC0F7FCCF675D0B5461159366FB05DFA2C5A53D4FEA4`
- `server/include/nstu/classroom_roster.hpp`: `F9B4B79B0EA63F872D96FBD8D7564D3B09533D633FDC3F18189C42B3902EFBB3`
- `server/src/classroom_roster.cpp`: `4D250B968BB066377486FA094F758E2AF3317133B4131CED6DCB16BB1CB9AF7E`
- `tests/CMakeLists.txt`: `FE7A197D0311DF367490C4E6B8AD2F912178CB541852C66716F08ADC06D876C6`

## 3. Allowlisted File Paths
Only the following files are permitted to be modified or created:
1. `common/include/nstu/url_validation.hpp` (new)
2. `common/src/url_validation.cpp` (new)
3. `common/CMakeLists.txt`
4. `common/include/nstu/protocol.hpp`
5. `common/include/nstu/control_messages.hpp`
6. `common/src/control_messages.cpp`
7. `server/include/nstu/control_plane.hpp`
8. `server/src/control_plane.cpp`
9. `server/include/nstu/classroom_roster.hpp`
10. `server/src/classroom_roster.cpp`
11. `tests/url_validation_tests.cpp` (new)
12. `tests/CMakeLists.txt`
13. `tasks/task-plans/website-quick-launch-core-1.md` (new)
14. `tasks/task-plans/website-quick-launch-core-1-result.md` (new)

## 4. Acceptance Criteria
1. **URL Validation Engine**:
   - `is_valid_launch_url(std::string_view url, std::string* error = nullptr)` accepts only `http://` and `https://` schemas (case-insensitive).
   - Rejects all other schemas, explicitly testing: `file:`, `javascript:`, `data:`, `vbscript:`, `ms-settings:`, `cmd:`, `powershell:`, `rundll32:`.
   - Strictly enforces length bound: `kMaximumUrlBytes = 2048`.
   - Rejects whitespace, control characters, null bytes, quotes, backticks, angle brackets, and shell injection symbols.
   - Requires non-empty host domain/IP.
   - `sanitize_launch_url(std::string_view raw_url)` provides trimming and scheme prefixing for user convenience.
2. **Protocol Command & Codec**:
   - `CommandType::launch_url = 60` defined in `common/include/nstu/protocol.hpp`.
   - `LaunchUrlMessage` struct and `encode_launch_url` / `decode_launch_url` functions in `control_messages.hpp`.
   - Length-prefixed serialization with payload bounds checking and validation rejection of malformed or unsafe URLs.
3. **Control Plane Dispatch**:
   - `ServerControlPlane::launch_url(client_id, url, error)` validates URL, verifies target client is connected, encodes payload, and sends `CommandType::launch_url`.
4. **Classroom Roster & Bulk Action**:
   - `BulkActionKind::launch_url = 4` added to `classroom_roster.hpp`.
   - `execute_bulk_action` executes URL launch across targeted clients with backward compatibility for existing callers.
   - Bilingual confirmation title, message, and result status formatters implemented in English and Vietnamese.
5. **Test Coverage & Verification**:
   - Dedicated unit test binary `nstu_url_validation_tests` in `tests/CMakeLists.txt`.
   - Full test coverage for validation edge cases, bypass attempts, codec roundtrips, and bulk action formatters.
   - Fast gate (`./scripts/verify-gate.sh`) and strict gate (`./scripts/verify-gate.sh --strict`) pass with zero compiler warnings and zero removed assertions.
   - Independent non-author subagent review verdict ACCEPT.

