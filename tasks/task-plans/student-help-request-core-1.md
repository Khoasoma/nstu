# Task Plan: Student Help Request Protocol & Server Core

- **Task Identifier**: `STUDENT-HELP-REQUEST-CORE-1`
- **Stream**: Roadmap Priority 2 / P2.3 (Complete daily teaching workflows - Help Request)
- **Component**: `common`, `server`, `tests`
- **Allowlisted Files**:
  - `common/include/nstu/protocol.hpp`
  - `common/include/nstu/control_messages.hpp`
  - `common/src/control_messages.cpp`
  - `server/include/nstu/client_registry.hpp`
  - `server/src/client_registry.cpp`
  - `server/include/nstu/control_plane.hpp`
  - `server/src/control_plane.cpp`
  - `server/include/nstu/classroom_roster.hpp`
  - `server/src/classroom_roster.cpp`
  - `tests/classroom_roster_tests.cpp`
  - `tasks/task-plans/student-help-request-core-1.md`

---

## 1. Context & Motivation

Per `REVISED_ROADMAP.md` (P2.3):
> *"Help Request: device/session-scoped request, teacher acknowledgement, cancel and reconnect semantics."*

In classroom teaching, students need an immediate, low-friction mechanism to raise their hand or request teacher assistance without disrupting the whole room. 
This task establishes the robust protocol codecs, client registry tracking, control plane dispatch, and classroom roster filtering foundations:
1. Typed binary codecs for `HelpRequestMessage` (raising/canceling hand with optional bounded text) and `HelpAckMessage` (teacher acknowledgment).
2. Wire command integration: `CommandType::help_request` (58) and `CommandType::help_ack` (59).
3. Thread-safe client registry state tracking: `help_requested`, `help_message`, `help_requested_at_unix_ms`.
4. Control plane dispatch: receiving client help requests, emitting notifications, and providing `resolve_help_request(client_id, &error)`.
5. Roster filtering: `RoomFilter::help`, `RoomCounts::help`, and `BulkActionKind::acknowledge_help`.

---

## 2. Technical Design

### A. Wire Protocol & Codecs
1. In `common/include/nstu/protocol.hpp`:
   ```cpp
   help_request = 58,
   help_ack = 59,
   ```
2. In `common/include/nstu/control_messages.hpp` & `common/src/control_messages.cpp`:
   - `inline constexpr std::size_t kMaximumHelpMessageBytes = 256;`
   - `struct HelpRequestMessage`:
     - `bool active = true;` (false indicates student lowered hand/cancelled)
     - `std::uint64_t timestamp_unix_ms = 0;`
     - `std::string message;` (optional brief student note, truncated/bounded to 256 bytes)
   - `struct HelpAckMessage`:
     - `bool resolved = true;`
     - `std::uint64_t timestamp_unix_ms = 0;`
   - Binary encoding format:
     - `HelpRequestMessage`: `[1 byte active][8 bytes timestamp_unix_ms LE][2 bytes message_length LE][message bytes]`
     - `HelpAckMessage`: `[1 byte resolved][8 bytes timestamp_unix_ms LE]`
   - Pure encoders and decoders:
     - `[[nodiscard]] std::vector<std::byte> encode_help_request(const HelpRequestMessage& msg);`
     - `[[nodiscard]] std::optional<HelpRequestMessage> decode_help_request(std::span<const std::byte> payload);`
     - `[[nodiscard]] std::vector<std::byte> encode_help_ack(const HelpAckMessage& msg);`
     - `[[nodiscard]] std::optional<HelpAckMessage> decode_help_ack(std::span<const std::byte> payload);`

### B. Client Registry Tracking
In `server/include/nstu/client_registry.hpp` and `server/src/client_registry.cpp`:
- Add fields to `ClientRecord`:
  - `bool help_requested = false;`
  - `std::string help_message;`
  - `std::uint64_t help_requested_at_unix_ms = 0;`
