#include "nstu/url_validation.hpp"
#include "nstu/control_messages.hpp"
#include "nstu/classroom_roster.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

namespace {

void test_valid_urls() {
    using nstu::url::is_valid_launch_url;
    std::string err;

    assert(is_valid_launch_url("https://classroom.google.com", &err));
    assert(err.empty());

    assert(is_valid_launch_url("http://example.org/", &err));
    assert(err.empty());

    assert(is_valid_launch_url("https://math.edu/quiz?id=10&page=2#question-5", &err));
    assert(err.empty());

    assert(is_valid_launch_url("http://192.168.1.50:8080/portal", &err));
    assert(err.empty());

    assert(is_valid_launch_url("https://sub.domain.co.uk/path/to/resource.pdf", &err));
    assert(err.empty());

    assert(is_valid_launch_url("https://example.com/search?q=C%2B%2B%20programming", &err));
    assert(err.empty());

    // Case-insensitive scheme
    assert(is_valid_launch_url("HTTP://EXAMPLE.COM/ABC", &err));
    assert(is_valid_launch_url("Https://Example.Com/Abc", &err));
}

void test_invalid_urls() {
    using nstu::url::is_valid_launch_url;
    std::string err;

    // Empty
    assert(!is_valid_launch_url("", &err));
    assert(!err.empty());

    // Missing scheme
    err.clear();
    assert(!is_valid_launch_url("google.com", &err));
    assert(!err.empty());

    err.clear();
    assert(!is_valid_launch_url("www.khanacademy.org", &err));
    assert(!err.empty());

    // Dangerous schemes
    err.clear();
    assert(!is_valid_launch_url("file:///C:/Windows/System32/cmd.exe", &err));
    assert(!err.empty());

    err.clear();
    assert(!is_valid_launch_url("javascript:alert(1)", &err));
    assert(!err.empty());

    err.clear();
    assert(!is_valid_launch_url("data:text/html,<script>alert(1)</script>", &err));
    assert(!err.empty());

    err.clear();
    assert(!is_valid_launch_url("vbscript:msgbox", &err));
    assert(!err.empty());

    err.clear();
    assert(!is_valid_launch_url("ms-settings:network", &err));
    assert(!err.empty());

    err.clear();
    assert(!is_valid_launch_url("powershell:Start-Process calc", &err));
    assert(!err.empty());

    err.clear();
    assert(!is_valid_launch_url("cmd:calc.exe", &err));
    assert(!err.empty());

    // Control characters and newlines
    err.clear();
    assert(!is_valid_launch_url("https://example.com/test\r\ncalc.exe", &err));
    assert(!err.empty());

    err.clear();
    std::string with_null = "https://example.com/";
    with_null.push_back('\0');
    with_null.append("evil");
    assert(!is_valid_launch_url(with_null, &err));
    assert(!err.empty());

    err.clear();
    std::string with_del = "https://example.com/";
    with_del.push_back(static_cast<char>(127));
    assert(!is_valid_launch_url(with_del, &err));
    assert(!err.empty());

    // Unencoded whitespace
    err.clear();
    assert(!is_valid_launch_url("https://example.com/path with spaces", &err));
    assert(!err.empty());

    // Quotes and backticks
    err.clear();
    assert(!is_valid_launch_url("https://example.com/\"foo\"", &err));
    assert(!err.empty());

    err.clear();
    assert(!is_valid_launch_url("https://example.com/'foo'", &err));
    assert(!err.empty());

    err.clear();
    assert(!is_valid_launch_url("https://example.com/`calc`", &err));
    assert(!err.empty());

    // Angle brackets
    err.clear();
    assert(!is_valid_launch_url("https://example.com/<script>", &err));
    assert(!err.empty());

    // Shell metacharacters and redirects
    err.clear();
    assert(!is_valid_launch_url("https://example.com/foo|bar", &err));
    assert(!err.empty());

    err.clear();
    assert(!is_valid_launch_url("https://example.com/^caret", &err));
    assert(!err.empty());

    err.clear();
    assert(!is_valid_launch_url("https://example.com\\something", &err));
    assert(!err.empty());

    // Empty host
    err.clear();
    assert(!is_valid_launch_url("https://", &err));
    assert(!err.empty());

    err.clear();
    assert(!is_valid_launch_url("http:///path", &err));
    assert(!err.empty());

    err.clear();
    assert(!is_valid_launch_url("https://?query=1", &err));
    assert(!err.empty());

    // Oversized URL (>2048 bytes)
    std::string oversized = "https://example.com/long?";
    oversized.append(2050, 'a');
    err.clear();
    assert(!is_valid_launch_url(oversized, &err));
    assert(!err.empty());
}

void test_sanitize_launch_url() {
    using nstu::url::sanitize_launch_url;

    assert(sanitize_launch_url("").empty());
    assert(sanitize_launch_url("   \t\r\n  ").empty());

    assert(sanitize_launch_url("  https://classroom.google.com  \n") ==
           "https://classroom.google.com");
    assert(sanitize_launch_url("http://internal.lab/quiz") ==
           "http://internal.lab/quiz");

    // Automatically prepends https:// if missing
    assert(sanitize_launch_url("wikipedia.org") == "https://wikipedia.org");
    assert(sanitize_launch_url("  classroom.google.com/c/12345 ") ==
           "https://classroom.google.com/c/12345");
}

void test_launch_url_codecs() {
    using nstu::control::LaunchUrlMessage;
    using nstu::control::encode_launch_url;
    using nstu::control::decode_launch_url;

    // Roundtrip test
    LaunchUrlMessage msg{.url = "https://example.com/quiz?id=42&sort=asc"};
    auto encoded = encode_launch_url(msg);
    assert(!encoded.empty());

    auto decoded = decode_launch_url(encoded);
    assert(decoded.has_value());
    assert(decoded->url == msg.url);

    // Encoding invalid URL fails
    LaunchUrlMessage dangerous{.url = "file:///C:/malicious.exe"};
    assert(encode_launch_url(dangerous).empty());

    // Truncated payload fails
    std::vector<std::byte> truncated(1, std::byte{0});
    assert(!decode_launch_url(truncated).has_value());

    // Oversized length prefix fails
    std::vector<std::byte> bad_len = {std::byte{0xFF}, std::byte{0x7F}};
    assert(!decode_launch_url(bad_len).has_value());

    // Decoding payload with unsafe URL fails
    std::string unsafe_str = "javascript:alert(1)";
    std::vector<std::byte> fake_payload;
    fake_payload.push_back(static_cast<std::byte>(unsafe_str.size() & 0xFF));
    fake_payload.push_back(static_cast<std::byte>((unsafe_str.size() >> 8) & 0xFF));
    for (char c : unsafe_str) {
        fake_payload.push_back(static_cast<std::byte>(c));
    }
    assert(!decode_launch_url(fake_payload).has_value());
}

void test_bulk_action_formatters() {
    using namespace nstu::server;

    // Title formatters
    assert(format_bulk_confirmation_title(BulkActionKind::launch_url, false) ==
           "Confirm Bulk Open Website");
    assert(format_bulk_confirmation_title(BulkActionKind::launch_url, true) ==
           "Xác nhận mở trang web hàng loạt");

    // Message formatters
    std::string msg_en = format_bulk_confirmation_message(
        BulkActionKind::launch_url, 15, "Lab 3", false);
    assert(msg_en.find("OPEN") != std::string::npos);
    assert(msg_en.find("15") != std::string::npos);
    assert(msg_en.find("Lab 3") != std::string::npos);

    std::string msg_vi = format_bulk_confirmation_message(
        BulkActionKind::launch_url, 15, "Phòng 3", true);
    assert(msg_vi.find("MỞ TRANG WEB") != std::string::npos);
    assert(msg_vi.find("15") != std::string::npos);
    assert(msg_vi.find("Phòng 3") != std::string::npos);

    // Status formatters
    BulkActionResult res{.attempted = 10, .succeeded = 9, .failed = 1, .error_messages = {}};
    std::string status_en = format_bulk_action_result_status(BulkActionKind::launch_url, res, false);
    assert(status_en.find("open website") != std::string::npos);
    assert(status_en.find("9 succeeded") != std::string::npos);
    assert(status_en.find("1 failed") != std::string::npos);

    std::string status_vi = format_bulk_action_result_status(BulkActionKind::launch_url, res, true);
    assert(status_vi.find("mở trang web") != std::string::npos);
    assert(status_vi.find("9 thành công") != std::string::npos);
    assert(status_vi.find("1 thất bại") != std::string::npos);
}

} // namespace

int main() {
    test_valid_urls();
    test_invalid_urls();
    test_sanitize_launch_url();
    test_launch_url_codecs();
    test_bulk_action_formatters();

    std::cout << "All URL validation and launch tests passed successfully.\n";
    return 0;
}

