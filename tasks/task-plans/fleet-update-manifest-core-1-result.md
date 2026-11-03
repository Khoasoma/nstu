# Task Result: FLEET-UPDATE-MANIFEST-CORE-1 (Remediated)

## Summary
- **Task ID**: `FLEET-UPDATE-MANIFEST-CORE-1`
- **Goal**: Implement signed update manifest data model, asymmetric ECDSA P-256 digital signature signing and verification (with client only requiring public trust root key), canonical signing payload including `rollout_percentage`, anti-downgrade and equal-version `already_up_to_date` rejection engine, fail-closed JSON parser with trailing garbage, comma validation, integer overflow protection and required fields checks, and deterministic canary rollout bucketing per `docs/AUTO_UPDATE_LTSC.md`.
- **Status**: Remediated and verified against all review findings.

## Remediations Addressed
1. **Asymmetric Cryptographic Signing (Criterion 4)**:
   - Implemented standard Windows BCrypt asymmetric ECDSA P-256 (`BCRYPT_ECDSA_P256_ALGORITHM`).
   - Private key signs the SHA-256 digest of the canonical payload via `BCryptSignHash`, producing a 64-byte raw signature (128 hex chars).
   - Client endpoint verifies via `BCryptVerifySignature` requiring *only* the public trust root key (`BCRYPT_ECCPUBLIC_BLOB` or raw 64 bytes). The private key never leaves the build worker / release signer.
   - Added `generate_update_signing_keypair()` helper for release tooling and test harnesses.
2. **Canonical Signing Payload**:
   - `build_canonical_manifest_signing_payload()` now explicitly binds `rollout_percentage`.
   - Unit test verifies tampering with `rollout_percentage` invalidates signature verification.
3. **Anti-Downgrade & Equal-Version Engine (Criterion 2)**:
   - Added `ManifestVerificationError::already_up_to_date`.
   - Rejects candidate version < installed version (`downgrade_detected`).
   - Rejects candidate version == installed version (`already_up_to_date`) unless `allow_same_version = true`.
4. **JSON Parser Fail-Closed Hardening (Criterion 5)**:
   - Trailing garbage check: verifies EOF after closing `}`.
   - Commas strictly enforced: trailing commas before `}` and missing commas between keys are rejected as `invalid_json`.
   - Checked return value of `skip_value()`.
   - Overflow protection in `parse_uint64`.
   - Enforced all mandatory fields per `docs/AUTO_UPDATE_LTSC.md`.
5. **Canary Rollout Engine & Naming (Criterion 3)**:
   - Exposes `compute_canary_bucket` (and alias `compute_canary_rollout_bucket`).
   - Fails closed (`std::nullopt`) if `machine_guid` is empty.
   - Gating in `verify_manifest_against_environment` defers clients when bucket >= rollout_percentage.

## Verification Receipts
- **Fast Gate**: `./scripts/verify-gate.sh`
  - PASS zero compiler warnings.
  - PASS zero weakened test assertions.
  - PASS 100% tests passed (70/70 suites).
- **Strict Gate**: `./scripts/verify-gate.sh --strict`
  - PASS zero compiler warnings under `-Wall -Wextra -Wpedantic -Werror` in Release mode.
  - PASS 100% tests passed (70/70 suites).
