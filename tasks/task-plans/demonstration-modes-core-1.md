# Task Plan: DEMONSTRATION-MODES-CORE-1

**Task ID**: `DEMONSTRATION-MODES-CORE-1`  
**Phase**: Priority 2 / P2.3 Daily Teaching Workflows & Demonstration Modes (Protocol & Codecs)  
**Roadmap Reference**: `REVISED_ROADMAP.md` (P2.3: "Demonstration modes: Focus and Follow-Along are distinct; snapshot wall remains low-bandwidth. Validate stream resource release and disconnect cleanup.") and `tasks/product-execution-plan.md` (`TOOL-08/09`: "Reuse Focus broadcast with resource/input-lock cleanup, then separate resizable Follow-Along. Keep the class wall snapshot-first.").

---

## 1. Description & Architectural Rationale

This task establishes the core wire protocol and binary codecs for teacher screen demonstration modes in NSTU:
1. **Protocol Command Definition (`common/include/nstu/protocol.hpp`)**:
   - `CommandType::host_broadcast_mode = 68`: Server-to-client control command carrying the active demonstration mode configuration and optional student input lock.
2. **Type Definitions & Codecs (`common/include/nstu/control_messages.hpp` & `common/src/control_messages.cpp`)**:
   - `DemonstrationMode` enum class:
     - `focus = 0`: Fullscreen presentation (topmost), intended for focused lectures where students view the teacher display.
     - `follow_along = 1`: Resizable floating window, intended for lab exercises where students follow along while interacting with their own desktop/tools.
   - `BroadcastModeMessage` struct:
     - `DemonstrationMode mode = DemonstrationMode::focus`: Configured presentation mode.
     - `bool lock_input = false`: Flag indicating whether student input should be locked during demonstration.
   - Binary serialization and deserialization functions:
     - `encode_broadcast_mode(const BroadcastModeMessage& msg) -> std::vector<std::byte>`
     - `decode_broadcast_mode(std::span<const std::byte> payload) -> std::optional<BroadcastModeMessage>`
   - Binary wire format:
     - 2 bytes Little-Endian version (`kBroadcastModeVersion = 1`).
     - 1 byte `mode` (must be `0` or `1`; any other value rejected).
     - 1 byte `lock_input` (must be `0` or `1`; any other value rejected).
     - Strict validation: exact length of 4 bytes, rejecting truncated buffers, trailing bytes, or unsupported versions.
3. **Comprehensive Unit Testing (`tests/control_channel_tests.cpp`)**:
   - Round-trip encoding and decoding tests for Focus mode (without lock and with lock).
   - Round-trip encoding and decoding tests for Follow-Along mode (without lock and with lock).
   - Rejection of unsupported version values.
   - Rejection of invalid/undefined mode values (e.g. `2`, `0xFF`).
   - Rejection of invalid bool values for `lock_input` (e.g. `2`, `0xFF`).
   - Rejection of truncated buffers (< 4 bytes) and oversized payloads with trailing garbage (> 4 bytes).

---

## 2. Acceptance Criteria

1. **Protocol Command**:
   - `CommandType::host_broadcast_mode` is declared with numeric value `68` in `common/include/nstu/protocol.hpp`.
2. **Type & Codec Implementation**:
   - `DemonstrationMode` enum class declared in `common/include/nstu/control_messages.hpp` with `focus = 0` and `follow_along = 1`.
   - `BroadcastModeMessage` struct declared with `mode` and `lock_input`.
   - `encode_broadcast_mode` and `decode_broadcast_mode` implemented in `common/src/control_messages.cpp`.
   - Codec enforces strict validation: fails closed on invalid versions, out-of-range mode enums, non-boolean values, truncation, and trailing garbage.
3. **Unit Tests**:
   - All tests in `tests/control_channel_tests.cpp` pass, covering valid permutations, bounds checks, corrupted inputs, and strict length rejection.
4. **Verification Gates**:
   - Fast gate (`./scripts/verify-gate.sh`) and strict gate (`./scripts/verify-gate.sh --strict`) pass with zero compiler warnings across all 67 test suites.

---

## 3. File Allowlist & Baseline SHA-256 Hashes

| Path | Baseline SHA-256 | Description |
|---|---|---|
| `common/include/nstu/protocol.hpp` | `AC061A0A9324686393B7C6417B0DA323DD6F9ED0E311E02F0DB9BABED1C2049F` | Declare `CommandType::host_broadcast_mode = 68` |
| `common/include/nstu/control_messages.hpp` | `73234204BC3E21D4BE914682B60D49DD53F645FDB9DF05304D4D2B447808ED31` | Declare `DemonstrationMode`, `BroadcastModeMessage`, and codecs |
| `common/src/control_messages.cpp` | `AB229F5AADC75CDAC00BE13BEDBC4BEBCFCAF0551E1552E786E3B2973DC3ACAF` | Implement `encode_broadcast_mode` and `decode_broadcast_mode` |
| `tests/control_channel_tests.cpp` | `A9E5E8D99EC59BF8B02A289B612BDA90E3E122F32E12BEFB8FCEFA939299A034` | Add comprehensive codec validation and failure tests |

---

## 4. Dependencies & Non-Negotiable Invariants

- **Author attribution**: Must commit exclusively as `khoasoma <songtu438@gmail.com>` with no AI assistant trailers.
- **Preserved dirty paths**: Never stage, modify, or revert `README.md`, `README.vi.md`, `docs/assets/banner.png`.
- **Private files**: Never stage or commit `PRODUCT_ROADMAP_NSTU.md`, `artifacts/`, `.claude/`, or `HANDOFF.md`.
- **Preserve all assertions**: Every existing assertion in all 67 test suites must be preserved.
- **Fail-Closed Codec Rule**: Any unrecognized payload version or corrupted field must return `std::nullopt`.
- **Low-Bandwidth Monitoring Invariant**: Broadcast modes must not interfere with or degrade low-bandwidth snapshot wall monitoring.

---

## 5. Verification Commands

```bash
# Fast verification gate (zero warnings, all 67 test suites pass)
./scripts/verify-gate.sh

# Strict verification gate (Release mode, -Werror, zero warnings)
./scripts/verify-gate.sh --strict
```

