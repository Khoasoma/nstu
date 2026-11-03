# Task Plan: FLEET-SUPPORT-BUNDLE-UI-1

**Task ID**: `FLEET-SUPPORT-BUNDLE-UI-1`  
**Phase**: Priority 3 / P3.1 Fleet Health & Diagnostics (Technician UI Support Bundle Modal, Consent Gate & Live Diagnostics Wiring)  
**Roadmap Reference**: `REVISED_ROADMAP.md` (P3.1: "Support bundles require preview/consent and tested sanitization" and Privacy Gates: "Generic support bundles exclude credentials, enrollment secrets, exam answers, screenshots, chat, student files and private browser contents. Test sanitization with canary secrets/student content; offer preview and consent before export.") and `tasks/product-execution-plan.md` (`FLEET-05`: "Portable secret-free room profiles separately from previewed, consented, canary-tested support sanitization/export.").

---

## 1. Description & Architectural Rationale

Following the completion of `FLEET-SUPPORT-BUNDLE-CORE-1` (which established the data model, negative boundary redaction engine, canary verification, and markdown generator in `nstu_common`), this task wires the support bundle generation, modal preview, consent confirmation, and export actions into the technician and developer surfaces in `server/app/main.cpp`:

1. **Dashboard State & Modal Trigger (`server/app/main.cpp`)**:
   - Add state fields to `DashboardState`:
     - `bool support_bundle_modal_open = false;`
     - `bool support_bundle_consented = false;`
     - `std::string support_bundle_preview;`
   - In `draw_technician_fleet_health`:
     - Add `[Export Support Bundle]` button in the fleet health surface.
     - When clicked, gathers live inventory metrics from `std::vector<ClientRecord>` (total known, online, stale, agent connected, UWF protected) and recent events from `g_diagnostics`.
     - Calls `nstu::support::create_sanitized_bundle` to produce a sanitized bundle.
     - Generates markdown preview via `nstu::support::build_support_bundle_markdown`.
     - Sets `support_bundle_consented = false` (consent is always required fresh on open) and opens modal.

2. **Sanitized Support Bundle Modal (`server/app/main.cpp`)**:
   - Implements `draw_support_bundle_modal`:
     - Displays title: "Export Sanitized Support Bundle" with bilingual EN/VI support.
     - Displays prominent privacy disclaimer informing the operator that credentials, keys, student identities, and exam data are strictly omitted.
     - Mandatory consent checkbox: `[ ] I consent to exporting this sanitized support bundle.`
     - Scrollable markdown preview window showing the full text to be exported.
     - Real-time canary verification indicator: "Canary Verification: PASSED (Clean)".
     - Role & Consent Gated Action Buttons:
       - "Copy to Clipboard": Copies markdown and updates status bar.
       - "Save to File": Writes to `%USERPROFILE%\Documents\NSTU_Support_Bundle_<timestamp>.md` and updates status bar.
       - Disabled when `support_bundle_consented == false` or when role is not permitted (`OperatorRole::teacher` is denied with an explanatory tooltip).
       - "Close" button.

3. **Server State Unit Testing (`tests/server_state_tests.cpp`)**:
   - Adds unit test coverage asserting role permission and consent matrix for support bundle export in the server context.
   - Preserves 100% of existing test assertions across all 68 suites.

---

## 2. Acceptance Criteria

1. **Live Inventory Integration**: The support bundle aggregates live fleet counts (total, online, stale, agent connected, UWF protected) from `ClientRecord` data.
2. **Explicit Human Consent Gate**: "Copy to Clipboard" and "Save to File" export actions are strictly disabled until the operator checks the explicit consent checkbox.
3. **Role Enforcement**: Only `technician` and `developer` roles can export support bundles; teachers are denied with clear UI guidance.
4. **Modal Preview & Canary Status**: Renders full markdown preview with clear privacy notices and displays the verified canary check status.
5. **Preserve All Assertions**: All existing assertions across all 68 test suites remain 100% intact.
6. **Dual Verification Gates**: Zero compiler warnings, 68 of 68 test suites pass cleanly under `./scripts/verify-gate.sh` and `./scripts/verify-gate.sh --strict`.

---

## 3. File Allowlist & Baseline SHA-256 Hashes

| Path | Baseline SHA-256 | Description |
|---|---|---|
| `server/app/main.cpp` | `5B6C7D434EA5A9135E37D180F1A09274E8FC5794D8589F30F611800025515525` | Support bundle trigger in fleet health view and modal preview with consent gate |
| `tests/server_state_tests.cpp` | `027BAA807B0DBA8D1B718B6825607438D3BA46FF75C9584433CE6200A99A152C` | Server state unit tests verifying support bundle consent and role matrix |

Strictly 2 files modified ($\le 5$ files limit respected).

---

## 4. Dependencies & Non-Negotiable Invariants

- **Author attribution**: Must commit exclusively as `khoasoma <songtu438@gmail.com>` with no AI assistant trailers.
- **Preserved dirty paths**: Never stage, modify, or revert `README.md`, `README.vi.md`, `docs/assets/banner.png`.
- **Private files**: Never stage or commit `PRODUCT_ROADMAP_NSTU.md`, `artifacts/`, `.claude/`, or `HANDOFF.md`.
- **Preserve all assertions**: Every existing assertion in all 68 test suites must be preserved.
- **Privacy Guarantee**: No leaking of credentials, student names, exam answers, or user paths.

---

## 5. Verification Commands

```bash
# Fast verification gate (zero warnings, all 68 test suites pass)
./scripts/verify-gate.sh

# Strict verification gate (Release mode, -Werror, zero warnings)
./scripts/verify-gate.sh --strict
```

