# Task Result: STUDENT-HELP-REQUEST-CORE-1

## 1. Task Metadata
- **Task ID**: `STUDENT-HELP-REQUEST-CORE-1`
- **Roadmap Ref**: `REVISED_ROADMAP.md` Priority 2 / P2.3 (Daily Teaching Workflows / Help Request Core)
- **Branch**: `feature/student-help-request-core`
- **Target Integration**: `main` (INDEV)
- **Author**: `khoasoma <songtu438@gmail.com>`

## 2. Summary of Implementation
- **Protocol wire commands**: Assigned `CommandType::help_request = 58` and `CommandType::help_ack = 59` in `common/include/nstu/protocol.hpp`.
- **Binary codecs**: Implemented `HelpRequestMessage` and `HelpAckMessage` codecs in `common/include/nstu/control_messages.hpp` and `common/src/control_messages.cpp` with strict 256-byte message length bounds checking and endian safety (`read_le` / `append_le`).
- **Client Registry**: Added `help_requested`, `help_message`, `help_requested_at_unix_ms` fields in `ClientRecord`. In `ClientRegistry::upsert()`, existing help request fields are copied forward to ensure periodic client status reports never erase active hand raises. Added thread-safe `ClientRegistry::set_help_request()`.
- **Control Plane**: Integrated `help_request` command dispatch in `ServerControlPlane` inbound message handling and implemented `ServerControlPlane::resolve_help_request()` which updates local registry and transmits `help_ack` to the target client.
- **Classroom Roster**: Added `RoomFilter::help = 5`, `RoomCounts::help`, and `BulkActionKind::acknowledge_help = 3` with full bilingual English and Vietnamese formatting in `server/include/nstu/classroom_roster.hpp` and `server/src/classroom_roster.cpp`. Maintained orthogonal count invariants (`online + attention + locked + offline == total`).
- **Tests**: Added `test_help_request_roster_and_codecs()` in `tests/classroom_roster_tests.cpp` covering codec serialization/deserialization, oversized payload rejection, truncated framing rejection, room filtering and counting, registry status upsert persistence, and bulk action localization formatters.

## 3. Source Hashes (SHA-256)
- `common/include/nstu/control_messages.hpp`: `DC5FC44A54D966FE34FDFE3C025B8DC4EB1F4BCE4737E6D547718DCD72CA0DBB`
- `common/include/nstu/protocol.hpp`: `5E0585DFF93E8AE13EC0971073A6304B9F14AF4AF6A72F25FB303CD6C9CA6F14`
- `common/src/control_messages.cpp`: `AD036108D0295FB5970D2107C12439DCDFD7BFF7FA85D313EA9BB28E827C0DF4`
- `server/include/nstu/classroom_roster.hpp`: `F9B4B79B0EA63F872D96FBD8D7564D3B09533D633FDC3F18189C42B3902EFBB3`
- `server/include/nstu/client_registry.hpp`: `8E5A4EA80F76EB8386C7CD04E0485C72E75B0E853422B92AC27553C9F7062964`
- `server/include/nstu/control_plane.hpp`: `9A382D05AD1817D949FAA608695D0C123594D1C71C0790A1633D8C00BF69B94D`
- `server/src/classroom_roster.cpp`: `4D250B968BB066377486FA094F758E2AF3317133B4131CED6DCB16BB1CB9AF7E`
- `server/src/client_registry.cpp`: `17347A89BAF917CACAC5D5D41915E72AF7667AA99A5AF5982E30EF7BD0EB6558`
- `server/src/control_plane.cpp`: `D7EFF0D7C0FD37636483CC0F7FCCF675D0B5461159366FB05DFA2C5A53D4FEA4`
- `tests/classroom_roster_tests.cpp`: `E1161873D388730EB9BDB31458F23F08270F9E5F8628233BFCB8536CC457F0F9`

## 4. Verification Receipts
- **Fast Gate**: `./scripts/verify-gate.sh`
  - Zero banned filenames / target names / identifiers outside string literals
  - No forbidden paths staged
  - No weakened or removed assertions (63 test files verified)
  - Node syntax & exam web recovery tests: 63 passed
  - C++ build: Succeeded with 0 warnings
  - C++ unit tests: 63/63 passed (100%)
- **Strict Gate**: `./scripts/verify-gate.sh --strict`
  - Release build with `-DNSTU_ENABLE_WERROR=ON`
  - Zero compiler warnings
  - C++ unit tests: 63/63 passed (100%)
- **Independent Non-Author Code Review**:
  - Subagent: `c01b5e9e-0ac2-486f-b3fa-ad74652ee548`
  - Role: Independent Non-Author Code Reviewer
  - Report Verdict: **ACCEPT**

## 5. Next Steps
- Commit allowlisted files to `feature/student-help-request-core`.
- Push feature branch to `origin/feature/student-help-request-core` and monitor CI.
- Fast-forward merge into `main` as INDEV under human integration rules.
- Verify Windows CI passes all 5 jobs on `main`.
