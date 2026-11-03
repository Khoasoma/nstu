# Task Plan: DEMONSTRATION-MODES-SERVER-1

**Task ID**: `DEMONSTRATION-MODES-SERVER-1`  
**Phase**: Priority 2 / P2.3 Daily Teaching Workflows & Demonstration Modes (Server Control Plane)  
**Roadmap Reference**: `REVISED_ROADMAP.md` (P2.3: "Demonstration modes: Focus and Follow-Along are distinct; snapshot wall remains low-bandwidth. Validate stream resource release and disconnect cleanup.") and `tasks/product-execution-plan.md` (`TOOL-08/09`: "Reuse Focus broadcast with resource/input-lock cleanup, then separate resizable Follow-Along. Keep the class wall snapshot-first.").

---

## 1. Description & Architectural Rationale

Following the protocol and wire codec establishment in `DEMONSTRATION-MODES-CORE-1`, this task implements server control plane state management and broadcast dispatch for demonstration modes in `ServerControlPlane`:
1. **Demonstration Mode State Management (`server/include/nstu/control_plane.hpp`)**:
   - `[[nodiscard]] bool set_broadcast_mode(control::DemonstrationMode mode, bool lock_input, std::string* error = nullptr);`
   - `[[nodiscard]] control::BroadcastModeMessage broadcast_mode() const;`
2. **Server Control Plane Implementation (`server/src/control_plane.cpp`)**:
   - Internal state tracking for `broadcast_mode_` in `ServerControlPlane::Impl` protected by mutex.
   - Default initialization: `DemonstrationMode::focus` with `lock_input = false`.
   - `set_broadcast_mode`:
     - Validates input mode bounds; returns `false` with error `"invalid demonstration mode"` if invalid.
     - Encodes payload via `control::encode_broadcast_mode`.
     - Updates internal state.
     - Broadcasts `protocol::CommandType::host_broadcast_mode` to all active connected clients.
   - `stop_host_broadcast`:
     - Dispatches `protocol::CommandType::host_broadcast_stop`.
     - Automatically resets `lock_input = false` in `broadcast_mode_` to guarantee resource and input-lock cleanup.
3. **Control Plane Unit Tests (`tests/control_plane_tests.cpp`)**:
   - Verify initial broadcast mode is `focus`, `lock_input = false`.
   - Verify `set_broadcast_mode(DemonstrationMode::follow_along, true)` updates state and dispatches `host_broadcast_mode` payload decodable by client.
   - Verify invalid demonstration mode is rejected.
   - Verify `stop_host_broadcast` resets `lock_input = false` and sends `host_broadcast_stop`.

---

## 2. Acceptance Criteria

1. **API Interface**: `ServerControlPlane` exposes `set_broadcast_mode(mode, lock_input, error)` and `broadcast_mode()`.
2. **Fail-Closed & Bounds Validation**: Unrecognized demonstration mode enum values are rejected with error and not broadcast.
3. **Resource & Input Lock Cleanup**: Calling `stop_host_broadcast` releases student input lock expectation (`lock_input = false`) and broadcasts `host_broadcast_stop`.
4. **Unit Test Verification**: `tests/control_plane_tests.cpp` passes and exercises `set_broadcast_mode`, payload decoding, error handling, and `stop_host_broadcast` state cleanup.
5. **No Regression**: All 67 existing test suites pass with zero warnings in fast and strict gates.

---

## 3. File Allowlist & Baseline SHA-256 Hashes

| Path | Baseline SHA-256 | Description |
|---|---|---|
| `server/include/nstu/control_plane.hpp` | `837B5C6AED56B649F3DDAD00A48E3315F064DA39AA7C84B1FFC98CF3730E2EC8` | Declare demonstration mode control plane methods |
| `server/src/control_plane.cpp` | `42A1ADDBDACE30A54C793466596DFA7B19051247FB3DB1B17915811F6896C735` | Implement demonstration mode broadcast, state tracking & cleanup |
| `tests/control_plane_tests.cpp` | `C6703DCF013452C994793A47AE9B57C4B6466B5611011100F831ADC9853F0EF6` | Add control plane demonstration mode unit tests |

---

## 4. Dependencies & Non-Negotiable Invariants

- **Author attribution**: Must commit exclusively as `khoasoma <songtu438@gmail.com>` with no AI assistant trailers.
- **Preserved dirty paths**: Never stage, modify, or revert `README.md`, `README.vi.md`, `docs/assets/banner.png`.
- **Private files**: Never stage or commit `PRODUCT_ROADMAP_NSTU.md`, `artifacts/`, `.claude/`, or `HANDOFF.md`.
- **Preserve all assertions**: Every existing assertion in all 67 test suites must be preserved.
- **Low-Bandwidth Invariant**: Demonstration mode control messages must not increase background telemetry or snapshot overhead.

---

## 5. Verification Commands

```bash
# Fast verification gate (zero warnings, all 67 test suites pass)
./scripts/verify-gate.sh

# Strict verification gate (Release mode, -Werror, zero warnings)
./scripts/verify-gate.sh --strict
```
