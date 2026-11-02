# Task Plan: TEACHER-EXAM-OPERATOR-CONSOLE-1

## 1. Context & Motivation
- Control plane APIs exist for launching exams, verifying instructor authorization tickets, synchronizing barriers, and managing live assessment lifecycles (`launch_exam()`, `begin_exam()`, `stop_exam()`, `ready_exam_clients()`, `exam_client_states()`).
- Additionally, Task 22 provided authoritative server-side grading and sealed archival export containers (`.nstugrade`).
- However, the server operator interface in `server/app/main.cpp` previously only contained basic input text fields tied to a single selected client and lacked full cohort orchestration, staged package discovery, live cohort telemetry monitoring, and one-click grading/sealed export.
- Task 23 integrates a full-featured, dedicated **Teacher Exam Operator Console** into the Dear ImGui interface.

## 2. Proposed Architecture & UI Capabilities

### 2.1 Package Discovery & Staging Picker
- Scans known package paths (e.g. `exams/packages`, application data roots, current directory).
- Automatically inspects available `.nstuexam` folders and extracts `package_id`, root path, and package digest.
- Provides a responsive dropdown/selector allowing the operator to switch active packages instantly.

### 2.2 Whole-Cohort Orchestration
- Allows operators to dispatch exams to **ALL online computers** simultaneously or target an individual selected client.
- Synchronized launch: watches client readiness (`ready_exam_clients()`) and provides a single "Begin All Ready" button to reveal exam questions concurrently.
- Emergency cohort-wide "Stop All Exams" or per-client stop.

### 2.3 Live Cohort Telemetry Table
- Real-time tabular monitoring of the entire exam room:
  - Machine hostname & IP address.
  - Connection & security status (reboot-to-restore verified).
  - Assigned candidate ID.
  - Current exam phase (Loading Package, Ready to Begin, In Progress, Stopped).
  - Cryptographic instructor authorization badge.
  - Per-row instant actions (Focus Client, Begin Exam, Stop Exam).

### 2.4 One-Click Grading & Sealed Export Container
- Direct integration with `nstu::server::grading` and `nstu::server::sealed_export`.
- Exports sealed `.nstugrade` container containing manifest, grades, student answers, and audit trail with cryptographic seal signature.
- Displays export location and SHA-256 seal status directly on the console.

## 3. Allowlisted Files
- `server/app/main.cpp`
- `tasks/task-plans/teacher-exam-operator-console-1.md`

## 4. Verification Plan
- Fast gate: `./scripts/verify-gate.sh` (0 compiler warnings, all C++ and web tests pass).
- Strict gate: `./scripts/verify-gate.sh --strict` (MSVC / MinGW Release `-Werror`, 0 warnings).
- Feature CI: GitHub Actions Windows matrix.
- Fast-forward merge to `main` and Main CI.
