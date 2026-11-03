# Phase 4 & 5 INDEV Status

**Status: INDEV (in development).** The complete Phase 4 Discrete Timing Telemetry Pipeline and Native WebView2 Kiosk are merged into `main` at commit `79d1fdfb9f17f631d2e5fd5caa031fbe66e07d7c`. All 16 Phase 4 tasks (Tasks 0–16) and Checkpoints A, B, C, and D are verified.

The master plan for **Phase 5: Exam Security Delivery & Network Hardening** (`tasks/task-plans/phase5-exam-security-delivery-plan.md`) is approved and underway. This document records the merged baseline and the remaining security acceptance criteria before production deployment.

## Integrated Phase 4 Work (Tasks 0–16)

| Work Area | Scope & Implementation | Status |
|---|---|---|
| **Telemetry Protocol & Transport** | Versioned binary framing (`exam_telemetry_batch` / `exam_telemetry_ack`), monotonic timestamps, sequence numbers, batch validation, and cross-session hash validation | **Merged to main** (`79d1fdf`) |
| **Client Discrete Timing Collector** | `nstu_exam_telemetry_collector`: per-question dwell, first-view, time-to-first-answer, selection change counts, blur/focus transitions, scroll depth | **Merged to main** (`79d1fdf`) |
| **Zero-Raw-Input Privacy Guarantee** | Absolute exclusion of keystrokes, clipboard data, raw mouse trajectories, and response text from telemetry collectors, outbox, wire formats, and journal | **Merged to main** (`79d1fdf`) |
| **Client DPAPI Retry Outbox** | Machine-scoped DPAPI storage (`nstu_exam_telemetry_outbox`) with bounded FIFO queue, transactional commits, and transport error backoff | **Merged to main** (`79d1fdf`) |
| **Server Telemetry Journal & Dedup** | Crash-safe server sink (`nstu_exam_telemetry_journal`) with 8×8 MiB preallocated rotating segments, ≤262,144 deduplication cache, 16 generations, un-resurrected replay floor, and 30-day retention bound | **Merged to main** (`79d1fdf`) |
| **Asynchronous Persistence Worker** | Dedicated queue worker (`nstu_exam_telemetry_worker`) decoupled from IOCP network threads, returning immediate `unavailable` ACKs under saturation | **Merged to main** (`79d1fdf`) |
| **Cross-Channel Isolation** | Cryptographically isolated answer event stream (`exam_answer_event`) and telemetry stream; answer submission and hash-chain verification never stalled by telemetry flood | **Merged to main** (`79d1fdf`) |
| **Lifecycle Outcome Architecture** | Complete disconnect, close, stop, and shutdown reporting foundation with audited operator results and student-service binding across Tasks 1–13 | **Merged to main** (`79d1fdf`) |
| **Native WebView2 Kiosk** | Pinned Microsoft.Web.WebView2 SDK acquisition in CMake/CI; fullscreen kiosk window with low-level shortcut suppression (Win, Alt+Tab, Ctrl+Esc), foreground re-grab, and local virtual host (`https://nstu.exam/`) | **Merged to main** (`79d1fdf`) |
| **Gate & Browser Recovery Tests** | 63 Node.js browser recovery test cases (`tests/exam_web_recovery_tests.js`) and syntax checks integrated into `scripts/verify-gate.sh` | **Merged to main** (`79d1fdf`) |

## Verification Baseline

- **C++ Tests**: 65/65 unit and integration test executables pass cleanly under MSVC and MinGW/UCRT64.
- **Node.js Web Tests**: 63 browser state recovery and submission tests pass.
- **Local Verification Gate**: `./scripts/verify-gate.sh` (fast gate) and `./scripts/verify-gate.sh --strict` (Release with `-Werror`) pass with 0 warnings.
- **Continuous Integration**: 4/4 Windows jobs on feature branches and 5/5 jobs on `main` passing clean.

## Integrated Operator Surfaces & Role Separation (Roadmap P2.1 / Task 24)

