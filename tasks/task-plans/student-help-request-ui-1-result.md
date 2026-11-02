# Task Result: STUDENT-HELP-REQUEST-UI-1

## 1. Task Metadata
- **Task ID**: `STUDENT-HELP-REQUEST-UI-1`
- **Roadmap Ref**: `REVISED_ROADMAP.md` Priority 2 / P2.3 (Daily Teaching Workflows / Help Request: device/session-scoped request, teacher acknowledgement, cancel and reconnect semantics)
- **Branch**: `feature/student-help-request-ui`
- **Target Integration**: `main` (INDEV)
- **Author**: `khoasoma <songtu438@gmail.com>`

## 2. Summary of Implementation
- **Agent Protocol IPC**: Declared `AgentMessageType::help_request = 38` (agent -> service) and `AgentMessageType::help_ack = 39` (service -> agent) in `client/include/nstu/agent_protocol.hpp`.
- **Client Service Relay**:
  - In `client/service/main.cpp`, inbound `AgentMessageType::help_request` validates bounds and forwards outbound `protocol::CommandType::help_request` to the server socket.
  - In `client/service/main.cpp`, inbound `protocol::CommandType::help_ack` validates payload with `decode_help_ack` and forwards inbound `AgentMessageType::help_ack` to the interactive agent pipe.
- **Client Agent UI**:
  - Added `kChatHelp = 1006` button in `g_chat_window` with dark styling and amber active tint (`RGB(180, 115, 25)`).
  - Implemented `toggle_help_request()` allowing students to raise hand (with optional brief message from chat input) or cancel an active hand-raise.
  - Handled teacher acknowledgment (`AgentMessageType::help_ack`) resetting button state and logging confirmation in the chat listbox.
  - Implemented reconnect semantics in `AgentMessageType::server_online` to re-transmit active help request with preserved notes when reconnected to the teacher server.
- **Teacher GUI (Server)**:
  - Added "Help" filter chip in `draw_workspace_toolbar` displaying live count of hands raised.
  - Displayed amber `[?]` badge with hover tooltip showing note in both grid card (`draw_client_card`) and table view (`draw_room_client_table`).
  - Added single-click "Ack Help" action button on table rows and "Acknowledge Help" bulk action button on selected cohorts.
  - Added prominent `#help-banner` in `draw_selected_client` with note preview and direct resolution button.
- **Unit Testing**: Added dedicated test suite in `tests/client_ipc_tests.cpp` exercising `AgentServiceQueue` queueing, FIFO ordering, and bidirectional serialization/deserialization of help request/ack payloads.

## 3. Source Hashes (SHA-256)
- `client/include/nstu/agent_protocol.hpp`: `5798F145322422E795713727D34871A33E7358FC0260523814A8824F89D62C1C`
- `client/agent/main.cpp`: `79C37791522389620AEC213723EC9A9BE3C4EB2B947AF145597374FFB4D8B4E1`
- `client/service/main.cpp`: `D223BB855768A7262C0D3349516A70B761671979ED35B06F15A727CE36D63E6E`
- `server/app/main.cpp`: `00425E866E662E5A6DC7411E54C8FB10562A6915A0E615385A6C8AF85A3682FA`
- `tests/client_ipc_tests.cpp`: `0911F2B92B99808806FD139659D409E3087D52798AF0C8512E054F5E12BC6ACE`

## 4. Verification Receipts
- **Fast Gate**: `./scripts/verify-gate.sh`
  - Zero banned filenames / target names / identifiers outside string literals
  - No forbidden paths staged
  - No weakened or removed assertions (63 test files verified)
  - Node syntax & exam web recovery tests: 63 passed
  - C++ build: Succeeded with 0 warnings
  - C++ unit tests: 63/63 passed (100%)
- **Strict Gate**: `./scripts/verify-gate.sh --strict`
  - Release build with `-DNSTU_ENABLE_WERROR=ON`
  - Zero compiler warnings
  - C++ unit tests: 63/63 passed (100%)
- **Independent Non-Author Code Review**:
  - Subagent: `fc0387ac-baec-4277-b0fc-631e11a03176`
  - Role: Independent Non-Author Code Reviewer
  - Report Verdict: **ACCEPT**

## 5. Next Steps
- Commit allowlisted files to `feature/student-help-request-ui`.
- Push feature branch to `origin/feature/student-help-request-ui` and monitor CI.
- Fast-forward merge into `main` as INDEV under human integration rules.
- Verify Windows CI passes all 5 jobs on `main`.
- Update `docs/INDEV_STATUS.md`.
