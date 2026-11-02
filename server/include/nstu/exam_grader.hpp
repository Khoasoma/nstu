#pragma once

#include "nstu/auth.hpp"
#include "nstu/exam_sync.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nstu::server::grading {

struct AnswerKeyItem {
    std::string question_id;
    std::string question_type = "multiple_choice";
    std::uint32_t points = 1;
    std::vector<std::string> accepted_answers;
    bool case_sensitive = false;
    bool trim_whitespace = true;
    bool numeric_tolerance = false;
    double numeric_value = 0.0;
    double numeric_epsilon = 1e-4;
    bool requires_manual_review = false;
};

struct AnswerKey {
    std::string package_id;
    security::Sha256Digest package_digest{};
    std::vector<AnswerKeyItem> items;
};

struct ItemGradeResult {
    std::string question_id;
    std::string question_type;
    std::uint32_t max_points = 0;
    std::uint32_t awarded_points = 0;
    bool answered = false;
    bool correct = false;
    bool requires_manual_review = false;
    bool manual_review_completed = false;
    std::string student_answer_text;
    std::string feedback_notes;
};

struct CandidateExamGrade {
    std::string package_id;
    security::Sha256Digest package_digest{};
    std::string candidate_id;
    exam::SessionId session_id{};
    std::uint32_t total_points = 0;
    std::uint32_t max_points = 0;
    double score_percentage = 0.0;
    bool fully_graded = false;
    std::size_t pending_manual_reviews = 0;
    std::uint64_t completion_timestamp_ms = 0;
    std::vector<ItemGradeResult> item_results;
};

[[nodiscard]] std::optional<AnswerKey> parse_answer_key_json(
    std::string_view json_text,
    std::string* error = nullptr);

[[nodiscard]] CandidateExamGrade grade_student_answers(
    const AnswerKey& key,
    const std::vector<exam::AnswerState>& answers,
    std::string candidate_id = {},
    exam::SessionId session_id = {});

[[nodiscard]] std::optional<CandidateExamGrade> grade_from_journal(
    const AnswerKey& key,
    const exam::AnswerJournal& journal,
    const exam::StateRequest& request,
    std::string* error = nullptr);

[[nodiscard]] bool apply_manual_review(
    CandidateExamGrade& grade,
    const std::string& question_id,
    std::uint32_t awarded_points,
    const std::string& feedback_notes = {},
    std::string* error = nullptr);

[[nodiscard]] std::string serialize_grade_json(
    const CandidateExamGrade& grade);

}  // namespace nstu::server::grading
