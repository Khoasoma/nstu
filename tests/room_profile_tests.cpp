#include "nstu/room_profile.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using namespace nstu::fleet;

void test_default_profile_export_and_import_roundtrip() {
    RoomProfile profile;
    profile.room_name = "Lab 101";

    const std::string json = export_room_profile_json(profile);
    assert(!json.empty());
    assert(json.find("\"room_name\": \"Lab 101\"") != std::string::npos);
    assert(json.find("\"schema_version\": \"1.0\"") != std::string::npos);

    const auto result = import_room_profile_json(json);
    assert(result.ok());
    assert(result.profile.room_name == "Lab 101");
    assert(result.profile.schema_version == "1.0");
    assert(result.profile.grid_columns == 6);
    assert(result.profile.grid_rows == 5);
    assert(result.profile.beacon_interval_seconds == 3);
    assert(result.profile.seats.empty());
    assert(result.profile.allowed_websites.empty());
    assert(result.profile.allowed_applications.empty());
}

void test_populated_profile_roundtrip() {
    RoomProfile original;
    original.schema_version = "1.0";
    original.room_name = "Phong May 02";
    original.room_description = "Lop Tin Hoc K65";
    original.organization = "Dai Hoc Bach Khoa";
    original.grid_columns = 8;
    original.grid_rows = 6;
    original.beacon_interval_seconds = 5;
    original.network_subnet_hint = "192.168.10.0/24";
    original.default_lock_message_en = "Classroom in progress";
    original.default_lock_message_vi = "Lop hoc dang dien ra";

    original.seats.push_back({"pc-01", "A-01", 0, 0});
    original.seats.push_back({"pc-02", "A-02", 0, 1});
    original.seats.push_back({"pc-03", "B-01", 1, 0});

    original.allowed_websites.push_back("https://code.org");
    original.allowed_websites.push_back("https://lms.school.edu/classroom");

    original.allowed_applications.push_back("code.exe");
    original.allowed_applications.push_back("calc.exe");

    const std::string json = export_room_profile_json(original);
    assert(!json.empty());

    const auto result = import_room_profile_json(json);
    assert(result.ok());
    assert(result.profile.room_name == original.room_name);
    assert(result.profile.room_description == original.room_description);
    assert(result.profile.organization == original.organization);
    assert(result.profile.grid_columns == original.grid_columns);
    assert(result.profile.grid_rows == original.grid_rows);
    assert(result.profile.beacon_interval_seconds == original.beacon_interval_seconds);
    assert(result.profile.network_subnet_hint == original.network_subnet_hint);
    assert(result.profile.default_lock_message_en == original.default_lock_message_en);
    assert(result.profile.default_lock_message_vi == original.default_lock_message_vi);

    assert(result.profile.seats.size() == 3);
    assert(result.profile.seats[0].hostname == "pc-01");
    assert(result.profile.seats[0].seat_label == "A-01");
    assert(result.profile.seats[0].row == 0 && result.profile.seats[0].col == 0);
    assert(result.profile.seats[1].hostname == "pc-02");
    assert(result.profile.seats[1].seat_label == "A-02");
    assert(result.profile.seats[1].row == 0 && result.profile.seats[1].col == 1);
    assert(result.profile.seats[2].hostname == "pc-03");
    assert(result.profile.seats[2].seat_label == "B-01");
    assert(result.profile.seats[2].row == 1 && result.profile.seats[2].col == 0);

    assert(result.profile.allowed_websites.size() == 2);
    assert(result.profile.allowed_websites[0] == "https://code.org");
    assert(result.profile.allowed_websites[1] == "https://lms.school.edu/classroom");

    assert(result.profile.allowed_applications.size() == 2);
    assert(result.profile.allowed_applications[0] == "code.exe");
    assert(result.profile.allowed_applications[1] == "calc.exe");
}

void test_negative_boundary_secret_rejection() {
    const std::vector<std::string> forbidden_payloads = {
        R"({"room_name":"Lab 1","secret":"supersecret"})",
        R"({"room_name":"Lab 1","api_key":"1234567890"})",
        R"({"room_name":"Lab 1","password":"admin"})",
        R"({"room_name":"Lab 1","private_key":"abcdef"})",
        R"({"room_name":"Lab 1","enrollment_secret":"xyz"})",
        R"({"room_name":"Lab 1","pairing_key":"sas123"})",
        R"({"room_name":"Lab 1","sas_code":"123456"})",
        R"({"room_name":"Lab 1","student_answer":"Choice B"})",
        R"({"room_name":"Lab 1","ticket":"exam-auth-999"})",
        R"({"room_name":"Lab 1","token":"bearer-abc"})",
        R"({"room_name":"Lab 1","entropy":"fedcba9876543210"})",
        R"({"room_name":"Lab 1","psk":"pre-shared-key-value"})",
        R"({"room_name":"Lab 1","credential":"user:pass"})",
        R"({"room_name":"Lab 1","notes":"Bearer eyJhbGciOiJIUzI1NiJ9"})",
        R"({"room_name":"Lab 1","notes":"password=super_secret_123"})",
        R"({"room_name":"Lab 1","notes":"token=xyz789"})",
        R"({"room_name":"Lab 1","notes":"secret=topsecret"})",
        R"({"room_name":"Lab 1","notes":"-----BEGIN PRIVATE KEY-----"})"
    };

    for (const auto& payload : forbidden_payloads) {
        assert(contains_forbidden_profile_secrets(payload));
        const auto result = import_room_profile_json(payload);
        assert(!result.ok());
        assert(result.error == RoomProfileError::forbidden_secret_detected);
    }
}

