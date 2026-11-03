#include "nstu/file_distribution.hpp"

#include <cassert>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

void test_is_safe_filename() {
    using namespace nstu::file_dist;

    // 1. Valid safe filenames
    assert(is_safe_filename("worksheet1.pdf"));
    assert(is_safe_filename("code_starter.cpp"));
    assert(is_safe_filename("test-data.json"));
    assert(is_safe_filename("lab_notes.txt"));
    assert(is_safe_filename("archive_v1.0.tar.gz"));
    assert(is_safe_filename("image.png"));
    assert(is_safe_filename("assignment_2026_final.docx"));

    // 2. Length limits
    assert(!is_safe_filename("")); // Empty
    const std::string name_255(255, 'a');
    assert(is_safe_filename(name_255));
    const std::string name_256(256, 'a');
    assert(!is_safe_filename(name_256));

    // 3. Directory traversal tokens
    assert(!is_safe_filename("."));
    assert(!is_safe_filename(".."));
    assert(!is_safe_filename("../evil.exe"));
    assert(!is_safe_filename("..\\evil.exe"));
    assert(!is_safe_filename("sub/file.txt"));
    assert(!is_safe_filename("sub\\file.txt"));
    assert(!is_safe_filename("/etc/passwd"));
    assert(!is_safe_filename("C:\\Windows\\System32\\cmd.exe"));

    // 4. Windows forbidden characters
    assert(!is_safe_filename("file<name.txt"));
    assert(!is_safe_filename("file>name.txt"));
    assert(!is_safe_filename("file:stream.txt"));
    assert(!is_safe_filename("evil.exe:$DATA")); // NTFS Alternate Data Stream
    assert(!is_safe_filename("file\"name.txt"));
    assert(!is_safe_filename("file|name.txt"));
    assert(!is_safe_filename("file?name.txt"));
    assert(!is_safe_filename("file*name.txt"));

    // 5. Control characters and non-printable
    assert(!is_safe_filename(std::string("file\0name.txt", 13)));
    assert(!is_safe_filename("file\x01name.txt"));
    assert(!is_safe_filename("file\x1Fname.txt"));
    assert(!is_safe_filename("file\x7Fname.txt"));
    assert(!is_safe_filename("line\nbreak.txt"));

    // 6. Leading or trailing whitespace and dots
    assert(!is_safe_filename(" file.txt"));
    assert(!is_safe_filename("file.txt "));
    assert(!is_safe_filename(".file.txt"));
    assert(!is_safe_filename("file.txt."));
    assert(!is_safe_filename("evil.exe."));

    // 7. Reserved Windows DOS device names
    assert(!is_safe_filename("con"));
    assert(!is_safe_filename("CON"));
    assert(!is_safe_filename("con.txt"));
    assert(!is_safe_filename("CON.TAR.GZ"));
    assert(!is_safe_filename("prn"));
    assert(!is_safe_filename("PRN.DAT"));
    assert(!is_safe_filename("aux"));
    assert(!is_safe_filename("AUX.CPP"));
    assert(!is_safe_filename("nul"));
    assert(!is_safe_filename("NUL.EXE"));
    assert(!is_safe_filename("com1"));
    assert(!is_safe_filename("COM1.TXT"));
    assert(!is_safe_filename("com9.bin"));
    assert(!is_safe_filename("lpt1"));
    assert(!is_safe_filename("LPT1.PDF"));
    assert(!is_safe_filename("lpt9.tar"));

    // 8. Legitimate names starting with similar substrings
    assert(is_safe_filename("contact.txt"));
    assert(is_safe_filename("conference.pdf"));
    assert(is_safe_filename("printer.log"));
    assert(is_safe_filename("auxiliary.cpp"));
    assert(is_safe_filename("null_ptr.hpp"));
    assert(is_safe_filename("complex.json"));
}

void test_sanitize_filename() {
    using namespace nstu::file_dist;

    assert(sanitize_filename("worksheet.pdf") == "worksheet.pdf");
    assert(sanitize_filename("../../evil/worksheet.pdf") == "worksheet.pdf");
    assert(sanitize_filename("..\\..\\evil\\notes.txt") == "notes.txt");
    assert(sanitize_filename("my<cool>:file?.pdf") == "my_cool__file_.pdf");
    assert(sanitize_filename("   ...report.docx...   ") == "report.docx");
    assert(sanitize_filename("") == "received_file.bin");
    assert(sanitize_filename("   ") == "received_file.bin");
    assert(sanitize_filename("...") == "received_file.bin");

    // DOS device sanitization
    const auto safe_con = sanitize_filename("con.txt");
    assert(is_safe_filename(safe_con));
    assert(safe_con == "safe_con.txt");

    const auto safe_aux = sanitize_filename("aux");
    assert(is_safe_filename(safe_aux));
    assert(safe_aux == "safe_aux");
}