| Work Area | Scope & Implementation | Status |
|---|---|---|
| **Domain Role & Permission Matrix** | Pure `OperatorRole` (Teacher, Technician, Developer) and `OperatorAction` matrix in `nstu::server::is_action_permitted`, preventing unauthorized execution of disruptive fleet actions | **Merged to main** (`b37c2ee`) |
| **Teacher Mode Clean Workspace** | Default role providing focused classroom operations; disruptive maintenance (UWF configuration, client reboot) and raw telemetry exports guarded with explanatory tooltips | **Merged to main** (`b37c2ee`) |
| **Dedicated Technician Fleet Surface** | Full `draw_technician_fleet_health` dashboard displaying fleet inventory metrics, reboot-to-restore active/pending status, average latency, and table with ping, loss, UWF badges, and UWF arming actions | **Merged to main** (`b37c2ee`) |
| **Developer Advanced Controls** | Retained raw DirectX/D3D11 graphics diagnostics modal and telemetry markdown review behind Developer mode | **Merged to main** (`b37c2ee`) |
| **Operator Surface Switcher & Fallback** | Top menu strip and Settings surface switcher with automatic fallback from `fleet_health` to `room_screens` when transitioning to Teacher role | **Merged to main** (`b37c2ee`) |
| **Permission Test Suite** | 33-cell permission matrix verification, bilingual localization tests (EN/VI), and string conversions in `tests/operator_role_tests.cpp` | **Merged to main** (`b37c2ee`) |

## Integrated Mutual Pairing & Student Confirmation (Stream S3 / Roadmap P0-03 & P1.2)

| Work Area | Scope & Implementation | Status |
|---|---|---|
| **Student SAS Confirmation Enforcement** | `StudentConfirmationChoice`, `StudentConfirmationDecision`, and pure `evaluate_student_confirmation` decision evaluator enforcing positive student SAS code confirmation before key commitment | **Merged to main** (`eb54145`) |
| **SAS Formatting Helper** | `format_short_authentication_string` in `nstu/pairing.hpp` formatting 6-digit SAS codes as `XXX YYY` for student visual inspection | **Merged to main** (`eb54145`) |
| **Fail-Closed Pairing & Sliced Heartbeats** | Fail-closed pairing flow if code observer or confirmation callback is omitted; sliced confirmation loop maintaining keepalive heartbeats to prevent TCP timeout | **Merged to main** (`eb54145`) |

## Integrated Installer Provisioning & Firewall Scoping (Roadmap P1.1 / Task 8)

| Work Area | Scope & Implementation | Status |
|---|---|---|
| **Server Firewall Provisioning** | Self-contained least-privilege Windows Defender Firewall rules for TCP 47001 (control plane) and UDP 47001 (discovery beacon) scoped strictly to `nstu-server.exe` on `private,domain` profiles | **Merged to main** (`420dacb`) |
| **Uninstall & Rollback Teardown** | Automatic deletion of server firewall rules during uninstall and install abort/rollback, preventing orphaned firewall entries | **Merged to main** (`420dacb`) |
| **Firewall Provisioning Validation** | Automated test `packaging/test-installer-firewall-provisioning.ps1` integrated into Windows CI validating rule specifications, absence of public profiles, and rollback cleanup | **Merged to main** (`420dacb`) |

## Integrated Classroom Roster & Safe Bulk Actions (Roadmap P2.2 / Order 10)

| Work Area | Scope & Implementation | Status |
|---|---|---|
| **Classroom Tabular View** | Dense 7-column tabular roster view (`draw_room_client_table`) with multi-select checkboxes, hostname & unread chat badges, IP, status badges, latency & loss, and UWF protection indicators | **Merged to main** (`24701ca`) |
| **Grid / List Workspace Toggle** | Seamless toggle between visual thumbnail wall and compact list view in `draw_workspace_toolbar` | **Merged to main** (`24701ca`) |
| **Safe Multi-Selection & Bulk Actions** | Multi-select state machine with "Select All" (filtered subset) and "Clear", plus modal confirmation dialog requiring explicit human authorization and stating exact target counts, room context, and offline exclusions | **Merged to main** (`24701ca`) |
| **Roster Scalability Benchmarks** | Headless unit tests in `tests/classroom_roster_tests.cpp` benchmarking 10, 30, and 50 client cohorts with sub-millisecond execution (<5 µs) | **Merged to main** (`24701ca`) |

## Integrated Daily Teaching Workflows: Student Help Requests (Roadmap P2.3 / Core & UI)