- Add method:
  - `[[nodiscard]] bool set_help_request(std::uint64_t id, bool active, std::string message, std::uint64_t timestamp);`

### C. Server Control Plane Dispatch
In `server/include/nstu/control_plane.hpp` and `server/src/control_plane.cpp`:
- Handle incoming `CommandType::help_request` in network message processing loop, updating `ClientRegistry`.
- Expose method:
  - `bool resolve_help_request(std::uint64_t client_id, std::string* error = nullptr);`
    Sends `CommandType::help_ack` to client and clears `help_requested` in `ClientRegistry`.

### D. Classroom Roster Filtering & Bulk Actions
In `server/include/nstu/classroom_roster.hpp` and `server/src/classroom_roster.cpp`:
- `enum class RoomFilter : int { all = 0, online = 1, attention = 2, locked = 3, offline = 4, help = 5 };`
- `RoomCounts`: add `std::size_t help = 0;`
- `BulkActionKind`: add `acknowledge_help = 3;`
- Filter logic: `RoomFilter::help` matches clients where `help_requested == true`.
- Bulk execution: `execute_bulk_action` handles `BulkActionKind::acknowledge_help`, calling `resolve_help_request` on all selected clients with active help requests.

---

## 3. Baseline Source Hashes

- `common/include/nstu/protocol.hpp`: `AA252086807C7D89692968907DD734706726167F157568AD17146EDC03FE5296`
- `common/include/nstu/control_messages.hpp`: `52375259D71367AFBED2D220BEFD1A79DC2BFBA022B8A02B8DB6790C525449CF`
- `common/src/control_messages.cpp`: `4CC327D457CE394483686313516000F8F348A58D5C6B81DE1DF07613661520AA`
- `server/include/nstu/client_registry.hpp`: `A57C5AF8E9B8C1BF48F51ABF986A816CD5CEB57ACB5CDC10DA689DE60F8CD766`
- `server/src/client_registry.cpp`: `2921A9B850AD3345A42D3619F59B59EED79C08AAB66E198FF0761D3AAA4DDDDF`
- `server/include/nstu/control_plane.hpp`: `4BD188E4E34B429B922898E367B6A0CFD6000B5495822131042B6C97A6876105`
- `server/src/control_plane.cpp`: `63F42591BD7F8365DDCD9C599C8F533AC747CCC830F38C0638CBA03F8A186951`
- `server/include/nstu/classroom_roster.hpp`: `6685A10763075EED83A2D6AC22715F4853873398F2BF4A07EEE9ECD042259821`
- `server/src/classroom_roster.cpp`: `25E50084AC6545A169EE7DCF4D1006D0F4AF02F929E0F115AC6291F74806DBA9`
- `tests/classroom_roster_tests.cpp`: `39C050B81B65695611739AC82D22DC9146AD6FA9A26C473835CBAC2B07F60321`

---

## 4. Acceptance Criteria

1. [ ] Protocol commands `help_request = 58` and `help_ack = 59` defined in `common/include/nstu/protocol.hpp`.
2. [ ] Binary codecs `encode_help_request`, `decode_help_request`, `encode_help_ack`, `decode_help_ack` round-trip accurately with bounds enforcement.
3. [ ] `ClientRegistry` tracks `help_requested`, `help_message`, and timestamp, persisting across touch/status transitions.
4. [ ] `ServerControlPlane` handles inbound client help requests and provides `resolve_help_request`.
5. [ ] `classroom_roster` supports `RoomFilter::help`, `RoomCounts::help`, and `BulkActionKind::acknowledge_help`.
6. [ ] Unit tests in `tests/classroom_roster_tests.cpp` verify help filtering, count aggregations, and bulk acknowledgment across cohorts.
7. [ ] Local `./scripts/verify-gate.sh` and `./scripts/verify-gate.sh --strict` pass with 0 warnings.
8. [ ] Windows CI passes all jobs on feature branch and upon fast-forward merge to `main`.

