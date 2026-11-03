# Task Plan: FLEET-HEALTH-STALE-INDICATOR-1

**Task ID**: `FLEET-HEALTH-STALE-INDICATOR-1`  
**Phase**: Priority 3 / P3.1 Fleet Health and Diagnostics (Stale-Data Detection & Freshness Telemetry)  
**Roadmap Reference**: `REVISED_ROADMAP.md` (P3.1: "Show device/version/service/agent/network/capture/UWF state with last-seen age and stale-data indicator. Avoid high-frequency polling and unnecessary CPU/RAM/application/site metadata.") and `tasks/product-execution-plan.md` (`FLEET-01/02`: "Stale-aware existing health/details first; add only justified low-frequency/change-driven version/service metadata under a privacy contract.").

---

## 1. Description & Architectural Rationale

This task implements stale-data awareness, last-seen age tracking, and activity telemetry for the Technician Fleet Health dashboard (`FLEET-01/02`):
1. **Freshness & Staleness Logic (`server/include/nstu/classroom_roster.hpp`, `server/src/classroom_roster.cpp`)**:
   - `is_client_stale(const ClientRecord& client, std::chrono::steady_clock::time_point now, std::chrono::seconds threshold = 30s) noexcept`:
     - Detects when an enrolled machine has stopped reporting telemetry within the threshold window.
     - Offline clients are not flagged as stale (they are already classified as offline).
     - Protects against clock skew / negative deltas.
   - `client_age_seconds(const ClientRecord& client, std::chrono::steady_clock::time_point now) noexcept`:
     - Computes monotonic elapsed duration since `client.last_seen`.
   - `format_relative_age(std::chrono::seconds age, bool is_vietnamese = false)`:
     - Formats elapsed age human-readably: "Just now" (<5s), "Xs ago", "Xm ago", "Xh ago", or "Never" ("Chưa nhận" in Vietnamese).
2. **Technician Fleet Health Dashboard Integration (`server/app/main.cpp`)**:
   - In `draw_technician_fleet_health`:
     - Add **Freshness / Last Seen** column:
       - Displays formatted relative age (`format_relative_age`).
       - If `is_client_stale(client, now)` is true, renders an amber/orange `Stale` badge with a detailed hover tooltip explaining that telemetry data may be outdated.
     - Add **Activity / Capture** column:
       - Renders live student activity state:
         - `Streaming (~15fps)` for active Focus stream.
         - `Capturing (X s)` when snapshotting is active.
         - `Viewing Screen` when student is viewing teacher broadcast.
         - `Idle` when connected and in standby.
     - Expand network column tooltips:
       - Displays delivery mode (`multicast` vs `unicast`), MAC address, and WoL support status.
3. **Unit Test Suite (`tests/classroom_roster_tests.cpp`)**:
   - Comprehensive tests for `is_client_stale`, `client_age_seconds`, and `format_relative_age` across fresh, stale, offline, uninitialized, and boundary timestamps.
   - Verified bilingual localization (EN/VI).

---

## 2. Acceptance Criteria

1. **Stale Data Detection**: An online client with `now - last_seen >= 30s` is flagged as stale; offline clients and freshly reporting clients are not.
2. **Relative Age Formatting**: Monotonic time deltas formatted accurately across seconds, minutes, hours, and uninitialized timestamps in English and Vietnamese.
3. **Fleet Health Table Expansion**: Table includes data freshness indicator and live activity/capture status without disrupting existing UWF or latency metrics.
4. **Privacy Invariant**: Strictly uses existing connection/heartbeat timestamps; no extra polling or intrusive client inspection.
5. **Dual Verification Gates**: Zero compiler warnings, 67 of 67 test suites pass cleanly under `./scripts/verify-gate.sh` and `./scripts/verify-gate.sh --strict`.

---

## 3. File Allowlist & Baseline SHA-256 Hashes

| Path | Baseline SHA-256 | Description |
|---|---|---|
| `server/include/nstu/classroom_roster.hpp` | `931D262D910D1CADC3A109F9BD3A1344F3D47A153EC40029B2E37E1A685DC0C5` | Declare freshness and age calculation utilities |
| `server/src/classroom_roster.cpp` | `A82180672BB5EF3B8968AD4240C3F147405D0938638859FF2BD37B847472C421` | Implement stale check, age duration, and formatting |
| `server/app/main.cpp` | `D6ADBC20039AAE5BFFCD99E166A95CED3080725269A8A44ED88C506377E184C2` | Render freshness, stale badge, and activity column in fleet table |
| `tests/classroom_roster_tests.cpp` | `55E42BCAADF1BA169C19E37BDB5F2E5C0C0B02CA6007D20169B4B05749BE9C2A` | Unit tests for stale check and age formatting logic |

Strictly 4 files modified ($\le 5$ files limit respected).

---

## 4. Dependencies & Non-Negotiable Invariants

- **Author attribution**: Must commit exclusively as `khoasoma <songtu438@gmail.com>` with no AI assistant trailers.
- **Preserved dirty paths**: Never stage, modify, or revert `README.md`, `README.vi.md`, `docs/assets/banner.png`.
- **Private files**: Never stage or commit `PRODUCT_ROADMAP_NSTU.md`, `artifacts/`, `.claude/`, or `HANDOFF.md`.
- **Preserve all assertions**: Every existing assertion in all 67 test suites must be preserved.
- **Low-overhead Fleet Monitoring**: No high-frequency polling or CPU/RAM/process metadata collection introduced.

---

## 5. Verification Commands

```bash
# Fast verification gate (zero warnings, all 67 test suites pass)
./scripts/verify-gate.sh

# Strict verification gate (Release mode, -Werror, zero warnings)
./scripts/verify-gate.sh --strict
```

