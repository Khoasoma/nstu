#include "nstu/exam_grader.hpp"
#include "nstu/sealed_export.hpp"

#include <cassert>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

namespace {

nstu::exam::AnswerEvent make_answer_event(
    const std::string& q_id,
    const std::string& answer_text,
    std::uint64_t seq) {
    nstu::exam::AnswerEvent ev;
    ev.package_id = "test-pack-01";
    ev.package_digest.fill(std::byte{0x11});
    ev.client_id.fill(std::byte{0x22});
    ev.session_id.fill(std::byte{0x33});
    ev.candidate_id = "student-101";
    ev.question_id = q_id;
    ev.question_revision = 1;
    ev.sequence = seq;
    ev.client_time_unix_milliseconds = 1'700'000'000'000ULL + seq;
    ev.kind = nstu::exam::AnswerKind::upsert;
    for (char c : answer_text) {
        ev.answer.push_back(static_cast<std::byte>(c));
    }
    return ev;
}

void test_parse_answer_key_json() {
    const std::string_view valid_json = R"({
        "package_id": "cs-midterm",
        "items": [
            {
                "id": "q1",
                "type": "multiple_choice",
                "points": 2,
                "accepted": ["B", "b"]
            },
            {
                "id": "q2",
                "type": "short_answer",
                "points": 3,
                "accepted": ["Newton"],
                "case_sensitive": false,
                "trim_whitespace": true
            },
            {
                "id": "q3",
                "type": "short_answer",
                "points": 5,
                "numeric": 3.1415,
                "epsilon": 0.001
            },
            {
                "id": "q4",
                "type": "essay",
                "points": 10
            }
        ]
    })";

    std::string error;
    const auto key = nstu::server::grading::parse_answer_key_json(valid_json, &error);
    assert(key.has_value());
    assert(key->package_id == "cs-midterm");
    assert(key->items.size() == 4);

    assert(key->items[0].question_id == "q1");
    assert(key->items[0].question_type == "multiple_choice");
    assert(key->items[0].points == 2);
    assert(key->items[0].accepted_answers.size() == 2);
    assert(!key->items[0].requires_manual_review);

    assert(key->items[1].question_id == "q2");
    assert(key->items[1].points == 3);
    assert(!key->items[1].case_sensitive);
    assert(key->items[1].trim_whitespace);

    assert(key->items[2].question_id == "q3");
    assert(key->items[2].points == 5);
    assert(key->items[2].numeric_tolerance);
    assert(std::abs(key->items[2].numeric_value - 3.1415) < 1e-6);

    assert(key->items[3].question_id == "q4");
    assert(key->items[3].points == 10);
    assert(key->items[3].requires_manual_review);

    // Malformed JSON test
    const std::string_view bad_json = "{\"package_id\": \"test\"}";
    assert(!nstu::server::grading::parse_answer_key_json(bad_json, &error));
}

void test_objective_grading_all_types() {
    nstu::server::grading::AnswerKey key;
    key.package_id = "test-pack-01";

    // Q1: multiple choice
    nstu::server::grading::AnswerKeyItem q1;
    q1.question_id = "q1";
    q1.question_type = "multiple_choice";
    q1.points = 2;
    q1.accepted_answers = {"B"};
    key.items.push_back(q1);

    // Q2: short answer text
    nstu::server::grading::AnswerKeyItem q2;
    q2.question_id = "q2";
    q2.question_type = "short_answer";
    q2.points = 3;
    q2.accepted_answers = {"Isaac Newton", "Newton"};
    q2.case_sensitive = false;
    q2.trim_whitespace = true;
    key.items.push_back(q2);

    // Q3: numeric tolerance
    nstu::server::grading::AnswerKeyItem q3;
    q3.question_id = "q3";
    q3.question_type = "short_answer";
    q3.points = 5;
    q3.numeric_tolerance = true;
    q3.numeric_value = 42.0;
    q3.numeric_epsilon = 0.05;
    key.items.push_back(q3);

    // Q4: essay
    nstu::server::grading::AnswerKeyItem q4;
    q4.question_id = "q4";
    q4.question_type = "essay";
    q4.points = 10;
    q4.requires_manual_review = true;
    key.items.push_back(q4);

    // Student Answers:
    // Q1 -> "B" (correct -> 2 pts)
    // Q2 -> "  newton  " (correct due to trim & case-insensitivity -> 3 pts)
    // Q3 -> "42.02" (correct due to tolerance 0.05 -> 5 pts)
    // Q4 -> "This is an essay about physics." (essay -> requires review, 0 pts initially)
    std::vector<nstu::exam::AnswerState> answers;

    auto make_state = [](const std::string& q_id, const std::string& ans) {
        nstu::exam::AnswerState s;
        s.question_id = q_id;
        s.kind = nstu::exam::AnswerKind::upsert;
        for (char c : ans) s.answer.push_back(static_cast<std::byte>(c));
        return s;
    };

    answers.push_back(make_state("q1", "B"));
    answers.push_back(make_state("q2", "  newton  "));
    answers.push_back(make_state("q3", "42.02"));
    answers.push_back(make_state("q4", "This is an essay about physics."));

    const auto grade = nstu::server::grading::grade_student_answers(
        key, answers, "student-101", {});

    assert(grade.package_id == "test-pack-01");
    assert(grade.candidate_id == "student-101");
    assert(grade.max_points == 20); // 2 + 3 + 5 + 10
    assert(grade.total_points == 10); // 2 + 3 + 5
    assert(std::abs(grade.score_percentage - 50.0) < 1e-4);
    assert(!grade.fully_graded);
    assert(grade.pending_manual_reviews == 1);
    assert(grade.item_results.size() == 4);

    assert(grade.item_results[0].correct && grade.item_results[0].awarded_points == 2);
    assert(grade.item_results[1].correct && grade.item_results[1].awarded_points == 3);
    assert(grade.item_results[2].correct && grade.item_results[2].awarded_points == 5);
    assert(!grade.item_results[3].correct && grade.item_results[3].awarded_points == 0);
    assert(grade.item_results[3].requires_manual_review && !grade.item_results[3].manual_review_completed);
}

