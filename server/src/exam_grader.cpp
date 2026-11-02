#include "nstu/exam_grader.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace nstu::server::grading {

namespace {

std::string trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return std::string(s);
}

std::string to_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

std::string bytes_to_string(const std::vector<std::byte>& bytes) {
    std::string s;
    s.reserve(bytes.size());
    for (std::byte b : bytes) {
        s.push_back(static_cast<char>(b));
    }
    return s;
}

// Minimal, robust JSON helper for extracting string and numeric fields
std::string extract_json_string(std::string_view json, std::string_view key) {
    const std::string search = "\"" + std::string(key) + "\"";
    const auto pos = json.find(search);
    if (pos == std::string_view::npos) return {};
    auto colon = json.find(':', pos + search.size());
    if (colon == std::string_view::npos) return {};
    auto quote1 = json.find('\"', colon + 1);
    if (quote1 == std::string_view::npos) return {};
    auto quote2 = json.find('\"', quote1 + 1);
    if (quote2 == std::string_view::npos) return {};
    return std::string(json.substr(quote1 + 1, quote2 - quote1 - 1));
}

std::int64_t extract_json_int(std::string_view json, std::string_view key, std::int64_t default_val = 0) {
    const std::string search = "\"" + std::string(key) + "\"";
    const auto pos = json.find(search);
    if (pos == std::string_view::npos) return default_val;
    auto colon = json.find(':', pos + search.size());
    if (colon == std::string_view::npos) return default_val;
    std::size_t start = colon + 1;
    while (start < json.size() && (std::isspace(static_cast<unsigned char>(json[start])) || json[start] == '\"')) {
        ++start;
    }
    std::size_t end = start;
    while (end < json.size() && (std::isdigit(static_cast<unsigned char>(json[end])) || json[end] == '-')) {
        ++end;
    }
    if (start == end) return default_val;
    char* endptr = nullptr;
    const auto val = std::strtoll(json.data() + start, &endptr, 10);
    return val;
}

double extract_json_double(std::string_view json, std::string_view key, double default_val = 0.0) {
    const std::string search = "\"" + std::string(key) + "\"";
    const auto pos = json.find(search);
    if (pos == std::string_view::npos) return default_val;
    auto colon = json.find(':', pos + search.size());
    if (colon == std::string_view::npos) return default_val;
    std::size_t start = colon + 1;
    while (start < json.size() && (std::isspace(static_cast<unsigned char>(json[start])) || json[start] == '\"')) {
        ++start;
    }
    std::size_t end = start;
    while (end < json.size() && (std::isdigit(static_cast<unsigned char>(json[end])) || json[end] == '.' || json[end] == '-' || json[end] == 'e' || json[end] == 'E' || json[end] == '+')) {
        ++end;
    }
    if (start == end) return default_val;
    char* endptr = nullptr;
    const auto val = std::strtod(json.data() + start, &endptr);
    return val;
}

bool extract_json_bool(std::string_view json, std::string_view key, bool default_val = false) {
    const std::string search = "\"" + std::string(key) + "\"";
    const auto pos = json.find(search);
    if (pos == std::string_view::npos) return default_val;
    auto colon = json.find(':', pos + search.size());
    if (colon == std::string_view::npos) return default_val;
    const auto rest = json.substr(colon + 1);
    if (rest.find("true") < rest.find("false")) return true;
    if (rest.find("false") < rest.find("true")) return false;
    return default_val;
}

std::vector<std::string> extract_json_string_array(std::string_view json, std::string_view key) {
    std::vector<std::string> result;
    const std::string search = "\"" + std::string(key) + "\"";
    const auto pos = json.find(search);
    if (pos == std::string_view::npos) return result;
    auto bracket1 = json.find('[', pos + search.size());
    if (bracket1 == std::string_view::npos) return result;
    auto bracket2 = json.find(']', bracket1);
    if (bracket2 == std::string_view::npos) return result;

    std::string_view array_content = json.substr(bracket1 + 1, bracket2 - bracket1 - 1);
    std::size_t offset = 0;
    while (offset < array_content.size()) {
        auto q1 = array_content.find('\"', offset);
        if (q1 == std::string_view::npos) break;
        auto q2 = array_content.find('\"', q1 + 1);
        if (q2 == std::string_view::npos) break;
        result.emplace_back(array_content.substr(q1 + 1, q2 - q1 - 1));
        offset = q2 + 1;
    }
    return result;
}

}  // namespace