void test_preset_validation() {
    // Valid websites
    assert(is_valid_website_preset("https://khanacademy.org"));
    assert(is_valid_website_preset("http://192.168.1.50:8080/portal"));
    assert(is_valid_website_preset("https://code.org/learn?topic=cxx"));

    // Invalid websites
    assert(!is_valid_website_preset(""));
    assert(!is_valid_website_preset("ftp://ftp.example.com"));
    assert(!is_valid_website_preset("javascript:alert(1)"));
    assert(!is_valid_website_preset("file:///C:/Windows/System32/cmd.exe"));
    assert(!is_valid_website_preset("data:text/html;base64,PHNjcmlwdD4="));
    assert(!is_valid_website_preset("blob:https://example.com/uuid"));
    assert(!is_valid_website_preset("https://admin:password@school.edu"));
    assert(!is_valid_website_preset("https://site.org with spaces"));

    // Valid applications
    assert(is_valid_application_preset("notepad.exe"));
    assert(is_valid_application_preset("calc.exe"));
    assert(is_valid_application_preset("Code.EXE"));
    assert(is_valid_application_preset("python-3.11.exe"));

    // Invalid applications
    assert(!is_valid_application_preset(""));
    assert(!is_valid_application_preset("..\\cmd.exe"));
    assert(!is_valid_application_preset("../cmd.exe"));
    assert(!is_valid_application_preset("/bin/bash"));
    assert(!is_valid_application_preset("C:\\Windows\\cmd.exe"));
    assert(!is_valid_application_preset("script.bat"));
    assert(!is_valid_application_preset("app.exe;rm -rf /"));
    assert(!is_valid_application_preset("app.exe | whoami"));
}

void test_hostname_validation() {
    assert(is_valid_hostname("student-pc01"));
    assert(is_valid_hostname("pc.lab1.local"));
    assert(is_valid_hostname("WS10"));

    assert(!is_valid_hostname(""));
    assert(!is_valid_hostname("-invalid"));
    assert(!is_valid_hostname("invalid-"));
    assert(!is_valid_hostname(".invalid"));
    assert(!is_valid_hostname("invalid."));
    assert(!is_valid_hostname("bad..host"));
    assert(!is_valid_hostname("bad*host"));
    assert(!is_valid_hostname("bad_host")); // Underscore forbidden in RFC 1123 hostname
}

void test_oversized_payload_rejection() {
    std::string huge = "{\"room_name\":\"Lab 1\",\"room_description\":\"";
    huge.append(70000, 'A');
    huge += "\"}";

    const auto result = import_room_profile_json(huge);
    assert(!result.ok());
    assert(result.error == RoomProfileError::oversized_payload);
}

void test_bounds_validation() {
    // grid_columns bounds [1, 16]
    {
        const std::string bad_cols = R"({"room_name":"Lab 1","grid_columns":17})";
        const auto res = import_room_profile_json(bad_cols);
        assert(!res.ok());
        assert(res.error == RoomProfileError::validation_failed);
    }
    {
        const std::string zero_cols = R"({"room_name":"Lab 1","grid_columns":0})";
        const auto res = import_room_profile_json(zero_cols);
        assert(!res.ok());
        assert(res.error == RoomProfileError::validation_failed);
    }

    // grid_rows bounds [1, 16]
    {
        const std::string bad_rows = R"({"room_name":"Lab 1","grid_rows":17})";
        const auto res = import_room_profile_json(bad_rows);
        assert(!res.ok());
        assert(res.error == RoomProfileError::validation_failed);
    }

    // beacon_interval bounds [1, 60]
    {
        const std::string bad_interval = R"({"room_name":"Lab 1","beacon_interval_seconds":61})";
        const auto res = import_room_profile_json(bad_interval);
        assert(!res.ok());
        assert(res.error == RoomProfileError::validation_failed);
    }

    // Seat coordinate exceeding grid bounds
    {
        const std::string bad_seat = R"({
            "room_name":"Lab 1",
            "grid_columns": 4,
            "grid_rows": 4,
            "seats": [
                {"hostname": "pc-01", "seat_label": "A-01", "row": 5, "col": 2}
            ]
        })";
        const auto res = import_room_profile_json(bad_seat);
        assert(!res.ok());
        assert(res.error == RoomProfileError::validation_failed);
    }
}

void test_missing_required_fields_and_schema() {
    // Missing room_name
    {
        const std::string missing_name = R"({"room_description":"Empty"})";
        const auto res = import_room_profile_json(missing_name);
        assert(!res.ok());
        assert(res.error == RoomProfileError::missing_required_field);
    }

    // Empty room_name
    {
        const std::string empty_name = R"({"room_name":""})";
        const auto res = import_room_profile_json(empty_name);
        assert(!res.ok());
        assert(res.error == RoomProfileError::missing_required_field);
    }

    // Unsupported schema version
    {
        const std::string bad_schema = R"({"schema_version":"2.0","room_name":"Lab 1"})";
        const auto res = import_room_profile_json(bad_schema);
        assert(!res.ok());
        assert(res.error == RoomProfileError::unsupported_schema);
    }

    // Invalid JSON syntax
    {
        const std::string invalid_syntax = R"({room_name: invalid})";
        const auto res = import_room_profile_json(invalid_syntax);
        assert(!res.ok());
        assert(res.error == RoomProfileError::invalid_json);
    }
}

int main() {
    std::cout << "Running nstu_room_profile_tests...\n";
    test_default_profile_export_and_import_roundtrip();
    test_populated_profile_roundtrip();
    test_negative_boundary_secret_rejection();
    test_preset_validation();
    test_hostname_validation();
    test_oversized_payload_rejection();
    test_bounds_validation();
    test_missing_required_fields_and_schema();
    std::cout << "All nstu_room_profile_tests passed successfully!\n";
    return 0;
}