void test_compute_staging_path() {
    using namespace nstu::file_dist;

    const std::filesystem::path base = std::filesystem::current_path() / "test_staging_dir";

    // Valid path inside staging dir
    const auto path1 = compute_staging_path(base, "worksheet.pdf");
    assert(path1.has_value());
    assert(*path1 == base / "worksheet.pdf");

    // Invalid / unsafe filenames must be rejected
    assert(!compute_staging_path(base, "../evil.exe").has_value());
    assert(!compute_staging_path(base, "..\\evil.exe").has_value());
    assert(!compute_staging_path(base, "con.txt").has_value());
    assert(!compute_staging_path(base, "file:ads").has_value());
    assert(!compute_staging_path(base, "").has_value());
}

void test_file_dist_start_codec() {
    using namespace nstu::file_dist;

    FileDistStartMessage msg;
    msg.session_id = 0x0102030405060708ULL;
    msg.file_size = 1048576; // 1 MiB
    for (std::size_t i = 0; i < 32; ++i) {
        msg.sha256[i] = static_cast<std::byte>(i + 1);
    }
    msg.filename = "lab_assignment.cpp";

    const auto encoded = encode_file_dist_start(msg);
    assert(!encoded.empty());
    assert(encoded.size() == 50 + msg.filename.size());

    const auto decoded = decode_file_dist_start(encoded);
    assert(decoded.has_value());
    assert(decoded->session_id == msg.session_id);
    assert(decoded->file_size == msg.file_size);
    assert(decoded->sha256 == msg.sha256);
    assert(decoded->filename == msg.filename);

    // Rejection on size exceeding kMaxFileSize
    FileDistStartMessage oversized = msg;
    oversized.file_size = kMaxFileSize + 1;
    assert(encode_file_dist_start(oversized).empty());

    // Rejection on unsafe filename
    FileDistStartMessage unsafe_msg = msg;
    unsafe_msg.filename = "../escape.exe";
    assert(encode_file_dist_start(unsafe_msg).empty());

    // Decode rejection on truncated payload
    assert(!decode_file_dist_start(std::span<const std::uint8_t>(encoded.data(), 49)).has_value());
    assert(!decode_file_dist_start(std::span<const std::uint8_t>(encoded.data(), encoded.size() - 1)).has_value());
}

void test_file_dist_chunk_codec() {
    using namespace nstu::file_dist;

    FileDistChunkMessage msg;
    msg.session_id = 0xAABBCCDDEEFF0011ULL;
    msg.offset = 65536;
    msg.data = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80};

    const auto encoded = encode_file_dist_chunk(msg);
    assert(!encoded.empty());
    assert(encoded.size() == 20 + msg.data.size());

    const auto decoded = decode_file_dist_chunk(encoded);
    assert(decoded.has_value());
    assert(decoded->session_id == msg.session_id);
    assert(decoded->offset == msg.offset);
    assert(decoded->data == msg.data);

    // Rejection on chunk exceeding kMaxChunkSize
    FileDistChunkMessage oversized_chunk = msg;
    oversized_chunk.data.resize(kMaxChunkSize + 1);
    assert(encode_file_dist_chunk(oversized_chunk).empty());

    // Decode rejection on truncated payload
    assert(!decode_file_dist_chunk(std::span<const std::uint8_t>(encoded.data(), 19)).has_value());
    assert(!decode_file_dist_chunk(std::span<const std::uint8_t>(encoded.data(), encoded.size() - 1)).has_value());
}

void test_file_dist_finish_codec() {
    using namespace nstu::file_dist;

    FileDistFinishMessage msg;
    msg.session_id = 9988776655443322ULL;

    const auto encoded = encode_file_dist_finish(msg);
    assert(encoded.size() == 8);

    const auto decoded = decode_file_dist_finish(encoded);
    assert(decoded.has_value());
    assert(decoded->session_id == msg.session_id);

    // Reject non-8 byte payload
    assert(!decode_file_dist_finish(std::span<const std::uint8_t>(encoded.data(), 7)).has_value());
}

