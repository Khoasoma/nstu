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

- **C++ Tests**: 71/71 unit and integration test executables pass cleanly under MSVC and MinGW/UCRT64.
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

## Integrated Wake-on-LAN & Honest Support Detection (Roadmap P2.3 / Core & UI)

| Work Area | Scope & Implementation | Status |
|---|---|---|
| **Network MAC & Magic Packet Abstraction** | `MacAddress` struct with case-insensitive colon/hyphen parsing and formatting; AMD 102-byte Magic Packet builder/verifier; UDP broadcast sender using `SO_BROADCAST`, `WinsockRuntime`, and RAII `SocketCloser` in `common/src/wake_on_lan.cpp` | **Merged to main** (`bbd2df3`) |
| **Honest Support Detection & Codec** | `WolSupportState` (`unknown`, `supported`, `unsupported`, `missing_mac`) with bilingual EN/VI labels; protocol command `wol_report = 62` with 8-byte binary codec (`encode_wol_report` / `decode_wol_report`) validating MAC, enum bounds, and reserved padding | **Merged to main** (`bbd2df3`) |
| **Operator Role Authorization** | Added `OperatorAction::wake_on_lan = 12` permitting Teacher (pre-class lab startup), Technician (maintenance), and Developer roles while disallowing unauthenticated callers | **Merged to main** (`bbd2df3`) |
| **Registry State Retention** | `ClientRecord` MAC and `WolSupportState` storage with thread-safe `set_wol_report`; `ClientRegistry::upsert` invariant preserving existing hardware state across status refreshes and reconnects | **Merged to main** (`bbd2df3`) |
| **Server Control Plane Execution** | `ServerControlPlane::wake_client` enforcing exam active containment, role permissions, online rejection ("Client is already online."), honest missing MAC check ("Cannot wake client: MAC address is unknown."), honest unsupported check ("Wake-on-LAN is not supported by client hardware."), and UDP magic packet broadcast with remote audit logging | **Merged to main** (`bbd2df3`) |
| **Classroom Roster Bulk Actions** | `BulkActionKind::wake_on_lan = 7` in `execute_bulk_action` inverting online filters so offline clients are targeted and online clients are skipped; bilingual EN/VI confirmation modal titles, confirmation dialogs, and result status formatters | **Merged to main** (`bbd2df3`) |
| **Hardware Detection & Client Service Dispatch** | `detect_local_hardware_wol()` querying Win32 `GetAdaptersAddresses` for operational physical network adapters, filtering loopback/tunnels, and validating non-zero 6-byte MACs; automated dispatch of `CommandType::wol_report = 62` upon connection in `nstu-service` | **Merged to main** (`b2986ed`) |
| **Teacher Console UI & Diagnostics Badges** | Dear ImGui single-client MAC and color-coded status badges (`● Supported`, `▲ Unsupported`, `○ Missing MAC`, `○ Unknown`), single-client "Wake" button with bilingual disabled tooltips (online, exam, missing MAC, unsupported), workspace toolbar "Wake" bulk button, and confirmation modal with inverted offline semantics and role gating | **Merged to main** (`b2986ed`) |

## Integrated File Distribution & Traversal Defenses (Roadmap P2.3 / Core)

