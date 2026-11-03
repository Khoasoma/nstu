# Task Plan: FLEET-UPDATE-STAGING-RECOVERY-1

## 1. Context & Motivation
Per `REVISED_ROADMAP.md` (Priority 3 / P3.2 Signed Update Pipeline and Canary) and `docs/AUTO_UPDATE_LTSC.md`:
Following `FLEET-UPDATE-MANIFEST-CORE-1` (which established the signed update manifest, asymmetric ECDSA P-256 verification, canonical signing payload, anti-downgrade engine, and deterministic canary rollout bucketing), this task implements the **Client Update Staging and Recovery State Machine** (`nstu::update::UpdateStagingController`):
- State enumeration (`idle`, `checking`, `deferred`, `downloading`, `verifying`, `staged`, `apply_pending_reboot`, `validating`, `committed`, `rolling_back`, `failed`).
- Protected staging lifecycle with atomic chunk streaming to `.part` files, continuous SHA-256 validation, and fail-closed deletion upon corruption or abort.
- Transactional state record (`StagingRecord`) serialization and deserialization with backup directory management for reliable rollback when post-update health checks fail.
- Deep Freeze cooperation and reboot-bound deployment semantics.

## 2. Allowlisted Files
1. `common/include/nstu/update_staging.hpp` (New)
2. `common/src/update_staging.cpp` (New)
3. `common/CMakeLists.txt` (Modify: register `src/update_staging.cpp`)
4. `tests/CMakeLists.txt` (Modify: register `tests/update_staging_tests.cpp`)
5. `tests/update_staging_tests.cpp` (New: comprehensive unit tests)

### Baseline SHA-256 Hashes
- `common/CMakeLists.txt`: `d4ba30d320db1f39357f0231fbb160db8d33a0377cb3849460138538f2611d9d`
- `tests/CMakeLists.txt`: `9933890e3ce30cec8c53b86b5d37c8753ebb874ecd4fc91fecf334b6ea6d6f17`

## 3. Acceptance Criteria
1. **State Machine Transitions**:
   - Implements full state machine: `idle` -> `downloading` -> `staged` -> `apply_pending_reboot` -> `validating` -> `committed` / `rolling_back`.
   - Transitions strictly follow `docs/AUTO_UPDATE_LTSC.md`.
2. **Atomic Staging & SHA-256 Verification**:
   - Streams chunks into `.part` temporary file while incrementally hashing.
   - Enforces byte limit: exceeding `expected_bytes` fails closed and purges `.part` file.
   - `finalize_staging()` verifies complete SHA-256 digest against manifest `sha256_hex`. Mismatch fails closed and deletes partial files.
3. **Transactional Backup & Rollback**:
   - `apply_staged_update()` moves active directory to backup directory before moving staged package to active.
   - `rollback_update()` atomically restores backup directory when health check fails.
   - `commit_update()` purges backup directory once health check passes.
4. **Durable Staging State Record**:
   - Saves `staging_record.json` in staging directory with canonical JSON format.
   - Recovers state cleanly across service restart.
5. **Quality & Assertion Baseline**:
   - 0 compiler warnings under MSVC `/W4 /WX` and GCC `-Wall -Wextra -Wpedantic -Werror`.
   - Preserve all existing assertions across test suites.
   - Local fast gate (`verify-gate.sh`) and strict gate (`verify-gate.sh --strict`) pass.

## 4. Test Strategy
- Unit test suite `tests/update_staging_tests.cpp`:
  - `test_staging_state_transitions_happy_path`: idle -> download -> stage -> apply -> validate -> commit.
  - `test_staging_sha256_mismatch_fails_closed`: corrupt payload aborts, deletes .part file.
  - `test_staging_byte_overflow_fails_closed`: exceeding byte_length aborts.
  - `test_staging_premature_finalize_fails_closed`: truncated payload aborts.
  - `test_staging_rollback_recovery`: simulate validation failure, verify backup restored.
  - `test_staging_state_record_persistence`: JSON serialization/deserialization across restart.