std::optional<AnswerKey> parse_answer_key_json(
    std::string_view json_text,
    std::string* error) {
    AnswerKey key;
    key.package_id = extract_json_string(json_text, "package_id");

    const auto items_pos = json_text.find("\"items\"");
    if (items_pos == std::string_view::npos) {
        if (error) *error = "missing 'items' array in answer key";
        return std::nullopt;
    }

    auto start_bracket = json_text.find('[', items_pos);
    if (start_bracket == std::string_view::npos) {
        if (error) *error = "invalid 'items' array";
        return std::nullopt;
    }

    // Parse each item object inside items array
    std::size_t curr = start_bracket + 1;
    while (curr < json_text.size()) {
        auto obj_start = json_text.find('{', curr);
        if (obj_start == std::string_view::npos) break;
        auto close_bracket = json_text.find(']', curr);
        if (close_bracket != std::string_view::npos && close_bracket < obj_start) {
            break;
        }

        // Find matching closing brace
        int depth = 1;
        std::size_t obj_end = obj_start + 1;
        while (obj_end < json_text.size() && depth > 0) {
            if (json_text[obj_end] == '{') ++depth;
            else if (json_text[obj_end] == '}') --depth;
            ++obj_end;
        }
        if (depth != 0) {
            if (error) *error = "unbalanced braces in items array";
            return std::nullopt;
        }

        std::string_view item_json = json_text.substr(obj_start, obj_end - obj_start);
        AnswerKeyItem item;
        item.question_id = extract_json_string(item_json, "id");
        if (item.question_id.empty()) {
            item.question_id = extract_json_string(item_json, "question_id");
        }
        const auto type = extract_json_string(item_json, "type");
        if (!type.empty()) {
            item.question_type = type;
        }
        item.points = static_cast<std::uint32_t>(extract_json_int(item_json, "points", 1));
        item.accepted_answers = extract_json_string_array(item_json, "accepted");
        if (item.accepted_answers.empty()) {
            item.accepted_answers = extract_json_string_array(item_json, "accepted_answers");
        }
        if (item.accepted_answers.empty()) {
            const auto single = extract_json_string(item_json, "accepted_answer");
            if (!single.empty()) {
                item.accepted_answers.push_back(single);
            }
        }
        item.case_sensitive = extract_json_bool(item_json, "case_sensitive", false);
        item.trim_whitespace = extract_json_bool(item_json, "trim_whitespace", true);
        if (item_json.find("\"numeric\"") != std::string_view::npos) {
            item.numeric_tolerance = true;
            item.numeric_value = extract_json_double(item_json, "numeric");
            item.numeric_epsilon = extract_json_double(item_json, "epsilon", 1e-4);
        }
        if (item.question_type == "essay" || extract_json_bool(item_json, "manual_review", false)) {
            item.requires_manual_review = true;
        }

        if (!item.question_id.empty()) {
            key.items.push_back(std::move(item));
        }
        curr = obj_end;
    }

    return key;
}

CandidateExamGrade grade_student_answers(
    const AnswerKey& key,
    const std::vector<exam::AnswerState>& answers,
    std::string candidate_id,
    exam::SessionId session_id) {
    CandidateExamGrade grade;
    grade.package_id = key.package_id;
    grade.package_digest = key.package_digest;
    grade.candidate_id = std::move(candidate_id);
    grade.session_id = session_id;

    for (const auto& item : key.items) {
        ItemGradeResult res;
        res.question_id = item.question_id;
        res.question_type = item.question_type;
        res.max_points = item.points;
        res.requires_manual_review = item.requires_manual_review;
        grade.max_points += item.points;

        // Find candidate response for this question
        const auto it = std::find_if(answers.begin(), answers.end(),
            [&](const exam::AnswerState& ans) {
                return ans.question_id == item.question_id && ans.kind == exam::AnswerKind::upsert;
            });

        if (it == answers.end() || it->answer.empty()) {
            res.answered = false;
            res.awarded_points = 0;
            res.correct = false;
            if (res.requires_manual_review) {
                // If question was not answered, manual review is automatically complete with 0 points
                res.manual_review_completed = true;
                res.feedback_notes = "Not answered by candidate";
            }
        } else {
            res.answered = true;
            res.student_answer_text = bytes_to_string(it->answer);

            if (item.requires_manual_review) {
                res.correct = false;
                res.awarded_points = 0;
                res.manual_review_completed = false;
                ++grade.pending_manual_reviews;
            } else {
                // Automatic grading
                bool is_correct = false;
                std::string student_ans = res.student_answer_text;
                if (item.trim_whitespace) {
                    student_ans = trim(student_ans);
                }

                if (item.numeric_tolerance) {
                    char* endptr = nullptr;
                    const double val = std::strtod(student_ans.c_str(), &endptr);
                    if (endptr != student_ans.c_str() &&
                        std::abs(val - item.numeric_value) <= item.numeric_epsilon) {
                        is_correct = true;
                    }
                } else {
                    for (const auto& acc : item.accepted_answers) {
                        std::string target = acc;
                        if (item.trim_whitespace) {
                            target = trim(target);
                        }
                        if (item.case_sensitive) {
                            if (student_ans == target) {
                                is_correct = true;
                                break;
                            }
                        } else {
                            if (to_lower(student_ans) == to_lower(target)) {
                                is_correct = true;
                                break;
                            }
                        }
                    }
                }

                res.correct = is_correct;
                res.awarded_points = is_correct ? item.points : 0;
                grade.total_points += res.awarded_points;
            }
        }

        grade.item_results.push_back(std::move(res));
    }

    grade.fully_graded = (grade.pending_manual_reviews == 0);
    if (grade.max_points > 0) {
        grade.score_percentage = (static_cast<double>(grade.total_points) * 100.0) /
                                  static_cast<double>(grade.max_points);
    } else {
        grade.score_percentage = 0.0;
    }

    return grade;
}