| Work Area | Scope & Implementation | Status |
|---|---|---|
| **Wire Protocol Commands & Codecs** | Protocol commands `file_dist_start = 63`, `file_dist_chunk = 64`, `file_dist_finish = 65`, `file_dist_status = 66`, and `file_dist_cancel = 67` with length-prefixed binary codecs, 64 MiB file size and 64 KiB chunk bounds | **Merged to main** (`b730b6e`) |
| **Path Traversal & Device Name Defense** | `is_safe_filename` rejecting `/`, `\`, `:`, control characters, `..`, leading/trailing spaces/dots, NTFS ADS (`:$DATA`), and all 22 Windows DOS reserved device names (`CON`, `PRN`, `AUX`, `NUL`, `COM1-9`, `LPT1-9`) with arbitrary extensions | **Merged to main** (`b730b6e`) |
| **Filename Sanitization & Staging Containment** | `sanitize_filename` normalizing unsafe characters and reserved stems to safe fallbacks; `compute_staging_path` verifying strict lexical containment inside base staging directory | **Merged to main** (`b730b6e`) |
| **End-to-End Cryptographic Integrity** | 32-byte `Sha256Digest` verification using `nstu::security::sha256` and `constant_time_equal`, rejecting corrupted or mismatched chunks | **Merged to main** (`b730b6e`) |
| **Passive Data Invariant** | Non-negotiable invariant strictly preserving received files as passive data with zero auto-execution | **Merged to main** (`b730b6e`) |
| **Operator Role Matrix** | Added `OperatorAction::distribute_file = 13` permitting Teacher, Technician, and Developer roles while denying unauthenticated callers | **Merged to main** (`0ac4ffa`) |
| **Server Control Plane Execution** | `ServerControlPlane::distribute_file` enforcing role permissions, offline semantics, active exam containment (`exam_active(client_id)` denial with security audit logging), 64 MiB file size limits, and 64 KiB chunked streaming with SHA-256 calculation | **Merged to main** (`0ac4ffa`) |
| **Classroom Roster Bulk Actions** | `BulkActionKind::distribute_file = 8` in `execute_bulk_action` dispatching to online clients, skipping offline machines, with bilingual EN/VI confirmation modal titles, confirmation dialogs, and result status formatters | **Merged to main** (`0ac4ffa`) |
| **Client Receiver Engine & Safe Staging** | `client/service/main.cpp` handling `file_dist_start`, `file_dist_chunk`, `file_dist_finish`, `file_dist_cancel`, staging to `%PUBLIC%\Documents\NSTU_Received` via atomic `.nstu_part` streaming, full SHA-256 verification before rename commit, cleanup on hash mismatch, cancellation, and disconnect, exam lockdown rejection, and strict zero auto-execution | **Merged to main** (`4ac60be`) |
| **Teacher Console UI & Modal Validation** | `server/app/main.cpp` single-client and toolbar bulk "Send File" triggers, real-time modal path existence, regular file, 64 MiB limit, and safe filename validation, and `OperatorAction::distribute_file` permission enforcement | **Merged to main** (`4ac60be`) |

## Integrated Demonstration Modes (Roadmap P2.3 / Tooling)

| Work Area | Scope & Implementation | Status |
|---|---|---|
| **Wire Protocol Commands & Domain Types** | `CommandType::host_broadcast_mode = 68` protocol command; `DemonstrationMode` enum class (`focus = 0`, `follow_along = 1`), and `BroadcastModeMessage` struct (`mode`, `lock_input`) | **Merged to main** (`0f7210c`) |
| **Fail-Closed Binary Codecs** | `encode_broadcast_mode` and `decode_broadcast_mode` enforcing strict 4-byte buffer size, LE version 1 negotiation, enum bounds checks, boolean bounds checks, and instant rejection of trailing bytes | **Merged to main** (`0f7210c`) |
| **Isolation & Assertion Baseline** | Zero assertion removals across all 67 test suites; snapshot frame codecs and low-bandwidth snapshot wall monitoring completely unaffected | **Merged to main** (`0f7210c`) |
| **Server Control Plane Demonstration Modes** | `ServerControlPlane::set_broadcast_mode` and `broadcast_mode` managing active demonstration modes (`focus` vs `follow_along`), enum bounds checking, and broadcasting `host_broadcast_mode` | **Merged to main** (`bf5a254`) |
| **Input Lock & Resource Cleanup Guarantee** | `stop_host_broadcast` unconditionally clearing `lock_input = false` on control plane and dispatching `host_broadcast_stop`; `stop()` resetting demonstration state | **Merged to main** (`bf5a254`) |
| **Client Service Broadcast Mode Relay** | `client/service/main.cpp` handling `protocol::CommandType::host_broadcast_mode`, verifying payload fail-closed via `control::decode_broadcast_mode`, and relaying to local agent queue | **Merged to main** (`7e0d884`) |
| **Agent IPC Channel & Type Validation** | `client/include/nstu/agent_protocol.hpp` declaring `AgentMessageType::host_broadcast_mode = 41`, `client/src/agent_protocol.cpp` updating valid type bounds, and `tests/client_ipc_tests.cpp` validating serialization, deserialization, queue transport, and out-of-bounds rejection | **Merged to main** (`7e0d884`) |
| **Student Agent Dual Presentation & Input Control** | `client/agent/main.cpp` implementing Focus Mode (fullscreen popup topmost, `apply_broadcast_input_lock`) and Follow-Along Mode (resizable floating overlapped window, non-topmost, close hides window), `kBroadcastModeChangedMessage = WM_APP + 9`, and gating `restore_control_window_order` | **Merged to main** (`2a73a58`) |
| **Fail-Safe Teardown & Cooperative Lock Cleanup** | `apply_broadcast_input_lock` cooperatively releasing Win32 `BlockInput` without clobbering active remote control sessions; unconditional input unlocking and window hiding upon server disconnect, pipe loss, broadcast stop, and agent destruction | **Merged to main** (`2a73a58`) |
| **Teacher Console Demonstration Ribbon Controls** | `server/app/main.cpp` ribbon UI with dual mode switcher ("Focus Mode" vs "Follow-Along"), "Lock input" checkbox disabled during Follow-Along mode, and dynamic runtime mode updates | **Merged to main** (`2a73a58`) |

## Integrated Fleet Reboot-to-Restore & Diagnostics (Stream S5)

| Work Area | Scope & Implementation | Status |
|---|---|---|
| **Typed UWF WMI Management** | `UwfWmi` interface and `ProductionUwfWmi` implementation; safe RAM/disk overlay sizing (`uwf_maximum_size_mb`), strictly ordered warning/critical thresholds, and transactional rollback | **Merged to main** (`471553e`) |
| **UWF Overlay Reporting Codec** | `nstu::uwf::OverlayReport` 23-byte binary codec with bounds validation, WMI signed VT_I4 limits, and pending-reboot threshold handling | **Merged to main** (`471553e`) |
| **Servicing & Decommissioning** | `set_uwf_servicing` for Windows Update maintenance, `disable_uwf` for controlled decommissioning, and `add_uwf_defender_exclusions` | **Merged to main** (`471553e`) |

## Integrated Fleet Health & Telemetry Diagnostics (Roadmap P3.1 / FLEET-01 & FLEET-02)

| Work Area | Scope & Implementation | Status |
|---|---|---|
| **Stale-Data Detection & Age Calculation** | Pure `is_client_stale`, `client_age_seconds`, and `format_relative_age` in `server/include/nstu/classroom_roster.hpp` and `server/src/classroom_roster.cpp` enforcing monotonic delta evaluation, uninitialized sentinel handling, and negative delta clamping without intrusive polling | **Merged to main** (`f22b3f4`) |
| **Technician Fleet Table Telemetry & Freshness** | `server/app/main.cpp` expanded `fleet-health-table` (9 columns) displaying formatted relative age, amber `[Stale]` warning badge with hover tooltip for stale online clients, live activity states (`Streaming`, `Capturing`, `Viewing Screen`, `Idle`), and enriched delivery/WoL/MAC tooltips | **Merged to main** (`f22b3f4`) |
| **Roster Diagnostics Test Suite** | `tests/classroom_roster_tests.cpp` `test_stale_data_detection()` covering fresh reporting, 30s threshold boundaries, 45s stale state, minute/hour formatting, offline client exclusion, uninitialized timestamp sentinel, and EN/VI localization | **Merged to main** (`f22b3f4`) |
| **Formal Fleet Telemetry Privacy Contract** | `docs/FLEET_TELEMETRY_PRIVACY_CONTRACT.md` codifying strict negative collection boundaries (zero keystrokes, clipboard, URLs, student identity, process lists, or CPU/RAM polling), low-frequency triggers, Technician/Developer RBAC restrictions, and volatile memory-only retention | **Merged to main** (`1964c3f`) |
| **Fleet Metadata Protocol & Wire Codecs** | `CommandType::fleet_metadata = 69` protocol command; `FleetMetadataMessage` struct, fail-closed `encode_fleet_metadata` and `decode_fleet_metadata` codecs with version negotiation and string bounding ($\le 32$ bytes), and unit tests in `tests/control_channel_tests.cpp` | **Merged to main** (`1964c3f`) |
| **Server Registry & Ingestion Engine** | `ClientRecord` extension (`client_version`, `agent_connected`), `ClientRegistry::set_fleet_metadata` thread-safe mutation, non-clobbering `upsert` preservation, and fail-closed `control_plane` command dispatch for `CommandType::fleet_metadata` | **Merged to main** (`1336324`) |
| **Technician UI Version & Agent Surfacing** | `server/app/main.cpp` enriched Computer name tooltip with version and agent status, amber `[No Agent]` warning badge in Status column with explanatory tooltip, and aggregate agent active metric in summary card | **Merged to main** (`1336324`) |
| **Server State Registry Test Suite** | `tests/server_state_tests.cpp` test coverage for `set_fleet_metadata` defaults, successful updates, non-clobbering `upsert` persistence, empty version preservation on disconnect, and unknown client rejection | **Merged to main** (`1336324`) |
| **Client Service Metadata & Agent State Sync** | `client/service/main.cpp` dispatching `CommandType::fleet_metadata` on authenticated connection setup and on student agent named-pipe connect/disconnect edges; `tests/client_control_tests.cpp` end-to-end integration test | **Merged to main** (`360ce50`) |
| **Support Bundle Sanitization & Canary Engine** | `common/include/nstu/support_bundle.hpp` and `common/src/support_bundle.cpp` implementing diagnostic data model, negative boundary redaction (user paths, hex keys, secrets, SAS codes, exam answers), fail-closed canary verification, role/consent export evaluation, and unit test suite in `tests/support_bundle_tests.cpp` | **Merged to main** (`331c0fa`) |
| **Support Bundle UI & Technician Live Export** | `server/app/main.cpp` implementing technician/developer support bundle modal, explicit consent checkbox gate, scrollable sanitized markdown preview, clipboard/file export, and live inventory diagnostics wiring; `tests/server_state_tests.cpp` verification suite | **Merged to main** (`15c0172`) |
| **Portable Room Profile Core Engine** | `common/include/nstu/room_profile.hpp` and `common/src/room_profile.cpp` implementing secret-free room profile model, bounded canonical JSON serializer/parser, fail-closed canary rejection (keys, tokens, secrets, tickets, answers), preset URL/app validators, and unit test suite in `tests/room_profile_tests.cpp` | **Merged to main** (`e8ad4c0`) |
| **Room Profile UI & Live Classroom Integration** | `server/app/main.cpp` implementing technician/developer room profile modal, JSON clipboard/file export, multiline import with fail-closed secret canary rejection, live classroom roster synchronization, and `tests/server_state_tests.cpp` verification suite | **Merged to main** (`81691f4`) |

## Integrated Signed Update Pipeline & LTSC Maintenance (Roadmap P3.2 / FLEET-03 & FLEET-04)

| Work Area | Scope & Implementation | Status |
|---|---|---|
| **Signed Update Manifest & Asymmetric Verification** | `common/include/nstu/update_manifest.hpp` and `common/src/update_manifest.cpp` implementing update manifest data model, asymmetric ECDSA P-256 digital signature signing & verification via Windows BCrypt (`BCRYPT_ECDSA_P256_ALGORITHM`) with client public trust root verification, canonical signing payload binding `rollout_percentage`, anti-downgrade and equal-version (`already_up_to_date`) engine, fail-closed JSON parser, and deterministic canary rollout bucketing engine in `tests/update_manifest_tests.cpp` | **Merged to main** (`7a8db01`) |
| **Update Staging & Rollback State Machine** | `common/include/nstu/update_staging.hpp` and `common/src/update_staging.cpp` implementing 11-stage client update lifecycle (`UpdateStagingController`), atomic chunk streaming to `.part` files with incremental BCrypt SHA-256 computation, fail-closed deletion upon mismatch/overflow/truncation, transactional active directory backup, atomic rollback restoration upon validation failure, immediate disk space reclamation, and persistent `staging_record.json` state recording in `tests/update_staging_tests.cpp` | **Merged to main** (`bd8749d`) |

## Fleet Version Inventory & Rollout Draft — INDEV

`FLEET-UPDATE-SERVICE-UI-1A` adds a Technician/Developer version inventory and
session-only release-channel, maintenance-plan and canary-percentage draft.
Teacher access is denied, invalid draft changes preserve the current settings,
and changing to an unauthorized role closes the modal and resets the draft.
Unknown versions and offline reports remain explicitly unknown or last reported;
no approved-target or up-to-date status is inferred.

The manifest and staging modules above remain foundations. This increment does
not deploy packages, send settings to clients, verify thaw/maintenance readiness
or execute rollback. Production updater wiring, authenticated status reports,
canary health/pause, rollback and real VM/LTSC/UWF/Deep Freeze qualification remain
required. This surface and the wider update workflow remain **INDEV**.

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

## Verification Scope and Staging Record Hardening — November 4, 2026

The merged-work tables and verification summaries above record implemented code
and scoped checks. Outstanding original task and checkpoint acceptance, real
service/peer/runtime behavior, isolated VM qualification, physical lab soak,
trusted signing, production readiness and exam acceptance remain open.

These staging record increments are integrated as **INDEV**:

| Increment | Qualified revision | Exact main CI |
|---|---|---|
| Startup schema and configuration-path preflight before recovery | `310b12e812c360fd4147a8bfcff585374d757606` | [41792898775](https://github.com/Khoasoma/nstu/actions/runs/41792898775) |
| Bounded canonical serialization and pre-save oversize refusal | `10b237f6f91ff283a5da4dd0a65f04aa466db883` | [41802619616](https://github.com/Khoasoma/nstu/actions/runs/41802619616) |
| Opened-leaf native admission and preflight read lease | `3164f65e0f819333eec1b046ecdc49f4518104d6` | [41809306480](https://github.com/Khoasoma/nstu/actions/runs/41809306480) |

Each revision passed four Windows build/test jobs with 71 C++ tests per job,
plus the separate Development release job on Ubuntu. Local fast and strict gates
passed all 71 C++ tests and 63 web cases with zero compiler warnings; independent
non-author reviews covered the respective source revisions. The internal VM
installer job supplies build evidence; real guest/runtime qualification remains
required.

The record retains schema 1.0 and its 12 fields, with a 64 KiB size limit.
Serialization counts exact escaped bytes before output allocation and uses
locale-independent decimal values. Oversized serialization is refused before
directory creation or live-record truncation. Startup reads the admitted leaf
through one native handle, rejects reparse entries, multiple hardlinks and
incompatible writable/delete sharing, and closes the reader before memory
assignment or existing interrupted-download recovery. Compatible readers,
refusal preservation, retry and persisted recovery JSON have regression coverage.

Full deferred publication remains pending. Stable/private ancestors and
namespace, retained temporary-object identity, preserved owner/group/ordered
DACL/control flags/mandatory-label metadata, checked complete-file atomic
publication and observable persistence failures still require qualification.
Deferral still advances memory before persistence, and other transitions still
ignore save failures. Post-close races, existing mapped views, crash/power-loss
recovery, signed updater transport/apply, real VM/UWF/Deep Freeze behavior and
production or exam acceptance are not established by these increments or CI.
