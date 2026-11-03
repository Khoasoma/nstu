# Task Result: FLEET-ROOM-PROFILE-CORE-1

**Task ID**: `FLEET-ROOM-PROFILE-CORE-1`  
**Phase**: Priority 3 / P3.1 Fleet Health & Privacy Gates (Portable Secret-Free Room Profile Data Model, Export/Import Engine & Canary Rejection)  
**Status**: Implemented, Verified & Under Independent Code Review  
**Branch**: `feature/fleet-room-profile-core`  
**Base Commit**: `611140c`  

---

## 1. Summary of Changes

Implemented the core data model, canonical serialization, fail-closed validation, and secret-canary rejection engine for portable NSTU Room Profiles:

1. **Room Profile Data Model (`common/include/nstu/room_profile.hpp`)**:
   - `RoomProfile`: Captures room name, description, institution, grid dimensions (`grid_columns`, `grid_rows`), seat assignments (`SeatAssignment`), authorized website presets, authorized application presets, default English and Vietnamese lock screen messages, network discovery subnet hint, and beacon interval.
   - Strictly portable classroom configuration only — zero credentials, zero pairing keys, zero enrollment secrets, zero student identity, zero exam answers, and zero telemetry buffers.
   - `RoomProfileResult` and `RoomProfileError` with structured error codes (`ok`, `invalid_json`, `oversized_payload`, `unsupported_schema`, `forbidden_secret_detected`, `validation_failed`, `missing_required_field`).

2. **Validation & Preset Sanitization (`common/src/room_profile.cpp`)**:
   - `is_valid_website_preset`: Strict URL validation (HTTP/HTTPS only, no userinfo credentials e.g. `user:pass@`, bounded $\le 256$ chars, rejects dangerous schemes `javascript:`, `file:`, `data:`, `blob:`).
   - `is_valid_application_preset`: Validates application presets (filename only, bounded $\le 64$ chars, rejects relative `..` or path separators `/` or `\\`, requires executable extension `.exe`).
   - `is_valid_hostname`: RFC 1123 compliant computer hostnames (alphanumeric, dots, hyphens, bounded $\le 32$ chars, no trailing or leading symbols, no double dots).

3. **Negative Boundary & Secret Canary Rejection (`common/src/room_profile.cpp`)**:
   - `contains_forbidden_profile_secrets`: Scans JSON keys and values for forbidden secret keywords: `"key"`, `"secret"`, `"token"`, `"password"`, `"private"`, `"entropy"`, `"student_answer"`, `"ticket"`, `"sas"`, `"pairing_key"`, `"credential"`, `"bearer"`, `"psk"`.
   - Fails closed immediately with `RoomProfileError::forbidden_secret_detected`.

4. **Canonical Export & Import Engine (`common/src/room_profile.cpp`)**:
   - `export_room_profile_json`: Exports a `RoomProfile` to canonical, cleanly formatted JSON with ISO 8601 UTC creation timestamp (using thread-safe, MSVC-safe `gmtime_s` / `gmtime_r`) and schema version `1.0`.
   - `import_room_profile_json`: Bounded parser ($\le 64$ KiB) enforcing schema validation, range checks (grid dimensions $[1, 16]$, beacon interval $[1, 60]$, max 256 seats, max 64 website presets, max 64 app presets), string length limits, and seat coordinate bounds against grid dimensions.

5. **Unit & Negative Boundary Test Suite (`tests/room_profile_tests.cpp`)**:
   - `test_default_profile_export_and_import_roundtrip`: Verifies default profile serialization and parsing fidelity.
   - `test_populated_profile_roundtrip`: Verifies roundtrip equality with all optional fields, multiple seats, website presets, application presets, and localized lock messages.
   - `test_negative_boundary_secret_rejection`: Asserts immediate fail-closed rejection across 18 distinct canary patterns and secret-bearing payloads.
   - `test_preset_validation`: Validates websites and executable filenames against attack vectors.
   - `test_hostname_validation`: Validates RFC 1123 hostname compliance.
   - `test_oversized_payload_rejection`: Asserts rejection of payloads $> 64$ KiB.
   - `test_bounds_validation`: Asserts range limits on grid columns, rows, beacon interval, and seat coordinates.
   - `test_missing_required_fields_and_schema`: Asserts proper error codes on missing room name, empty room name, bad schema version, and invalid JSON syntax.

---

## 2. File Allowlist Verification

| Path | Status | Description |
|---|---|---|
| `common/include/nstu/room_profile.hpp` | NEW | Room profile data model and export/import declarations |
| `common/src/room_profile.cpp` | NEW | JSON serializer, bounded parser, preset validators, and secret canary rejection |
| `common/CMakeLists.txt` | MODIFIED | Registered `src/room_profile.cpp` in `nstu_common` |
| `tests/CMakeLists.txt` | MODIFIED | Registered `nstu_room_profile_tests` target and test |
| `tests/room_profile_tests.cpp` | NEW | Unit and negative boundary test suite |

Strictly 5 files ($\le 5$ limit respected).

---

## 3. Verification Gate Results

- **Fast Verification Gate (`./scripts/verify-gate.sh`)**:
  - `PASS no banned filenames`
  - `PASS no banned CMake target names`
  - `PASS no banned identifiers outside string literals`
  - `PASS artifacts/, .claude/, HANDOFF.md not staged`
  - `PASS no assertions removed from tests/`
  - `PASS node --check exam/web/app.js`
  - `PASS exam web recovery tests: 63 passed`
  - `PASS build succeeded (0 compiler warnings)`
  - `PASS 100% tests passed, 0 tests failed out of 69`
- **Strict Verification Gate (`./scripts/verify-gate.sh --strict`)**:
  - `PASS build succeeded (Release + Werror, 0 compiler warnings)`
  - `PASS 100% tests passed, 0 tests failed out of 69`