void test_manual_review_workflow() {
    nstu::server::grading::AnswerKey key;
    key.package_id = "test-pack-01";

    nstu::server::grading::AnswerKeyItem q1;
    q1.question_id = "q1";
    q1.points = 10;
    q1.requires_manual_review = true;
    key.items.push_back(q1);

    std::vector<nstu::exam::AnswerState> answers;
    nstu::exam::AnswerState s1;
    s1.question_id = "q1";
    s1.kind = nstu::exam::AnswerKind::upsert;
    s1.answer = {std::byte{'A'}, std::byte{'n'}, std::byte{'s'}};
    answers.push_back(s1);

    auto grade = nstu::server::grading::grade_student_answers(key, answers, "student-101", {});
    assert(!grade.fully_graded);
    assert(grade.pending_manual_reviews == 1);
    assert(grade.total_points == 0);

    // Exceeding max points must fail
    std::string error;
    assert(!nstu::server::grading::apply_manual_review(grade, "q1", 15, "Too high", &error));
    assert(grade.total_points == 0);
    assert(!grade.fully_graded);

    // Non-existent question must fail
    assert(!nstu::server::grading::apply_manual_review(grade, "non_existent", 5, "", &error));

    // Valid review
    assert(nstu::server::grading::apply_manual_review(grade, "q1", 8, "Good depth", &error));
    assert(grade.fully_graded);
    assert(grade.pending_manual_reviews == 0);
    assert(grade.total_points == 8);
    assert(std::abs(grade.score_percentage - 80.0) < 1e-4);
    assert(grade.item_results[0].manual_review_completed);
    assert(grade.item_results[0].feedback_notes == "Good depth");

    // Serialization verification
    const auto json = nstu::server::grading::serialize_grade_json(grade);
    assert(json.find("\"total_points\": 8") != std::string::npos);
    assert(json.find("\"fully_graded\": true") != std::string::npos);
}

void test_grade_from_answer_journal() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
                      ("nstu-grade-journal-" + std::to_string(stamp));

    nstu::exam::AnswerJournal journal;
    std::string error;
    assert(journal.open(path, &error));

    const auto ev1 = make_answer_event("q1", "Paris", 1);
    assert(journal.append(ev1, &error).status == nstu::exam::AppendStatus::accepted);

    nstu::server::grading::AnswerKey key;
    key.package_id = ev1.package_id;
    key.package_digest = ev1.package_digest;

    nstu::server::grading::AnswerKeyItem q1;
    q1.question_id = "q1";
    q1.points = 5;
    q1.accepted_answers = {"Paris"};
    key.items.push_back(q1);

    nstu::exam::StateRequest req;
    req.package_id = ev1.package_id;
    req.package_digest = ev1.package_digest;
    req.client_id = ev1.client_id;
    req.session_id = ev1.session_id;
    req.candidate_id = ev1.candidate_id;

    const auto grade = nstu::server::grading::grade_from_journal(key, journal, req, &error);
    assert(grade.has_value());
    assert(grade->total_points == 5);
    assert(grade->fully_graded);

    journal.close();
    std::error_code ec;
    std::filesystem::remove(path, ec);
    auto lock_path = path;
    lock_path += L".lock";
    std::filesystem::remove(lock_path, ec);
}

