# Task Plan: WEBSITE-QUICK-LAUNCH-UI-1

## 1. Objective & Scope
This task implements the user-facing and execution layers of **Website and Quick Launch** per `REVISED_ROADMAP.md` (Priority 2 / P2.3: "Website and Quick Launch: validated URLs/launch definitions; never arbitrary shell execution in Teacher mode"):
1. **Client Service Named Pipe Relay**:
   - Handle incoming `CommandType::launch_url = 60` from the server control plane in `client/service/main.cpp`.
   - Validate and decode payload using `nstu::control::decode_launch_url`.
   - Defense-in-depth: If exam lockdown is armed (`g_exam_lockdown_controller.is_armed()`), drop the command immediately.
   - Forward validated payload via `AgentMessageType::launch_url = 40` across named pipe to the interactive student agent session.
2. **Student Agent Execution & Exam Lockdown Containment**:
   - Define `AgentMessageType::launch_url = 40` in `client/include/nstu/agent_protocol.hpp`.
   - Update `valid_type(AgentMessageType type)` in `client/src/agent_protocol.cpp` to allow `type <= AgentMessageType::launch_url`.
   - In `client/agent/main.cpp`, decode the message payload.
   - **Exam Security Invariant**: If `exam_host_engaged()`, suppress/drop the URL launch command immediately and enforce foreground kiosk containment to prevent test breakout!
   - In standard teaching mode, validate the URL with `nstu::url::is_valid_launch_url`.
   - Execute safe non-shell URL launch via `ShellExecuteW(nullptr, L"open", wide_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL)` in the interactive desktop session.
   - Log the opened website in the student chat history.
3. **Teacher Dear ImGui Console Surfaces**:
   - In `server/app/main.cpp`:
     - Add `std::array<char, 2048> launch_url_input{}` to `DashboardState`.
     - In `draw_workspace_toolbar`, add a "Launch Web" bulk action button when students are checked, activating `BulkActionKind::launch_url` and opening `state.bulk_confirmation_open`.
     - In `draw_bulk_action_confirmation_popup`, when `state.pending_bulk_action == BulkActionKind::launch_url`:
       - Show interactive URL text input with hint `https://...`.
       - Perform real-time validation via `nstu::url::is_valid_launch_url`; display validation warning in red if invalid and disable execution button.
       - Provide quick-launch preset buttons (e.g. `LMS`, `Docs`) for quick populating.
       - Pass `state.launch_url_input.data()` as `extra_payload` to `execute_bulk_action`.
     - In `draw_selected_client` (selected client view), add single-client "Launch Web" action.
4. **Automated Verification**:
   - Add unit test coverage for `AgentMessageType::launch_url = 40` in `tests/client_ipc_tests.cpp`:
     - Test `encode_agent_message` and `decode_agent_message` round-trip.
     - Test `AgentServiceQueue` push and priority drain.
   - Pass `./scripts/verify-gate.sh` (fast gate) and `./scripts/verify-gate.sh --strict` (clean Release build + `-Werror`).

---

## 2. File Allowlist & Source Baselines (SHA-256)
| File | Baseline SHA-256 |
|---|---|
| `client/include/nstu/agent_protocol.hpp` | `5798f145322422e795713727d34871a33e7358fc0260523814a8824f89d62c1c` |
| `client/src/agent_protocol.cpp` | `dbe43c91af6fcc76a2299155b441998fb597d3ef92e50e4909c99e1b281589cd` |
| `client/service/main.cpp` | `d223bb855768a7262c0d3349516a70b761671979ed35b06f15a727ce36d63e6e` |
| `client/agent/main.cpp` | `79c37791522389620aec213723ec9a9be3c4eb2b947af145597374ffb4d8b4e1` |
| `server/app/main.cpp` | `00425e866e662e5a6dc7411e54c8fb10562a6915a0e615385a6c8af85a3682fa` |
| `tests/client_ipc_tests.cpp` | `0911f2b92b99808806fd139659d409e3087d52798af0c8512e054f5e12bc6ace` |
| `tasks/task-plans/website-quick-launch-ui-1.md` | Tracked |

---

## 3. Acceptance Criteria
1. `AgentMessageType::launch_url = 40` defined in `client/include/nstu/agent_protocol.hpp`.
2. `valid_type(AgentMessageType type)` in `client/src/agent_protocol.cpp` updated to accept `launch_url` (up to `AgentMessageType::launch_url = 40`).
3. `client/service/main.cpp` handles `CommandType::launch_url`, decodes with `decode_launch_url`, drops if lockdown is armed, and forwards `AgentMessageType::launch_url` to the student agent.
4. `client/agent/main.cpp` handles `AgentMessageType::launch_url`:
   - Rejects/suppresses execution and enforces foreground if `exam_host_engaged() == true`.
   - Validates URL safety with `nstu::url::is_valid_launch_url` before execution.
   - Invokes `ShellExecuteW(nullptr, L"open", wide_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL)` in the interactive session.
   - Logs launch event into chat history.
5. `server/app/main.cpp` integrates:
   - "Launch Web" bulk button in `draw_workspace_toolbar`.
   - Interactive URL entry and real-time validation feedback in `draw_bulk_action_confirmation_popup`.
   - Single-client "Launch Web" button in `draw_selected_client`.
   - Full bilingual support (EN/VI) with `tr(state, english, vietnamese)`.
6. No shell interpretation (`cmd.exe`, `powershell.exe`) is invoked; only direct registered browser handling via `ShellExecuteW` with `open` verb on strictly validated `http://`/`https://` URLs.
7. Zero warnings under strict C++20 `-Wall -Wextra -Werror` / MSVC `/W4 /WX`.
8. All gate checks and tests pass 100%.

---

## 4. Test & Verification Plan
- Unit tests: `tests/client_ipc_tests.cpp` validating `AgentMessageType::launch_url` encoding, decoding, and queuing.
- Build targets: `cmake --build build/mingw --target nstu_client_ipc_tests nstu-server nstu-service nstu-agent`.
- Run fast gate: `./scripts/verify-gate.sh`.
- Run strict gate: `./scripts/verify-gate.sh --strict`.
- Independent non-author code review subagent invocation.
