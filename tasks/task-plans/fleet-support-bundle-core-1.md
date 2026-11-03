# Task Plan: FLEET-SUPPORT-BUNDLE-CORE-1

**Task ID**: `FLEET-SUPPORT-BUNDLE-CORE-1`  
**Phase**: Priority 3 / P3.1 Fleet Health & Diagnostics (Sanitized Support Bundle Data Model, Redaction Engine & Canary Testing)  
**Roadmap Reference**: `REVISED_ROADMAP.md` (P3.1: "Support bundles require preview/consent and tested sanitization" and Privacy Gates: "Generic support bundles exclude credentials, enrollment secrets, exam answers, screenshots, chat, student files and private browser contents. Test sanitization with canary secrets/student content; offer preview and consent before export.") and `tasks/product-execution-plan.md` (`FLEET-05`: "Portable secret-free room profiles separately from previewed, consented, canary-tested support sanitization/export.").

---

## 1. Description & Architectural Rationale

This task establishes the core data structures, sanitization engine, role/consent evaluator, and markdown report generator for NSTU Support Bundles in `nstu_common`:

1. **Diagnostic Data Model (`common/include/nstu/support_bundle.hpp`)**:
   - `SystemDiagnosticSummary`: Captures application name, version, channel, OS version, operator role, graphics mode, network adapter description, and aggregate fleet counts (total, online, stale, agent connected, UWF protected) without individual student identities or raw MACs.
   - `DiagnosticEvent`: Structured timestamp, severity (`information`, `warning`, `error`), component, and detail.
   - `SupportBundle`: Composed bundle combining summary, event deque, and sanitization verification metadata.

2. **Sanitization & Redaction Engine (`common/src/support_bundle.cpp`)**:
   - `sanitize_diagnostic_text`: Pure string sanitization enforcing:
     - User path anonymization (e.g., `C:\Users\<username>` and `/home/<username>` mapped to `<USERPROFILE>`).
     - Cryptographic secret redaction (32+ char hex hashes/keys, bearer tokens, API keys, passwords, SAS pairing codes).
     - Assessment isolation (student answers, exam tickets, question payloads redacted).
     - Length bounding (truncation with notice) to prevent log-bombing / denial of service.
   - `verify_bundle_canaries`: Fail-closed verification checking sanitized output against a battery of sensitive canary patterns (pre-shared key, SAS code, password, bearer token, exam answer, user profile path). Returns `false` if any sensitive canary escapes redaction.

3. **Role & Consent Evaluation (`common/include/nstu/support_bundle.hpp`)**:
   - `SupportBundleConsentDecision`: `permitted`, `denied_no_consent`, `denied_insufficient_role`.
   - `evaluate_support_bundle_export(OperatorRole role, bool user_consented)`: Enforces that support bundles may only be exported when explicit human consent is granted AND the operator role is `technician` or `developer` (Teachers are protected from accidental data exposure).

4. **Structured Markdown Generation (`common/src/support_bundle.cpp`)**:
   - `build_support_bundle_markdown`: Generates human-readable Markdown with clear privacy notices, summary tables, and sanitized event logs ready for modal preview or support attachment.

5. **Unit & Canary Test Suite (`tests/support_bundle_tests.cpp`)**:
   - Tests summary building, sanitization rules, user path scrubbing, credential/token redaction, canary validation, consent decision matrix, and markdown formatting.

---

## 2. Acceptance Criteria

1. **Negative Privacy Guarantee**: Sanitizer strips passwords, tokens, hex keys, SAS codes, exam answers, and user paths from all bundle fields and events.
2. **Canary Validation**: `verify_bundle_canaries` fails closed if any designated canary string appears in sanitized output.
3. **Role & Consent Enforcement**: Export evaluation strictly requires `user_consented == true` and `role` $\in \{\text{technician}, \text{developer}\}$; denies teachers and unconsented requests.
4. **Markdown Formatting**: `build_support_bundle_markdown` renders clean, human-readable markdown with summary tables and explicit privacy disclaimer.
5. **Preserve All Assertions**: All existing assertions across all 67 test suites remain 100% intact.
6. **Dual Verification Gates**: Zero compiler warnings, 68 of 68 test suites pass cleanly under `./scripts/verify-gate.sh` and `./scripts/verify-gate.sh --strict`.

---

## 3. File Allowlist & Baseline SHA-256 Hashes

| Path | Baseline SHA-256 | Description |
|---|---|---|
| `common/include/nstu/support_bundle.hpp` | *NEW* | Support bundle data model, consent evaluator, and sanitization declarations |
| `common/src/support_bundle.cpp` | *NEW* | Redaction engine, canary verification, and markdown generator |
| `common/CMakeLists.txt` | `7AF602761C4AB8AED35C201CCACA59E812710615F1425EEB8B86B98BC4A17D5E` | Register `src/support_bundle.cpp` in `nstu_common` |
| `tests/CMakeLists.txt` | `BFC7D18D7D3FF1AFB2110F387E923E1D0FCEC363470CD6F08D1DE8FD5AB7480B` | Register `nstu_support_bundle_tests` target and test |
| `tests/support_bundle_tests.cpp` | *NEW* | Unit and canary test suite for support bundle sanitization and consent |

Strictly 5 files modified/added ($\le 5$ files limit respected).

---

## 4. Dependencies & Non-Negotiable Invariants

- **Author attribution**: Must commit exclusively as `khoasoma <songtu438@gmail.com>` with no AI assistant trailers.
- **Preserved dirty paths**: Never stage, modify, or revert `README.md`, `README.vi.md`, `docs/assets/banner.png`.
- **Private files**: Never stage or commit `PRODUCT_ROADMAP_NSTU.md`, `artifacts/`, `.claude/`, or `HANDOFF.md`.
- **Preserve all assertions**: Every existing assertion in all 67 test suites must be preserved.
- **Privacy Guarantee**: Zero leak of student identities, keys, secrets, or exam answers.

---

## 5. Verification Commands

```bash
# Fast verification gate (zero warnings, all 68 test suites pass)
./scripts/verify-gate.sh

# Strict verification gate (Release mode, -Werror, zero warnings)
./scripts/verify-gate.sh --strict
```

