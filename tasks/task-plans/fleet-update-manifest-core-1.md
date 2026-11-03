# Task Plan: FLEET-UPDATE-MANIFEST-CORE-1

**Task ID**: `FLEET-UPDATE-MANIFEST-CORE-1`  
**Phase**: Priority 3 / P3.2 Signed update pipeline & canary (Update Manifest Data Model, Cryptographic Verification & Anti-Downgrade Engine)  
**Roadmap Reference**: `REVISED_ROADMAP.md` (P3.2: "Before UI work, specify: signing trust root, offline/root key and rotation; artifact hash/signature verification; anti-downgrade and replay protection; protocol/schema compatibility and minimum supported versions") and `tasks/product-execution-plan.md` (`FLEET-03/04`: "Signed trust/rotation/hash/anti-downgrade and resumable staging/recovery, then canary health gates/pause/rollback and sanitized history.") and `docs/AUTO_UPDATE_LTSC.md`.

---

## 1. Description & Architectural Rationale

This task implements the cryptographic manifest foundation, semantic versioning, anti-downgrade protection, and client environment compatibility evaluator for the NSTU update pipeline in `nstu_common`:

1. **Semantic Versioning (`SemanticVersion` in `common/include/nstu/update_manifest.hpp`)**:
   - Structured `major`, `minor`, `patch` with three-way comparison (`<=>`), string formatting (`to_string()`), and robust parsing from string views.
   - Strict monotonic version ordering to prevent downgrade attacks and establish stepping-stone upgrade paths.

2. **Update Manifest Data Model (`UpdateManifest` in `common/include/nstu/update_manifest.hpp`)**:
   - Encapsulates: `schema_version` ("1.0"), `package_role` (`client` vs `server`), candidate `version`, `min_supported_version`, `package_url` (HTTP/HTTPS URL), `byte_length`, `sha256_hex` (64-character lowercase hex digest), `release_channel` (`ltsc_stable`, `security`, `pilot`), `release_timestamp_utc`, `rollout_percentage` ($[0, 100]$), `min_windows_build` (e.g. 17763 for LTSC 2019), `requires_reboot`, `key_id` (rotation identifier), and `signature_hex`.
   - Strictly role-specific artifacts: clients cannot update to server packages, and vice versa.

3. **Cryptographic Signing & Verification Engine (`common/src/update_manifest.cpp`)**:
   - Canonical signing payload: Deterministic format concatenating critical release fields (`schema`, `role`, `version`, `min_supported_version`, `url`, `size`, `sha256`, `channel`, `timestamp`, `min_build`, `key_id`).
   - `sign_update_manifest`: Computes HMAC-SHA256 signature using `nstu::security::hmac_sha256` over the canonical payload.
   - `verify_manifest_signature`: Constant-time verification (`nstu::security::constant_time_equal`) against the release trust root key.

4. **Environment Compatibility & Anti-Downgrade Engine (`common/src/update_manifest.cpp`)**:
   - `ClientEnvironment`: Represents current machine state (`installed_role`, `current_version`, `current_windows_build`, `machine_guid`).
   - `verify_manifest_against_environment`: Comprehensive evaluation:
     - Rejects bad signatures (`invalid_signature`).
     - Rejects role mismatch between installed role and package role (`role_mismatch`).
     - Rejects version downgrades (`downgrade_detected`).
     - Rejects obsolete client versions that do not satisfy `min_supported_version` (`below_minimum_supported_version`).
     - Rejects incompatible OS versions below `min_windows_build` (`incompatible_os_build`).
     - Evaluates deterministic canary rollout: hashes machine GUID to compute bucket $[0, 99]$; checks against `rollout_percentage` (`rollout_deferred`).
     - Rejects malformed SHA-256 digests or invalid package URLs.

5. **Canonical JSON Serialization & Parser (`common/src/update_manifest.cpp`)**:
   - `export_update_manifest_json`: Formats clean canonical JSON representation of the manifest.
   - `import_update_manifest_json`: Bounded parser ($\le 64$ KiB) enforcing schema version, field bounds, and syntax integrity.

6. **Unit Test Suite (`tests/update_manifest_tests.cpp`)**:
   - Exhaustive tests for semantic version parsing/ordering, signing and constant-time verification, anti-downgrade enforcement, role mismatch rejection, OS build checks, canary rollout gating, and JSON roundtrip fidelity.

---

## 2. Acceptance Criteria

1. **Monotonic Version Comparison**: `SemanticVersion` accurately parses versions and enforces strict ordering ($1.0.0 < 1.0.1 < 1.1.0 < 2.0.0$).
2. **Cryptographic Authenticity**: Unsigned manifests, modified payloads, or incorrect keys fail closed with `invalid_signature`.
3. **Anti-Downgrade Invariant**: Manifests proposing a version lower than the installed version are rejected with `downgrade_detected`.
4. **Role & Prerequisite Isolation**: Rejects role transitions (`client` $\leftrightarrow$ `server`), OS builds below requirement, and versions below `min_supported_version`.
5. **Deterministic Canary Staging**: Rollout percentage deterministically admits or defers clients based on machine GUID bucket hash.
6. **Dual Verification Gates**: Zero compiler warnings, 70 of 70 test suites pass cleanly under `./scripts/verify-gate.sh` and `./scripts/verify-gate.sh --strict`.

---

## 3. File Allowlist & Baseline SHA-256 Hashes

| Path | Baseline SHA-256 | Description |
|---|---|---|
| `common/include/nstu/update_manifest.hpp` | *NEW* | Update manifest data model, semantic version, and verification declarations |
| `common/src/update_manifest.cpp` | *NEW* | Signing engine, verification logic, canary rollout evaluator, and JSON parser |
| `common/CMakeLists.txt` | `4AE867030118C8242CD3DB22881845EADFCC1E26FA0A91AD6DF2F81BFEEF622F` | Register `src/update_manifest.cpp` in `nstu_common` |
| `tests/CMakeLists.txt` | `87E5330C7B63CEA763CF8AAF657F956F8C77AECF980FBC8422569D787D67A717` | Register `nstu_update_manifest_tests` target and test |
| `tests/update_manifest_tests.cpp` | *NEW* | Unit test suite for update manifest signing, verification, and anti-downgrade |

Strictly 5 files modified/added ($\le 5$ files limit respected).

---

## 4. Dependencies & Non-Negotiable Invariants

- **Author attribution**: Must commit exclusively as `khoasoma <songtu438@gmail.com>` with no AI assistant trailers.
- **Preserved dirty paths**: Never stage, modify, or revert `README.md`, `README.vi.md`, `docs/assets/banner.png`.
- **Private files**: Never stage or commit `PRODUCT_ROADMAP_NSTU.md`, `artifacts/`, `.claude/`, or `HANDOFF.md`.
- **Zero compiler warnings**: Must compile cleanly under MSVC `/W4 /WX` and GCC `-Wall -Wextra -Wpedantic -Werror`.
- **Preserve all assertions**: Every existing assertion in all 69 test suites must be preserved.

---

## 5. Verification Commands

```bash
# Fast gate
./scripts/verify-gate.sh

# Strict gate
./scripts/verify-gate.sh --strict
```

