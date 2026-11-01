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

- **C++ Tests**: 56/56 unit and integration test executables pass cleanly under MSVC and MinGW/UCRT64.
- **Node.js Web Tests**: 63 browser state recovery and submission tests pass.
- **Local Verification Gate**: `./scripts/verify-gate.sh` (fast gate) and `./scripts/verify-gate.sh --strict` (Release with `-Werror`) pass with 0 warnings.
- **Continuous Integration**: 4/4 Windows jobs on feature branches and 5/5 jobs on `main` passing clean.

## Phase 5 Security Delivery Roadmap (Tasks 18–23)

| Task | Title | Key Objective |
|---|---|---|
| **Task 18** | `WFP-IPV6-DEFAULT-DENY-HARDENING-1` | Enforce WFP ALE IPv6 default-deny filter to prevent IPv6 bypass during active exams |
| **Task 19** | `DYNAMIC-EXAM-IP-TRACKING-1` | Periodic DNS re-resolution and dynamic WFP filter re-arm for allowed CDN origins |
| **Task 20** | `ANTI-TAMPER-HEARTBEAT-KILLSWITCH-1` | Cryptographic HMAC challenge/response heartbeat between agent and service; auto-terminate kiosk on tampering |
| **Task 21** | `SAS-SIMULATION-RECOVERY-1` | Secure Attention Sequence (SAS) software simulation and crash recovery drills |
| **Task 22** | `SECURE-PACKAGE-AUTHORIZATION-1` | Instructor/deployment authorization binding for exam packages and candidate identities |
| **Task 23** | `REAL-ENVIRONMENT-ACCEPTANCE-1` | 50-client lab validation and physical switch performance measurements |

## Acceptance That Remains Open Before Production Release

1. **Physical Lab Soak & Matrix**: Running the full 50-client physical hardware, network switch, and long-duration soak test checklist in [PRODUCTION_VALIDATION.md](PRODUCTION_VALIDATION.md).
2. **Persistent VM Qualification**: Exercising real WebView2 runtime installation and UWF reboot-to-restore cycles in an isolated persistent VM (per `tasks/task-plans/qualify-real-webview2-vm-1.md`).
3. **Authenticode Production Signing**: Signing all binaries and installer packages with a genuine, trusted CA code-signing certificate (replacing development test signatures).
4. **Independent Security Audit**: Fuzzing binary protocols, named pipes, and WFP filters prior to official high-stakes assessment deployments.