void test_sealed_export_roundtrip_and_tamper() {
    std::vector<std::byte> signing_key(32);
    for (std::size_t i = 0; i < signing_key.size(); ++i) {
        signing_key[i] = static_cast<std::byte>(0x44u + i);
    }

    nstu::server::grading::CandidateExamGrade grade;
    grade.package_id = "exam-physics";
    grade.candidate_id = "candidate-01";
    grade.total_points = 95;
    grade.max_points = 100;
    grade.score_percentage = 95.0;
    grade.fully_graded = true;

    std::vector<nstu::exam::AnswerState> answers;
    nstu::exam::AnswerState a1;
    a1.question_id = "q1";
    a1.sequence = 1;
    a1.kind = nstu::exam::AnswerKind::upsert;
    a1.answer = {std::byte{'A'}};
    answers.push_back(a1);

    std::vector<std::string> audit_events = {
        "candidate_started",
        "answer_saved_q1",
        "exam_submitted"
    };

    // 1. Create sealed container
    const auto container = nstu::server::sealed_export::create_sealed_export(
        grade, answers, audit_events, signing_key, 1'700'000'000'000ULL);
    assert(!container.empty());

    // 2. Verify valid container
    nstu::server::sealed_export::SealedExportData data;
    auto res = nstu::server::sealed_export::verify_sealed_export(
        container, signing_key, &data);
    assert(res == nstu::server::sealed_export::SealedExportVerifyResult::valid);
    assert(data.manifest.package_id == "exam-physics");
    assert(data.manifest.candidate_id == "candidate-01");
    assert(data.manifest.total_points == 95);
    assert(data.manifest.sections.size() == 3);
    assert(!data.grade_summary_json.empty());
    assert(!data.student_answers_json.empty());
    assert(!data.audit_trail_json.empty());

    // 3. Wrong verification key
    std::vector<std::byte> wrong_key = signing_key;
    wrong_key[0] ^= std::byte{0xFF};
    res = nstu::server::sealed_export::verify_sealed_export(container, wrong_key);
    assert(res == nstu::server::sealed_export::SealedExportVerifyResult::signature_mismatch);

    // 4. Corrupted magic
    auto bad_magic = container;
    bad_magic[0] = std::byte{0x00};
    res = nstu::server::sealed_export::verify_sealed_export(bad_magic, signing_key);
    assert(res == nstu::server::sealed_export::SealedExportVerifyResult::invalid_magic);

    // 5. Tampered manifest payload
    auto bad_manifest = container;
    // Manifest starts after 88 byte header
    bad_manifest[88 + 5] ^= std::byte{0x01};
    res = nstu::server::sealed_export::verify_sealed_export(bad_manifest, signing_key);
    assert(res == nstu::server::sealed_export::SealedExportVerifyResult::manifest_digest_mismatch);

    // 6. Tampered signature
    auto bad_sig = container;
    // Seal signature is located at offset 48..79
    bad_sig[48] ^= std::byte{0xFF};
    res = nstu::server::sealed_export::verify_sealed_export(bad_sig, signing_key);
    assert(res == nstu::server::sealed_export::SealedExportVerifyResult::signature_mismatch);

    // 7. Tampered section payload content
    auto bad_section = container;
    // Flip byte in section payload near end
    bad_section[bad_section.size() - 5] ^= std::byte{0x01};
    res = nstu::server::sealed_export::verify_sealed_export(bad_section, signing_key);
    assert(res == nstu::server::sealed_export::SealedExportVerifyResult::section_digest_mismatch);

    // 8. Truncated payload
    const auto truncated = std::span(container.data(), container.size() - 10);
    res = nstu::server::sealed_export::verify_sealed_export(truncated, signing_key);
    assert(res == nstu::server::sealed_export::SealedExportVerifyResult::payload_truncated);
}

void test_sealed_export_file_io() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
                      ("test_export_" + std::to_string(stamp) + ".nstugrade");

    std::vector<std::byte> signing_key(32, std::byte{0x88});

    nstu::server::grading::CandidateExamGrade grade;
    grade.package_id = "exam-bio";
    grade.candidate_id = "candidate-42";
    grade.total_points = 88;
    grade.max_points = 100;
    grade.score_percentage = 88.0;
    grade.fully_graded = true;

    std::vector<nstu::exam::AnswerState> answers;
    std::vector<std::string> audit_events = {"start", "finish"};

    std::string error;
    assert(nstu::server::sealed_export::export_to_file(
        path, grade, answers, audit_events, signing_key, &error));

    const auto imported = nstu::server::sealed_export::import_from_file(
        path, signing_key, &error);
    assert(imported.has_value());
    assert(imported->manifest.package_id == "exam-bio");
    assert(imported->manifest.total_points == 88);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

}  // namespace

int main() {
    test_parse_answer_key_json();
    test_objective_grading_all_types();
    test_manual_review_workflow();
    test_grade_from_answer_journal();
    test_sealed_export_roundtrip_and_tamper();
    test_sealed_export_file_io();
    return 0;
}
