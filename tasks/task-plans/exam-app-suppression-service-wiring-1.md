# EXAM-APP-SUPPRESSION-SERVICE-WIRING-1 Task Plan — November 2, 2026

## 1. Context and Objective

In [tasks/task-plans/phase5-exam-security-delivery-plan.md](phase5-exam-security-delivery-plan.md), Task 19 addresses active application suppression during computer-based exams.

Currently, `nstu::client::ExamAppSuppression` (`client/include/nstu/exam_app_suppression.hpp`) exists as a pure classification policy with built-in protection for critical OS and exam processes (`kProtectedImages`). However, it is not wired into process enumeration or the `nstu-service` supervisor loop (`client/service/main.cpp`). Prohibited background applications (cheat software, external chat, unauthorized browsers) can remain running during an active exam.

Task 19 wires application suppression into the active exam lifecycle:
1. Enumerates running processes in interactive student sessions (Session ID > 0).
2. Enforces the non-negotiable protection floor (`kProtectedImages`) and Session 0 exclusion (never touching Windows services or drivers).
3. Terminates unauthorized user-mode processes in student sessions.
4. Emits structured audit events (`Category::exam`, `action="app_suppress"`).
5. Provides dependency injection seams for unit and integration testing without terminating live system processes.

---

## 2. Technical Architecture & Component Design

### 2.1 Process Representation & Injected Seams
```cpp
struct ProcessSnapshotEntry {
    std::uint32_t pid = 0;
    std::uint32_t session_id = 0;
    std::string image_name;
};

using ProcessEnumeratorFn = std::function<std::vector<ProcessSnapshotEntry>()>;
using ProcessTerminatorFn = std::function<bool(std::uint32_t pid, std::string_view image_name)>;
using SuppressionAuditFn = std::function<void(std::uint32_t pid, std::string_view image_name, std::uint32_t session_id)>;
```

### 2.2 `ExamAppSuppressionExecutor`
Coordinates active suppression:
- Holds `ExamAppSuppression` classifier.
- Accepts `ProcessEnumeratorFn`, `ProcessTerminatorFn`, and optional `SuppressionAuditFn`.
- In `scan(std::uint32_t target_session_id)`:
  - If not armed, does nothing.
  - Enumerates processes via enumerator seam.
  - For each process:
    - Verifies `entry.session_id == target_session_id` (must be > 0; Session 0 is rejected unconditionally).
    - Checks `classifier.should_suppress(entry.image_name)`.
    - If true, invokes terminator seam.
    - If terminated, invokes audit callback and records suppression.
  - Avoids redundant termination calls on processes already being reaped.

### 2.3 Windows Native Seams
- Enumerator: Uses `CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)` with `Process32FirstW` / `Process32NextW` and `ProcessIdToSessionId`.
- Terminator: Uses `OpenProcess(PROCESS_TERMINATE, FALSE, pid)` and `TerminateProcess(hProcess, 1)`.

### 2.4 Service Wiring (`client/service/main.cpp`)
- Instantiate `g_app_suppression_executor`.
- In `agent_supervisor_loop`:
  - Every 2 seconds alongside `g_exam_lockdown_controller.tick(now_ms)`:
  - Calls `g_app_suppression_executor.scan(active_session_id)`.
- Arming / Disarming:
  - Armed when exam starts (`exam_arm_worker` / `exam_start`).
  - Disarmed when exam stops, candidate disconnects, service stops, or watchdog expires.

---

## 3. Allowlisted Product Files (4)

1. `client/include/nstu/exam_app_suppression.hpp`
2. `client/src/exam_app_suppression.cpp`
3. `client/service/main.cpp`
4. `tests/exam_app_suppression_tests.cpp`

---

## 4. Verification & Testing

1. **Unit Tests (`tests/exam_app_suppression_tests.cpp`)**:
   - Test `ExamAppSuppression` pure classification (existing coverage).
   - Test `ExamAppSuppressionExecutor` with mocked process list:
     - Disarmed executor terminates nothing.
     - Armed executor terminates unauthorized apps in session > 0.
     - Armed executor NEVER terminates protected images (`kProtectedImages`) regardless of session.
     - Armed executor NEVER touches Session 0 processes (system services, drivers).
     - Armed executor respects manifest-allowlisted applications.
     - Audit callback records expected PID, image name, and session ID.
   - Safe verification of real Windows process enumeration without termination.
2. **Local Gates**:
   - Fast gate: `./scripts/verify-gate.sh` passes 100% with zero compiler warnings.
   - Strict gate: `./scripts/verify-gate.sh --strict` (Release with `-Werror`) passes 100%.
3. **CI Validation**:
   - Feature branch CI passes 4/4 Windows jobs (`gh run watch`).
   - Main branch CI passes 5/5 jobs on fast-forward merge.

---

## 5. Execution Sequence

1. Author task plan `tasks/task-plans/exam-app-suppression-service-wiring-1.md`.
2. Check out branch `feature/exam-app-suppression-service-wiring`.
3. Implement `ProcessSnapshotEntry`, native enumerator/terminator, and `ExamAppSuppressionExecutor` in `client/include/nstu/exam_app_suppression.hpp` and `client/src/exam_app_suppression.cpp`.
4. Wire executor into `client/service/main.cpp` supervisor loop and exam lifecycle.
5. Expand unit tests in `tests/exam_app_suppression_tests.cpp`.
6. Run fast and strict verification gates.
7. Commit with author `khoasoma <songtu438@gmail.com>`.
8. Push branch, monitor CI, merge `--ff-only` to `main`, and verify main CI.
