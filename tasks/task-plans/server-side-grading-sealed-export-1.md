# Task Plan: SERVER-SIDE-GRADING-SEALED-EXPORT-1

## 1. Context & Motivation
- Candidate answer events are reliably committed to the append-only `AnswerJournal` on the NSTU server.
- However, authoritative evaluation and archiving require:
  1. Automated server-side grading of objective question types (`multiple_choice`, `short_answer`, `listening`, `reading`).
  2. Manual evaluation workflow for open-ended questions (`essay`).
  3. Authoritative scoring aggregation without mutating raw student journals.
  4. Tamper-evident sealed export containers (`.nstugrade`) binding student responses, grade summaries, manifest digests, audit trails, and cryptographic HMAC-SHA256 seals.
- Answer keys must never be deployed to student agent clients, maintaining strict confidentiality.

## 2. Proposed Architecture & Cryptographic Primitives

### 2.1 Exam Grader (`server/include/nstu/exam_grader.hpp`, `server/src/exam_grader.cpp`)
- `AnswerKeyItem`:
  - `question_id`: matching manifest question identifier.
  - `question_type`: `multiple_choice`, `short_answer`, `essay`, `listening`, `reading`.
  - `points`: integer weight (e.g. 1–1000).
  - `accepted_answers`: list of valid answer strings (e.g. choice letter, phrase).
  - `case_sensitive`: boolean flag for text comparison.
  - `trim_whitespace`: whitespace trimming flag.
  - `numeric_tolerance`, `numeric_value`, `numeric_epsilon`: for numeric equality.
  - `requires_manual_review`: true for essays and subjective items.
- `CandidateExamGrade`:
  - Aggregates `total_points`, `max_points`, `score_percentage`, `pending_manual_reviews`, `fully_graded`.
  - Itemized `ItemGradeResult` detailing student response, correctness, awarded points, review status, and feedback notes.
- Functions:
  - `parse_answer_key_json(std::string_view json_text)`
  - `grade_student_answers(const AnswerKey& key, const std::vector<exam::AnswerState>& answers, ...)`
  - `grade_from_journal(const AnswerKey& key, const exam::AnswerJournal& journal, const exam::StateRequest& request)`
  - `apply_manual_review(CandidateExamGrade& grade, const std::string& question_id, std::uint32_t points, const std::string& notes)`

### 2.2 Sealed Export Container (`server/include/nstu/sealed_export.hpp`, `server/src/sealed_export.cpp`)
- Container specification (`.nstugrade`):
  - Magic: `0x5345414CU` ("SEAL")
  - Version: `0x0001`
  - Canonical manifest with SHA-256 digests of each embedded archive section:
    - `grade_summary.json`
    - `student_answers.json`
    - `audit_trail.json`
  - Top-level HMAC-SHA256 seal signature over the manifest digest.
- Functions:
  - `create_sealed_export(const CandidateExamGrade& grade, const std::vector<exam::AnswerState>& answers, const std::vector<std::string>& audit_events, std::span<const std::byte> signing_key) -> std::vector<std::byte>`
  - `verify_sealed_export(std::span<const std::byte> container, std::span<const std::byte> verify_key, SealedExportSummary* out_summary) -> SealedExportVerifyResult`
  - Rejects corrupted seals, mismatched digests, or tampered inner payloads.

### 2.3 Allowlisted Files
- `server/include/nstu/exam_grader.hpp`
- `server/src/exam_grader.cpp`
- `server/include/nstu/sealed_export.hpp`
- `server/src/sealed_export.cpp`
- `server/CMakeLists.txt`
- `tests/CMakeLists.txt`
- `tests/exam_grader_tests.cpp`
- `tasks/task-plans/server-side-grading-sealed-export-1.md`

## 3. Verification Plan
- Fast gate: `./scripts/verify-gate.sh` (0 compiler warnings, all C++ and web tests pass).
- Strict gate: `./scripts/verify-gate.sh --strict` (MSVC / MinGW Release `-Werror`, 0 warnings).
- Feature CI: GitHub Actions Windows matrix.
- Merge to `main` and Main CI.
