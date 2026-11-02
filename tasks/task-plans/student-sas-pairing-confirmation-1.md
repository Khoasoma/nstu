# Task Plan: Student SAS Pairing Confirmation & Mutual Verification

- **Task Identifier**: `STUDENT-SAS-PAIRING-CONFIRMATION-1`
- **Stream**: Stream S3 / Roadmap P0-03 (Discovery-First Client Setup & Student Confirmation)
- **Component**: `common`, `client`, `tests`
- **Allowlisted Files**:
  - `common/include/nstu/pairing.hpp`
  - `common/src/pairing.cpp`
  - `client/include/nstu/client_pairing.hpp`
  - `client/src/client_pairing.cpp`
  - `tests/client_pairing_tests.cpp`
  - `tests/ui_pairing_tests.cpp`
  - `tests/CMakeLists.txt`
  - `tasks/task-plans/student-sas-pairing-confirmation-1.md`

---

## 1. Objectives & Scope

Enforce the Hard Rule from `AGENTS.md`:
> *"Trust between server and client is anchored by a six-digit pairing code (SAS). Treat it as the security root: never auto-approve a pairing without the code being confirmed on both machines."*

Specifically:
1. **SAS Formatting Helper**:
   - `format_short_authentication_string(sas)`: Formats 6-digit SAS as `"XXX YYY"` with a space for clear visual comparison on student and teacher screens.
2. **Student Confirmation Evaluation Logic**:
   - `StudentConfirmationChoice`: Enum representing user choice or state (`confirmed`, `rejected`, `mismatched_code`, `timed_out`, `no_ui`, `waiting`).
   - `StudentConfirmationDecision`: Enum (`save`, `refuse`).
   - `evaluate_student_confirmation`: Pure decision function that requires `code_shown == true`, `choice == confirmed`, and matching non-empty codes. Any timeout, mismatch, rejection, or missing UI fails closed as `refuse`.
3. **Client Pairing Flow Hardening**:
   - In `pair_with_server`, require `code_observer` (fails closed immediately if null, e.g. Session 0 with no interactive display).
   - In `pair_with_server`, invoke `options.student_confirmation` in non-blocking slices (max 250 ms) while maintaining heartbeat keepalives so the server's TCP connection does not time out while the student verifies the code.
   - Client sends its confirmation tag and commits keys to DPAPI storage **only after** positive student confirmation.
   - If the student rejects or mismatches the code, the client aborts with `PairingOutcome::student_refused` and zeroizes all key material.
4. **Unit & Integration Test Coverage**:
   - Update `tests/client_pairing_tests.cpp` with confirmation callbacks and a test proving failure when confirmation is omitted.
   - Add `tests/ui_pairing_tests.cpp` covering confirm, reject, timeout, no UI, mismatched code, SAS formatting, choice text, fail-closed without code observer, and end-to-end pairing with student confirmation.
   - Add `nstu_ui_pairing_tests` to `tests/CMakeLists.txt`.

---

## 2. Verification Protocol

- Fast verification gate: `scripts/verify-gate.sh` (0 warnings, 100% test pass).
- Strict verification gate: `scripts/verify-gate.sh --strict` (MSVC / Release + `-Werror`, 0 warnings, 100% test pass).
- Push to `origin feature/student-sas-pairing-confirmation`.
- 4-job Windows CI on GitHub Actions.
- Merge to `main` via fast-forward and push.
- 5-job release workflow on `main`.