| Work Area | Scope & Implementation | Status |
|---|---|---|
| **Wire Commands & Binary Framing** | Versioned protocol commands `help_request = 58` and `help_ack = 59`, with `HelpRequestMessage` and `HelpAckMessage` codecs enforcing 256-byte message length bounds and host-endian safety | **Merged to main** (`7477902`) |
| **Registry Help State & Upsert Invariant** | Thread-safe `ClientRegistry::set_help_request` and status report upsert invariant copying existing help state forward to prevent periodic status reports from wiping active student hand-raises | **Merged to main** (`7477902`) |
| **Server Control Plane Resolution** | Inbound `help_request` dispatch in `ServerControlPlane` and teacher-driven `resolve_help_request` updating local registry and dispatching `help_ack` command | **Merged to main** (`7477902`) |
| **Classroom Filtering & Bulk Resolution** | Orthogonal `RoomFilter::help` filter and `RoomCounts::help` counter maintaining status partition invariants (`online + attention + locked + offline == total`), plus `BulkActionKind::acknowledge_help` with EN/VI localization formatters | **Merged to main** (`7477902`) |
| **Agent IPC Protocol & Relay** | `AgentMessageType::help_request = 38` and `help_ack = 39` with length-validated bidirectional forwarding between student agent named pipe and server socket | **Merged to main** (`2e5155d`) |
| **Student Hand-Raise Interaction** | High-DPI dark/amber styled "Raise Hand" / "Lower Hand" button in chat UI, notes input capture, visual notifications, and reconnect re-transmission on `server_online` | **Merged to main** (`2e5155d`) |
| **Teacher Console Visualization & Actions** | Toolbar "Help" filter chip with live `counts.help`, grid card amber badge `[?]`, table row badge with single-click "Ack Help" action, bulk acknowledgment, and `#help-banner` in detail view | **Merged to main** (`2e5155d`) |

## Integrated Daily Teaching Workflows: Website & Quick Launch (Roadmap P2.3 / Core & UI)

| Work Area | Scope & Implementation | Status |
|---|---|---|
| **URL Security & Validation** | Pure `is_valid_launch_url` and `sanitize_launch_url` enforcing strict `http://` / `https://` allowlist, 2048-byte limit, host presence, and rejection of shell metacharacters, control bytes, and unencoded spaces | **Merged to main** (`c14bfcb`) |
| **Wire Command & Codecs** | Protocol command `launch_url = 60` with length-prefixed binary codecs (`encode_launch_url` / `decode_launch_url`) in `control_messages` preventing buffer overruns or malformed payloads | **Merged to main** (`c14bfcb`) |
| **Server Control Plane Dispatch** | `ServerControlPlane::launch_url` validating client authentication and URL safety before routing command | **Merged to main** (`c14bfcb`) |
| **Classroom Roster Bulk Launch** | `BulkActionKind::launch_url = 4` with backward-compatible `extra_payload` parameter in `execute_bulk_action`, plus bilingual EN/VI confirmation and result status formatters | **Merged to main** (`c14bfcb`) |
| **Service Named-Pipe Relay & Defense-in-Depth** | Inbound `launch_url = 60` handling in `nstu-service`, dropping command if `g_exam_lockdown_controller.is_armed()`, and forwarding validated payload via `AgentMessageType::launch_url = 40` across named pipe | **Merged to main** (`3328d95`) |
| **Student Agent Execution & Kiosk Containment** | `nstu-agent` URL launching via `ShellExecuteW` on validated URLs in interactive desktop session, strictly dropping launch and enforcing foreground containment when `exam_host_engaged() == true`, and recording in chat history | **Merged to main** (`3328d95`) |
| **Teacher Console UI & Validation UX** | Dear ImGui "Launch Web" bulk toolbar button, single-client action in selected client view, and modal confirmation popup featuring real-time URL validation, LMS/Docs presets, and execution guards | **Merged to main** (`3328d95`) |

## Integrated Power Actions: Reboot & Shutdown (Roadmap P2.3 / Core & UI)

