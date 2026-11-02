# Task Plan: UWF Overlay Reporting & Servicing Integration

- **Task Identifier**: `UWF-OVERLAY-SERVICING-INTEGRATION-1`
- **Stream**: Stream S5 (Reboot-to-Restore Setup & Fleet Diagnostics)
- **Component**: `common`, `setup`, `tests`
- **Allowlisted Files**:
  - `common/include/nstu/uwf_overlay_report.hpp`
  - `common/src/uwf_overlay_report.cpp`
  - `common/CMakeLists.txt`
  - `setup/include/nstu/setup/uwf.hpp`
  - `setup/src/uwf.cpp`
  - `tests/uwf_overlay_report_tests.cpp`
  - `tests/uwf_wmi_tests.cpp`
  - `tests/CMakeLists.txt`
  - `tasks/task-plans/uwf-overlay-servicing-integration-1.md`

---

## 1. Objectives & Scope

Integrate verified Stream S5 capabilities into `main`:
1. **Typed UWF WMI Interface (`UwfWmi`) & Production Adapter**:
   - Pure abstract interface for testing without hardware mutation.
   - Production COM/WMI implementation querying `root\standardcimv2\embedded` with proper VT_I4 / signed int32 bounds checks on uint32 properties.
2. **Overlay Configuration & Safe Sizing**:
   - `uwf_maximum_size_mb`: calculates safe RAM/disk overlay capacity clamped between minimum floor and system limits.
   - `configure_uwf_overlay`: updates next-boot overlay type and maximum size, with atomic rollback if intermediate steps fail.
   - Sets warning and critical thresholds in strictly ascending order (`warning < critical`).
3. **Overlay Consumption Probe & Binary Codec**:
   - `probe_uwf_overlay`: non-mutating consumption and threshold inquiry.
   - `nstu::uwf::OverlayReport`: 23-byte binary codec with strict bounds, signed WMI limit checks, and pending-reboot threshold validation.
4. **Servicing Mode, Decommission & Defender Exclusions**:
   - `set_uwf_servicing`: toggle UWF servicing mode for scheduled Windows Updates.
   - `disable_uwf`: schedule filter deactivation for decommissioning without unprotecting volumes or removing exclusions.
   - `add_uwf_defender_exclusions`: persist Microsoft-documented Defender signature update paths with full rollback on partial failure.
5. **Deterministic Seam Tests**:
   - Fake WMI mock covering all state combinations, step-level failure rollbacks, threshold order violations, and edge cases.
   - Zero real hardware mutation during tests.

---

## 2. Verification Protocol

- Fast verification gate: `scripts/verify-gate.sh` (0 warnings, 100% test pass).
- Strict verification gate: `scripts/verify-gate.sh --strict` (MSVC / Release + `-Werror`, 0 warnings, 100% test pass).
- Push to `origin feature/uwf-overlay-servicing-integration`.
- 4-job Windows CI on GitHub Actions.
- Merge to `main` via fast-forward and push.
- 5-job release workflow on `main`.

