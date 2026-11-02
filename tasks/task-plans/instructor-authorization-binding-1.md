# Task Plan: INSTRUCTOR-SESSION-AUTHORIZATION-BINDING-1

## 1. Context & Motivation
- Device pairing and SAS verification authenticate client devices to the NSTU control plane.
- However, starting an official high-stakes proctored assessment session requires explicit cryptographic authorization from an instructor/proctor credential.
- Without cryptographic instructor authorization tickets, an operator or network actor could launch unauthorized exams or manipulate session parameters without non-repudiation.
- Task 21 introduces `InstructorAuthorizationTicket` with cryptographic HMAC-SHA256 signatures, temporal validity bounds, and cryptographic bindings to the package, package digest, session, and candidate.
- Server admission verifies the ticket before creating `ActiveExamContext`, emitting structured audit events for both successful authorizations and failed/tampered attempts.

## 2. Proposed Architecture & Cryptographic Primitives
1. **Canonical `InstructorAuthorizationTicket`** (`common/include/nstu/exam_auth.hpp`):
   - `instructor_id`: UTF-8 identifier (UUID, email, or badge ID; max 128 bytes).
   - `package_id`: manifest package ID (max 128 bytes).
   - `package_digest`: 32-byte SHA-256 digest of the `.nstuexam` package.
   - `session_id`: 16-byte UUID session identifier.
   - `candidate_id`: student candidate identifier (max 128 bytes; empty if wildcard cohort ticket).
   - `valid_from_ms`: Unix epoch milliseconds when authorization begins.
   - `valid_until_ms`: Unix epoch milliseconds when authorization expires.
   - `instructor_signature`: 32-byte HMAC-SHA256 signature binding all fields.

2. **Canonical Transcript & Signing** (`common/src/exam_auth.cpp`):
   - Computes signable byte stream prefixed by domain separation tag `NSTU-INSTRUCTOR-TICKET-V1`.
   - Binds fixed header, validity timestamps, 32-byte package digest, 16-byte session ID, and length-prefixed text fields.
   - Cryptographically signs with `security::hmac_sha256(key, transcript)`.
   - Validates using constant-time comparison `security::constant_time_equal` to prevent timing attacks.

3. **Wire Codec**:
   - `encode_instructor_ticket()` and `decode_instructor_ticket()`: safe, bounded serialization.

4. **Server Control Plane Admission Binding** (`server/include/nstu/control_plane.hpp`, `server/src/control_plane.cpp`):
   - Configurable `instructor_key` in `ServerControlPlaneConfig` or `set_instructor_key()`.
   - `require_instructor_ticket` policy toggle.
   - Overloaded `start_exam(client_id, request, ticket, error)` and `start_exam(client_id, request, error)`.
   - Checks validity window against server time, verifies HMAC signature, and asserts strict binding against `ExamStartRequest` (package ID, package digest, session ID, candidate ID).
   - Emits structured audit log events (`Category::exam`, `component="exam_auth"`):
     - `info`: `action="ticket_verified"`, `result="authorized"`.
     - `warning`: `action="ticket_verify_failed"`, `result="expired" | "signature_mismatch" | "binding_mismatch"`.
     - `warning`: `action="ticket_missing"`, `result="unauthorized"`.

5. **Test Coverage** (`tests/control_plane_exam_auth_tests.cpp`):
   - Pure codec & cryptographic unit tests: roundtrip encoding/decoding, valid signature, tampered field detection, expired ticket rejection, future ticket rejection, mismatched session/package/candidate rejection.
   - Server integration tests: admission with valid ticket, admission rejection when ticket is expired, tampered, or mismatched, and audit emission verification.

## 3. Allowlisted Files
- `common/include/nstu/exam_auth.hpp`
- `common/src/exam_auth.cpp`
- `common/CMakeLists.txt`
- `server/include/nstu/control_plane.hpp`
- `server/src/control_plane.cpp`
- `tests/control_plane_exam_auth_tests.cpp`
- `tasks/task-plans/instructor-authorization-binding-1.md`

## 4. Verification Plan
- Fast gate: `./scripts/verify-gate.sh` (0 warnings, all unit & web tests pass).
- Strict gate: `./scripts/verify-gate.sh --strict` (MSVC / MinGW Release `-Werror`, 0 warnings).
- Feature CI: GitHub Actions Windows matrix (4 jobs).
- Merge to `main` and Main CI (5 jobs).
