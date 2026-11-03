# Task Plan: FLEET-ROOM-PROFILE-CORE-1

**Task ID**: `FLEET-ROOM-PROFILE-CORE-1`  
**Phase**: Priority 3 / P3.1 Fleet Health & Privacy Gates (Portable Secret-Free Room Profile Data Model, Export/Import Engine & Canary Rejection)  
**Roadmap Reference**: `REVISED_ROADMAP.md` (P3.1 and Privacy Gates: "Generic support bundles exclude credentials, enrollment secrets, exam answers... Room profiles separately from previewed, consented, canary-tested support sanitization/export.") and `tasks/product-execution-plan.md` (`FLEET-05`: "Portable secret-free room profiles separately from previewed, consented, canary-tested support sanitization/export.").

---

## 1. Description & Architectural Rationale

This task implements the core data model, canonical serialization, fail-closed validation, and secret-canary rejection engine for portable NSTU Room Profiles in `nstu_common`:

1. **Room Profile Data Model (`common/include/nstu/room_profile.hpp`)**:
   - `RoomProfile`: Captures room name, description, institution, layout grid dimensions (`grid_columns`, `grid_rows`), seat-to-hostname assignments (`SeatAssignment`), authorized website presets, authorized application presets, default English and Vietnamese lock screen messages, network discovery subnet hint, and beacon interval.
   - Strictly portable classroom configuration only — zero device credentials, zero enrollment secrets, zero student identity, zero exam answers, and zero telemetry buffers.

2. **Validation & Preset Sanitization (`common/src/room_profile.cpp`)**:
   - `is_valid_website_preset`: Strictly validates URLs (must have `http://` or `https://` scheme, no userinfo credentials e.g. `user:pass@`, bounded $\le 256$ chars, no dangerous schemes like `javascript:`, `file:`, `data:`).
   - `is_valid_application_preset`: Validates application presets (filename only, bounded $\le 64$ chars, no relative `..` or absolute path separators `/` or `\\`, must have executable extension `.exe`).
   - `is_valid_hostname`: Validates RFC 1123 compliant computer hostnames (alphanumeric, dots, hyphens, bounded $\le 32$ chars).

3. **Negative Boundary & Secret Canary Rejection (`common/src/room_profile.cpp`)**:
   - `contains_forbidden_profile_secrets`: Scans JSON payloads for forbidden secret keywords: `"key"`, `"secret"`, `"token"`, `"password"`, `"private"`, `"entropy"`, `"student_answer"`, `"ticket"`, `"sas"`, `"pairing_key"`, `"credential"`, `"bearer"`, `"psk"`.
   - Any room profile payload containing credential-bearing keys or sensitive assessment fields fails closed immediately with `RoomProfileError::forbidden_secret_detected`.

4. **Canonical Export & Import Engine (`common/src/room_profile.cpp`)**:
   - `export_room_profile_json`: Exports a `RoomProfile` to canonical, cleanly formatted JSON with ISO 8601 UTC creation timestamp and schema version `1.0`.
   - `import_room_profile_json`: Bounded parser ($\le 64$ KiB) enforcing schema validation, range checks (grid dimensions $[1, 16]$, beacon interval $[1, 60]$, max 256 seats, max 64 website presets, max 64 app presets), string length limits, and seat coordinate bounds.

5. **Unit & Negative Boundary Test Suite (`tests/room_profile_tests.cpp`)**:
   - Validates roundtrip export/import fidelity, bounds checking, forbidden secret rejection, malicious preset rejection (traversal, non-HTTP URLs), oversized payload rejection, and schema mismatch handling.

---

## 2. Acceptance Criteria

1. **Roundtrip Fidelity**: Exporting a valid `RoomProfile` to JSON and re-importing yields an identical, valid profile.
2. **Negative Secret Boundary**: Any JSON containing forbidden secret keywords (`key`, `secret`, `token`, `password`, `private`, `entropy`, `student_answer`, `ticket`, `sas`, etc.) in key positions fails closed with `RoomProfileError::forbidden_secret_detected`.
3. **Preset Hardening**: Rejects URLs with non-HTTP schemes or credentials, and application presets with path traversals or directory separators.
4. **Dimension & Count Bounds**: Clamps or validates grid columns $[1, 16]$, grid rows $[1, 16]$, beacon interval $[1, 60]$, seats count $\le 256$, websites count $\le 64$, applications count $\le 64$.
5. **Preserve All Assertions**: All existing assertions across all 68 test suites remain 100% intact.
6. **Dual Verification Gates**: Zero compiler warnings, 69 of 69 test suites pass cleanly under `./scripts/verify-gate.sh` and `./scripts/verify-gate.sh --strict`.

---

## 3. File Allowlist & Baseline SHA-256 Hashes

| Path | Baseline SHA-256 | Description |
|---|---|---|
| `common/include/nstu/room_profile.hpp` | *NEW* | Room profile data model, error codes, and export/import declarations |
| `common/src/room_profile.cpp` | *NEW* | JSON serializer, bounded parser, preset validators, and secret canary rejection |
| `common/CMakeLists.txt` | `F5A579FFEE2369EDE01B7C8DA7096ABA6A636229ED6275218EFDB931831E1C80` | Register `src/room_profile.cpp` in `nstu_common` |
| `tests/CMakeLists.txt` | `2654FAB6A6F84CA0C9D831F4F5657DE915BD37F9F9837576628F6B6411055B83` | Register `nstu_room_profile_tests` test target |
| `tests/room_profile_tests.cpp` | *NEW* | Unit and negative boundary test suite for room profile export/import |

Strictly 5 files modified/added ($\le 5$ files limit respected).

---

## 4. Dependencies & Non-Negotiable Invariants

- **Author attribution**: Must commit exclusively as `khoasoma <songtu438@gmail.com>` with no AI assistant trailers.
- **Preserved dirty paths**: Never stage, modify, or revert `README.md`, `README.vi.md`, `docs/assets/banner.png`.
- **Private files**: Never stage or commit `PRODUCT_ROADMAP_NSTU.md`, `artifacts/`, `.claude/`, or `HANDOFF.md`.
- **Preserve all assertions**: Every existing assertion in all 68 test suites must be preserved.
- **Secret-Free Invariant**: Zero credentials, tokens, keys, entropy, or exam answers in room profiles.

---

## 5. Verification Commands

```bash
# Fast gate
./scripts/verify-gate.sh

# Strict gate
./scripts/verify-gate.sh --strict
```

