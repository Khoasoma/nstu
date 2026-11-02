# Task Plan: STUDENT-HELP-REQUEST-UI-1

## 1. Context & Objectives
- **Task ID**: `STUDENT-HELP-REQUEST-UI-1`
- **Roadmap Ref**: `REVISED_ROADMAP.md` Priority 2 / P2.3: "Daily teaching workflows: Help Request: device/session-scoped request, teacher acknowledgement, cancel and reconnect semantics"
- **Branch**: `feature/student-help-request-ui`
- **Prerequisite**: `STUDENT-HELP-REQUEST-CORE-1` (Merged to main at `7477902` and documented at `de09135`)
- **Objective**: Complete end-to-end integration of the Student Help Request feature across client agent UI, client service relay, and teacher server GUI:
  1. Student agent UI provides intuitive "Raise Hand" / "Cancel Hand" toggle with live state in chat window and automatic reconnect re-transmission.
  2. Client service relays help request/ack payloads between agent named pipe and server control socket.
  3. Teacher GUI surfaces live Help filter chip, badges on grid cards and table rows, direct single-click resolution, and bulk acknowledgment.

## 2. Baseline Source Hashes (SHA-256)
- `client/include/nstu/agent_protocol.hpp`: `82D17193048C6896FEBC7D18A7AA635EA959DA684ED9882C497969B28D8E422F`
- `client/agent/main.cpp`: `F3F5941DE8F983E83100C553E0516966E149FC86A179811E4D73B9E867587878`
- `client/service/main.cpp`: `4EEA3A4A71504F40359CBBBD5AD4DF031438CAE053C89415C9C10943FD8038C6`
- `server/app/main.cpp`: `548DDBEE427626007A175D6A50F4304F37FD33F841A5E79BA695B80A0108CC44`
- `tests/client_ipc_tests.cpp`: `C303727C6B63B927961442FBEA101D9D20ADF62D2C6687C962B3DA1AC1989C3C`

## 3. Allowlisted File Paths
Only the following files are permitted to be modified or created:
1. `client/include/nstu/agent_protocol.hpp`
2. `client/agent/main.cpp`
3. `client/service/main.cpp`
4. `server/app/main.cpp`
5. `tests/client_ipc_tests.cpp`
6. `tasks/task-plans/student-help-request-ui-1.md`
7. `tasks/task-plans/student-help-request-ui-1-result.md`

## 4. Acceptance Criteria
1. **Agent Protocol IPC**:
   - `AgentMessageType::help_request = 38` and `AgentMessageType::help_ack = 39` defined in `client/include/nstu/agent_protocol.hpp`.
2. **Client Service Relay**:
   - In `client/service/main.cpp`: Handle `AgentMessageType::help_request` from agent; validate length and queue outbound `protocol::CommandType::help_request`.
   - In `client/service/main.cpp`: Handle `protocol::CommandType::help_ack` from server; queue inbound `AgentMessageType::help_ack` to agent.
3. **Student Agent UI**:
   - In `client/agent/main.cpp`: Add `kChatHelp` button in `g_chat_window` styled cleanly alongside `kChatSend`.
   - Button toggles between "Raise Hand" and "Lower Hand" (or "Cancel").
   - Transmits encoded `HelpRequestMessage` to service.
   - Upon receiving `AgentMessageType::help_ack`: sets state to lowered, updates button, logs confirmation in chat box.
   - Reconnect semantics: When `server_online` transitions to 1, if help request was active, re-transmits `help_request` so the teacher server regains the hand raise after network blips.
4. **Teacher Server GUI**:
   - In `server/app/main.cpp`:
     - Toolbar: "Help" filter chip with count `counts.help` alongside All/Online/Attention/Locked/Offline.
     - Grid Card: Amber `[?]` badge with tooltip showing help message when `client.help_requested`.
     - Table Row: `[?]` badge with tooltip in Hostname column; "Ack Help" action button in Action column.
     - Bulk Actions: "Acknowledge Help" bulk action button when clients are selected.
     - Selected Client View: Prominent Help alert banner with message and "Acknowledge Help" resolution button.
5. **Test Coverage**:
   - In `tests/client_ipc_tests.cpp`: Unit test verifying `AgentServiceQueue` queueing and handling of `AgentMessageType::help_request` and `AgentMessageType::help_ack` with `HelpRequestMessage` / `HelpAckMessage` payloads.
   - No assertions removed or weakened.
6. **Quality Gates**:
   - `./scripts/verify-gate.sh` passes with 0 warnings.
   - `./scripts/verify-gate.sh --strict` passes with 0 warnings.
   - Independent non-author subagent review verdict ACCEPT.