std::optional<CandidateExamGrade> grade_from_journal(
    const AnswerKey& key,
    const exam::AnswerJournal& journal,
    const exam::StateRequest& request,
    std::string* error) {
    const auto state_resp = journal.state(request, error);
    if (!state_resp) {
        return std::nullopt;
    }
    return grade_student_answers(key, state_resp->answers, request.candidate_id, request.session_id);
}

bool apply_manual_review(
    CandidateExamGrade& grade,
    const std::string& question_id,
    std::uint32_t awarded_points,
    const std::string& feedback_notes,
    std::string* error) {
    auto it = std::find_if(grade.item_results.begin(), grade.item_results.end(),
        [&](const ItemGradeResult& item) {
            return item.question_id == question_id;
        });

    if (it == grade.item_results.end()) {
        if (error) *error = "question_id not found in exam results";
        return false;
    }

    if (awarded_points > it->max_points) {
        if (error) *error = "awarded points cannot exceed max points for question";
        return false;
    }

    const std::uint32_t prev_points = it->awarded_points;
    const bool was_pending = it->requires_manual_review && !it->manual_review_completed;

    it->awarded_points = awarded_points;
    it->correct = (awarded_points == it->max_points);
    it->manual_review_completed = true;
    if (!feedback_notes.empty()) {
        it->feedback_notes = feedback_notes;
    }

    grade.total_points = (grade.total_points - prev_points) + awarded_points;
    if (was_pending && grade.pending_manual_reviews > 0) {
        --grade.pending_manual_reviews;
    }
    grade.fully_graded = (grade.pending_manual_reviews == 0);

    if (grade.max_points > 0) {
        grade.score_percentage = (static_cast<double>(grade.total_points) * 100.0) /
                                  static_cast<double>(grade.max_points);
    } else {
        grade.score_percentage = 0.0;
    }

    return true;
}

std::string serialize_grade_json(const CandidateExamGrade& grade) {
    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"package_id\": \"" << grade.package_id << "\",\n";
    ss << "  \"candidate_id\": \"" << grade.candidate_id << "\",\n";
    ss << "  \"total_points\": " << grade.total_points << ",\n";
    ss << "  \"max_points\": " << grade.max_points << ",\n";
    ss << "  \"score_percentage\": " << grade.score_percentage << ",\n";
    ss << "  \"fully_graded\": " << (grade.fully_graded ? "true" : "false") << ",\n";
    ss << "  \"pending_manual_reviews\": " << grade.pending_manual_reviews << ",\n";
    ss << "  \"items\": [\n";
    for (std::size_t i = 0; i < grade.item_results.size(); ++i) {
        const auto& item = grade.item_results[i];
        ss << "    {\n";
        ss << "      \"id\": \"" << item.question_id << "\",\n";
        ss << "      \"type\": \"" << item.question_type << "\",\n";
        ss << "      \"max_points\": " << item.max_points << ",\n";
        ss << "      \"awarded_points\": " << item.awarded_points << ",\n";
        ss << "      \"answered\": " << (item.answered ? "true" : "false") << ",\n";
        ss << "      \"correct\": " << (item.correct ? "true" : "false") << ",\n";
        ss << "      \"requires_manual_review\": " << (item.requires_manual_review ? "true" : "false") << ",\n";
        ss << "      \"manual_review_completed\": " << (item.manual_review_completed ? "true" : "false") << ",\n";
        ss << "      \"student_answer\": \"" << item.student_answer_text << "\",\n";
        ss << "      \"feedback\": \"" << item.feedback_notes << "\"\n";
        ss << "    }" << (i + 1 < grade.item_results.size() ? "," : "") << "\n";
    }
    ss << "  ]\n";
    ss << "}\n";
    return ss.str();
}

}  // namespace nstu::server::grading