void test_file_dist_status_codec() {
    using namespace nstu::file_dist;

    const std::vector<FileDistStatus> statuses = {
        FileDistStatus::in_progress,
        FileDistStatus::completed,
        FileDistStatus::hash_mismatch,
        FileDistStatus::write_error,
        FileDistStatus::cancelled,
        FileDistStatus::rejected_exam_active,
        FileDistStatus::rejected_unsafe_path,
        FileDistStatus::rejected_quota_exceeded,
    };

    for (auto st : statuses) {
        FileDistStatusMessage msg;
        msg.session_id = 12345ULL;
        msg.bytes_received = 524288;
        msg.status = st;

        const auto encoded = encode_file_dist_status(msg);
        assert(encoded.size() == 24);

        const auto decoded = decode_file_dist_status(encoded);
        assert(decoded.has_value());
        assert(decoded->session_id == msg.session_id);
        assert(decoded->bytes_received == msg.bytes_received);
        assert(decoded->status == msg.status);
    }

    // Reject invalid status byte
    std::vector<std::uint8_t> invalid_payload(24, 0);
    invalid_payload[16] = 99; // Unknown status enum
    assert(!decode_file_dist_status(invalid_payload).has_value());
}

void test_file_dist_cancel_codec() {
    using namespace nstu::file_dist;

    FileDistCancelMessage msg;
    msg.session_id = 778899ULL;
    msg.reason_code = 404;

    const auto encoded = encode_file_dist_cancel(msg);
    assert(encoded.size() == 16);

    const auto decoded = decode_file_dist_cancel(encoded);
    assert(decoded.has_value());
    assert(decoded->session_id == msg.session_id);
    assert(decoded->reason_code == msg.reason_code);

    assert(!decode_file_dist_cancel(std::span<const std::uint8_t>(encoded.data(), 15)).has_value());
}

void test_status_labels() {
    using namespace nstu::file_dist;

    assert(std::string(to_string(FileDistStatus::completed)) == "completed");
    assert(std::string(to_string(FileDistStatus::hash_mismatch)) == "hash_mismatch");

    assert(std::string(status_label(FileDistStatus::completed, false)) == "Completed");
    assert(std::string(status_label(FileDistStatus::completed, true)) == "Hoàn thành");

    assert(std::string(status_label(FileDistStatus::hash_mismatch, false)) == "Hash Mismatch");
    assert(std::string(status_label(FileDistStatus::hash_mismatch, true)).find("mã băm") != std::string::npos);

    assert(std::string(status_label(FileDistStatus::rejected_exam_active, false)).find("Exam Active") != std::string::npos);
    assert(std::string(status_label(FileDistStatus::rejected_exam_active, true)).find("kỳ thi") != std::string::npos);
}

void test_sha256_verification_and_mismatch() {
    const std::string content = "Hello, NSTU File Distribution Test Content!";
    std::vector<std::byte> bytes(content.size());
    for (std::size_t i = 0; i < content.size(); ++i) {
        bytes[i] = static_cast<std::byte>(content[i]);
    }

    const auto digest = nstu::security::sha256(bytes);
    assert(digest.has_value());

    // Matches itself
    assert(nstu::security::constant_time_equal(*digest, *digest));

    // Mismatched data produces mismatched digest
    std::vector<std::byte> corrupted_bytes = bytes;
    corrupted_bytes[0] = static_cast<std::byte>('X');
    const auto corrupted_digest = nstu::security::sha256(corrupted_bytes);
    assert(corrupted_digest.has_value());
    assert(!nstu::security::constant_time_equal(*digest, *corrupted_digest));
}

} // namespace

int main() {
    std::cout << "Running File Distribution tests...\n";

    test_is_safe_filename();
    std::cout << "PASS: test_is_safe_filename\n";

    test_sanitize_filename();
    std::cout << "PASS: test_sanitize_filename\n";

    test_compute_staging_path();
    std::cout << "PASS: test_compute_staging_path\n";

    test_file_dist_start_codec();
    std::cout << "PASS: test_file_dist_start_codec\n";

    test_file_dist_chunk_codec();
    std::cout << "PASS: test_file_dist_chunk_codec\n";

    test_file_dist_finish_codec();
    std::cout << "PASS: test_file_dist_finish_codec\n";

    test_file_dist_status_codec();
    std::cout << "PASS: test_file_dist_status_codec\n";

    test_file_dist_cancel_codec();
    std::cout << "PASS: test_file_dist_cancel_codec\n";

    test_status_labels();
    std::cout << "PASS: test_status_labels\n";

    test_sha256_verification_and_mismatch();
    std::cout << "PASS: test_sha256_verification_and_mismatch\n";

    std::cout << "All File Distribution tests passed successfully!\n";
    return 0;
}