| Work Area | Scope & Implementation | Status |
|---|---|---|
| **Wire Protocol & 2-Byte Codecs** | Protocol command `power_action = 61` with fixed 2-byte binary codecs (`encode_power_action` / `decode_power_action`), validating action kind (`reboot = 1`, `shutdown = 2`) and flag bit 0 (`force`), rejecting reserved bits 1–7 | **Merged to main** (`9750b37`) |
| **Server-Side Authorization & Gating** | Added `OperatorAction::shutdown_client = 11` to role matrix; `ServerControlPlane::power_action` enforcing Teacher disarmed from disruptive operations, Technician & Developer authorization, and `force == true` requiring elevated role | **Merged to main** (`9750b37`) |
| **Active Exam Containment Guard** | `ServerControlPlane::power_action` strictly checking `exam_active(client_id)` to refuse reboots or shutdowns on clients with ongoing exams, with remote session audit logging | **Merged to main** (`9750b37`) |
| **Classroom Roster Bulk Actions** | `BulkActionKind::reboot = 5` and `BulkActionKind::shutdown = 6` in `execute_bulk_action`, skipping offline clients, with bilingual EN/VI confirmation warnings ("Any unsaved work may be lost." / "Các thay đổi chưa lưu có thể bị mất.") and execution status formatting | **Merged to main** (`9750b37`) |
| **Service Win32 Execution & Privilege Management** | `nstu-service` handling `power_action = 61`, dropping during armed exam lockdown, acquiring `SeShutdownPrivilege`, and calling `InitiateSystemShutdownExW` with graceful 5s / forced immediate parameters | **Merged to main** (`032daf5`) |
| **Teacher Console UI & Role-Gated Modal** | Dear ImGui "Reboot" and "Shutdown" bulk toolbar buttons, single-client actions with active-exam tooltips, and modal confirmation enforcing P2.1 role gating with warning banner for Teacher role | **Merged to main** (`032daf5`) |


## Integrated Fleet Reboot-to-Restore & Diagnostics (Stream S5)

| Work Area | Scope & Implementation | Status |
|---|---|---|
| **Typed UWF WMI Management** | `UwfWmi` interface and `ProductionUwfWmi` implementation; safe RAM/disk overlay sizing (`uwf_maximum_size_mb`), strictly ordered warning/critical thresholds, and transactional rollback | **Merged to main** (`471553e`) |
| **UWF Overlay Reporting Codec** | `nstu::uwf::OverlayReport` 23-byte binary codec with bounds validation, WMI signed VT_I4 limits, and pending-reboot threshold handling | **Merged to main** (`471553e`) |
| **Servicing & Decommissioning** | `set_uwf_servicing` for Windows Update maintenance, `disable_uwf` for controlled decommissioning, and `add_uwf_defender_exclusions` | **Merged to main** (`471553e`) |

## Phase 5 Security Delivery Roadmap (Tasks 18–23) — Complete

| Task | Title | Key Objective | Status |
|---|---|---|---|
| **Task 18** | `WFP-IPV6-DEFAULT-DENY-HARDENING-1` | Enforce dual-stack WFP ALE IPv4/IPv6 default-deny filter with fail-safe dynamic teardown | **Merged to main** (`65f13d4`) |
| **Task 19** | `EXAM-APP-SUPPRESSION-SERVICE-WIRING-1` | Wire active application suppression service lifecycle to terminate unauthorized student apps | **Merged to main** (`b622ac3`) |
| **Task 20** | `CONTROL-CHANNEL-AEAD-ENCRYPTION-1` | Confidentiality & AEAD authenticated encryption (AES-256-GCM) for control plane payloads | **Merged to main** (`23c7e97`) |
| **Task 21** | `INSTRUCTOR-SESSION-AUTHORIZATION-BINDING-1` | Cryptographic instructor authorization tickets binding official assessment sessions | **Merged to main** (`79a4fea`) |
| **Task 22** | `SERVER-SIDE-GRADING-SEALED-EXPORT-1` | Automated objective scoring, manual essay review workflow, and sealed export container | **Merged to main** (`4082646`) |
| **Task 23** | `TEACHER-EXAM-OPERATOR-CONSOLE-1` | Native Dear ImGui teacher console exam management tab with live client monitoring | **Merged to main** (`b2b3024`) |

## Acceptance That Remains Open Before Production Release

1. **Physical Lab Soak & Matrix**: Running the full 50-client physical hardware, network switch, and long-duration soak test checklist in [PRODUCTION_VALIDATION.md](PRODUCTION_VALIDATION.md).
2. **Persistent VM Qualification**: Exercising real WebView2 runtime installation and UWF reboot-to-restore cycles in an isolated persistent VM (per `tasks/task-plans/qualify-real-webview2-vm-1.md`).
3. **Authenticode Production Signing**: Signing all binaries and installer packages with a genuine, trusted CA code-signing certificate (replacing development test signatures).
4. **Independent Security Audit**: Fuzzing binary protocols, named pipes, and WFP filters prior to official high-stakes assessment deployments.
