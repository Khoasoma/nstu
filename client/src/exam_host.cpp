#include "nstu/exam_host.hpp"
#include "nstu/auth.hpp"

#include "nstu/exam_bridge.hpp"
#include "nstu/exam_control.hpp"
#include "nstu/exam_lockdown_controller.hpp"
#include "nstu/exam_sync.hpp"

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <wtsapi32.h>

#if NSTU_HAS_WEBVIEW2
#include <wrl.h>
#include <unknwn.h>
#if defined(__GNUC__) && !defined(_MSC_VER)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#endif
#include <WebView2.h>
#if defined(__GNUC__) && !defined(_MSC_VER)
#pragma GCC diagnostic pop
#endif
#endif

#include <atomic>
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace nstu::client {
namespace {

constexpr wchar_t kExamWindowClass[] = L"NSTU.ExamHost.Window";
constexpr wchar_t kExamWindowTitle[] = L"NSTU Assessment";
constexpr wchar_t kExamVirtualHost[] = L"nstu.exam";
constexpr UINT_PTR kBridgeTimerId = 0x4e535455u;
constexpr UINT kBridgeTimerPeriodMs = 250;
constexpr UINT_PTR kExamAliveTimerId = 0x4e535456u;
constexpr UINT kExamAliveTimerPeriodMs = 10'000;
// The initialization timer uses a generation-derived id.  This matters on
// Win32 because a WM_TIMER queued for a previous window operation can arrive
// after KillTimer() and a new exam can already be initializing.  Distinct ids
// let the window procedure discard that stale notification safely.
constexpr UINT_PTR kInitializationTimerBase = 0x4e535600u;
constexpr UINT kInitializationTimerPeriodMs = 250;
constexpr UINT kInitializationTimeoutMs = 15'000;
constexpr std::size_t kMaximumManifestBytes = 1u * 1024u * 1024u;
constexpr std::size_t kMaximumPackageBytes = 128u * 1024u * 1024u;
constexpr std::size_t kMaximumPackageFiles = 4096;
constexpr std::size_t kMaximumJsonBytes = 128u * 1024u;
constexpr std::size_t kMaximumJsonDepth = 16;
constexpr std::size_t kMaximumJsonMembers = 256;
constexpr std::size_t kMaximumJsonStringBytes = 64u * 1024u;
constexpr UINT kHostMessageExamReady = WM_APP + 0x51;
// Private message used by the technician recovery chord. WM_CLOSE is
// intentionally ignored by the kiosk window, so recovery must be explicit.
constexpr UINT kHostMessageExamStop = WM_APP + 0x52;

void set_error(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

std::wstring utf8_to_wide(std::string_view value) {
    if (value.empty()) {
        return {};
    }
    const int source_length = static_cast<int>(value.size());
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                           value.data(), source_length,
                                           nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            source_length, result.data(), length) != length) {
        return {};
    }
    return result;
}

[[maybe_unused]] std::string wide_to_utf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const int source_length = static_cast<int>(value.size());
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                           value.data(), source_length,
                                           nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            source_length, result.data(), length, nullptr,
                            nullptr) != length) {
        return {};
    }
    return result;
}

#if NSTU_HAS_WEBVIEW2
// Encode host-controlled JSON as a JavaScript string literal before passing it
// to JSON.parse in the document-created bootstrap.  Treating the JSON as a
// raw object expression would make the bootstrap depend on JavaScript object
// literal semantics (notably the special __proto__ key) and would leave
// syntax-sensitive Unicode/control characters at the ABI boundary.
std::wstring javascript_string_literal(std::wstring_view value) {
    std::wstring result;
    result.reserve(value.size() + 2);
    result.push_back(L'"');
    for (const wchar_t character : value) {
        switch (character) {
        case L'"': result += L"\\\""; break;
        case L'\\': result += L"\\\\"; break;
        case L'\b': result += L"\\b"; break;
        case L'\f': result += L"\\f"; break;
        case L'\n': result += L"\\n"; break;
        case L'\r': result += L"\\r"; break;
        case L'\t': result += L"\\t"; break;
        case static_cast<wchar_t>(0x2028): result += L"\\u2028"; break;
        case static_cast<wchar_t>(0x2029): result += L"\\u2029"; break;
        default:
            if (character < 0x20 || character == 0x7f) {
                constexpr wchar_t digits[] = L"0123456789abcdef";
                result += L"\\u00";
                result.push_back(digits[(static_cast<unsigned int>(character) >> 4u) & 0x0fu]);
                result.push_back(digits[static_cast<unsigned int>(character) & 0x0fu]);
            } else {
                result.push_back(character);
            }
            break;
        }
    }
    result.push_back(L'"');
    return result;
}
#endif

template <typename Function>
Function load_function(HMODULE module, const char* name) noexcept {
    const FARPROC raw = module == nullptr ? nullptr : GetProcAddress(module, name);
    Function function{};
    static_assert(sizeof(function) == sizeof(raw));
    std::memcpy(&function, &raw, sizeof(function));
    return function;
}

bool is_zero_digest(std::span<const std::byte> value) noexcept {
    return std::all_of(value.begin(), value.end(),
                       [](std::byte byte) { return byte == std::byte{0}; });
}

template <std::size_t N>
bool parse_hex(std::string_view value, std::array<std::byte, N>& output) {
    if (value.size() != N * 2) {
        return false;
    }
    auto nibble = [](char character) -> int {
        if (character >= '0' && character <= '9') {
            return character - '0';
        }
        if (character >= 'a' && character <= 'f') {
            return character - 'a' + 10;
        }
        if (character >= 'A' && character <= 'F') {
            return character - 'A' + 10;
        }
        return -1;
    };
    for (std::size_t index = 0; index < N; ++index) {
        const int high = nibble(value[index * 2]);
        const int low = nibble(value[index * 2 + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        output[index] = static_cast<std::byte>((high << 4) | low);
    }
    return true;
}

template <typename Array>
std::string hex_string(const Array& bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const std::byte byte : bytes) {
        const auto value = std::to_integer<unsigned int>(byte);
        result.push_back(digits[(value >> 4) & 0x0f]);
        result.push_back(digits[value & 0x0f]);
    }
    return result;
}

bool read_file_bounded(const std::filesystem::path& path, std::size_t limit,
                       std::vector<std::byte>& output, std::string* error) {
    std::error_code status_error;
    const auto status = std::filesystem::status(path, status_error);
    if (status_error || !std::filesystem::is_regular_file(status)) {
        set_error(error, "exam package file is unavailable");
        return false;
    }
    const auto size = std::filesystem::file_size(path, status_error);
    if (status_error || size > limit || size >
        static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
        set_error(error, "exam package file is too large");
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        set_error(error, "exam package file could not be opened");
        return false;
    }
    output.assign(static_cast<std::size_t>(size), std::byte{0});
    if (!output.empty()) {
        input.read(reinterpret_cast<char*>(output.data()),
                   static_cast<std::streamsize>(output.size()));
        if (input.gcount() != static_cast<std::streamsize>(output.size())) {
            set_error(error, "exam package file read failed");
            return false;
        }
    }
    return true;
}

// Check every existing component instead of only the final path. A junction
// or symlink in a parent directory can otherwise be resolved away by
// weakly_canonical() before the containment check runs.
bool path_has_reparse_point(const std::filesystem::path& path) noexcept {
    try {
        if (path.empty()) return false;
        const auto normalized = path.lexically_normal();
        auto has_reparse_attribute = [](const std::filesystem::path& value) {
            const DWORD attributes = GetFileAttributesW(value.c_str());
            return attributes != INVALID_FILE_ATTRIBUTES &&
                   (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        };
        std::filesystem::path prefix = normalized.root_path();
        if (!prefix.empty() && has_reparse_attribute(prefix)) return true;
        for (const auto& component : normalized) {
            if (component == normalized.root_name() ||
                component == normalized.root_directory()) {
                continue;
            }
            prefix /= component;
            if (has_reparse_attribute(prefix)) return true;
        }
    } catch (...) {
        // Callers perform independent existence/type checks. A filesystem
        // exception here must not turn a valid missing-leaf path into an
        // accidental allow; the subsequent canonical/status checks fail
        // closed where the path is required to exist.
    }
    return false;
}

bool path_is_within(const std::filesystem::path& root,
                    const std::filesystem::path& candidate) {
    std::error_code error;
    const auto root_canonical = std::filesystem::weakly_canonical(root, error);
    if (error) {
        return false;
    }
    const auto candidate_canonical =
        std::filesystem::weakly_canonical(candidate, error);
    if (error) {
        return false;
    }
#ifdef _WIN32
    const auto root_text = root_canonical.native();
    const auto candidate_text = candidate_canonical.native();
    if (candidate_text.size() < root_text.size() ||
        _wcsnicmp(candidate_text.c_str(), root_text.c_str(),
                  root_text.size()) != 0) {
        return false;
    }
    return candidate_text.size() == root_text.size() ||
           candidate_text[root_text.size()] == L'\\' ||
           candidate_text[root_text.size()] == L'/';
#else
    const auto relative = std::filesystem::relative(candidate_canonical,
                                                     root_canonical, error);
    return !error && relative.native().find("..") != 0;
#endif
}

// A small bounded JSON reader is used at the native boundary so browser
// messages are parsed structurally rather than by substring matching.  It is
// intentionally limited to the JSON types needed by the exam bridge.
struct JsonValue {
    enum class Kind : std::uint8_t { null_value, boolean, number, string,
                                     object, array };
    Kind kind = Kind::null_value;
    bool boolean = false;
    std::string scalar;
    std::map<std::string, JsonValue> object;
    std::vector<JsonValue> array;
};

void append_codepoint_utf8(std::string& output, std::uint32_t codepoint) {
    if (codepoint <= 0x7fu) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ffu) {
        output.push_back(static_cast<char>(0xc0u | (codepoint >> 6u)));
        output.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
    } else if (codepoint <= 0xffffu) {
        output.push_back(static_cast<char>(0xe0u | (codepoint >> 12u)));
        output.push_back(static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
    } else if (codepoint <= 0x10ffffu) {
        output.push_back(static_cast<char>(0xf0u | (codepoint >> 18u)));
        output.push_back(static_cast<char>(0x80u | ((codepoint >> 12u) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
    }
}

class JsonReader {
public:
    explicit JsonReader(std::string_view input) : input_(input) {}

    bool parse(JsonValue& value, std::string* error) {
        if (input_.size() > kMaximumJsonBytes || !parse_value(value, 0) ||
            !consume_whitespace() || position_ != input_.size()) {
            set_error(error, "invalid or oversized JSON message");
            return false;
        }
        return true;
    }

private:
    bool consume_whitespace() noexcept {
        while (position_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }
        return true;
    }

    bool parse_value(JsonValue& value, std::size_t depth) {
        if (depth > kMaximumJsonDepth) {
            return false;
        }
        consume_whitespace();
        if (position_ >= input_.size()) {
            return false;
        }
        switch (input_[position_]) {
        case '{': return parse_object(value, depth + 1);
        case '[': return parse_array(value, depth + 1);
        case '"':
            value.kind = JsonValue::Kind::string;
            return parse_string(value.scalar);
        case 't':
            if (input_.substr(position_, 4) != "true") return false;
            position_ += 4;
            value.kind = JsonValue::Kind::boolean;
            value.boolean = true;
            return true;
        case 'f':
            if (input_.substr(position_, 5) != "false") return false;
            position_ += 5;
            value.kind = JsonValue::Kind::boolean;
            value.boolean = false;
            return true;
        case 'n':
            if (input_.substr(position_, 4) != "null") return false;
            position_ += 4;
            value.kind = JsonValue::Kind::null_value;
            return true;
        default:
            value.kind = JsonValue::Kind::number;
            return parse_number(value.scalar);
        }
    }

    bool parse_object(JsonValue& value, std::size_t depth) {
        if (input_[position_++] != '{') return false;
        value.kind = JsonValue::Kind::object;
        consume_whitespace();
        if (position_ < input_.size() && input_[position_] == '}') {
            ++position_;
            return true;
        }
        std::size_t members = 0;
        while (position_ < input_.size()) {
            if (++members > kMaximumJsonMembers || input_[position_] != '"') {
                return false;
            }
            std::string key;
            if (!parse_string(key)) return false;
            consume_whitespace();
            if (position_ >= input_.size() || input_[position_++] != ':') {
                return false;
            }
            JsonValue child;
            if (!parse_value(child, depth)) return false;
            if (!value.object.emplace(std::move(key), std::move(child)).second) {
                return false;
            }
            consume_whitespace();
            if (position_ >= input_.size()) return false;
            if (input_[position_] == '}') {
                ++position_;
                return true;
            }
            if (input_[position_++] != ',') return false;
            consume_whitespace();
        }
        return false;
    }

    bool parse_array(JsonValue& value, std::size_t depth) {
        if (input_[position_++] != '[') return false;
        value.kind = JsonValue::Kind::array;
        consume_whitespace();
        if (position_ < input_.size() && input_[position_] == ']') {
            ++position_;
            return true;
        }
        while (position_ < input_.size()) {
            if (value.array.size() >= kMaximumJsonMembers) return false;
            JsonValue child;
            if (!parse_value(child, depth)) return false;
            value.array.push_back(std::move(child));
            consume_whitespace();
            if (position_ >= input_.size()) return false;
            if (input_[position_] == ']') {
                ++position_;
                return true;
            }
            if (input_[position_++] != ',') return false;
            consume_whitespace();
        }
        return false;
    }

    bool parse_string(std::string& output) {
        if (position_ >= input_.size() || input_[position_++] != '"') {
            return false;
        }
        output.clear();
        while (position_ < input_.size()) {
            const unsigned char character =
                static_cast<unsigned char>(input_[position_++]);
            if (character == '"') {
                if (output.size() > kMaximumJsonStringBytes) return false;
                return output.empty() || !utf8_to_wide(output).empty();
            }
            if (character < 0x20u) return false;
            if (character != '\\') {
                output.push_back(static_cast<char>(character));
                if (output.size() > kMaximumJsonStringBytes) return false;
                continue;
            }
            if (position_ >= input_.size()) return false;
            const char escape = input_[position_++];
            switch (escape) {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u': {
                if (input_.size() - position_ < 4) return false;
                std::uint32_t codepoint = 0;
                for (int index = 0; index < 4; ++index) {
                    const char digit = input_[position_++];
                    codepoint <<= 4u;
                    if (digit >= '0' && digit <= '9') codepoint += digit - '0';
                    else if (digit >= 'a' && digit <= 'f') codepoint += digit - 'a' + 10;
                    else if (digit >= 'A' && digit <= 'F') codepoint += digit - 'A' + 10;
                    else return false;
                }
                append_codepoint_utf8(output, codepoint);
                break;
            }
            default: return false;
            }
            if (output.size() > kMaximumJsonStringBytes) return false;
        }
        return false;
    }

    bool parse_number(std::string& output) {
        const std::size_t begin = position_;
        if (position_ < input_.size() && input_[position_] == '-') ++position_;
        if (position_ >= input_.size()) return false;
        if (input_[position_] == '0') {
            ++position_;
        } else {
            if (input_[position_] < '1' || input_[position_] > '9') return false;
            while (position_ < input_.size() &&
                   std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                ++position_;
            }
        }
        if (position_ < input_.size() && input_[position_] == '.') {
            ++position_;
            if (position_ >= input_.size() ||
                !std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                return false;
            }
            while (position_ < input_.size() &&
                   std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                ++position_;
            }
        }
        if (position_ < input_.size() &&
            (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() &&
                (input_[position_] == '+' || input_[position_] == '-')) ++position_;
            if (position_ >= input_.size() ||
                !std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                return false;
            }
            while (position_ < input_.size() &&
                   std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                ++position_;
            }
        }
        output.assign(input_.substr(begin, position_ - begin));
        return true;
    }

    std::string_view input_;
    std::size_t position_ = 0;
};

const JsonValue* member(const JsonValue& object, std::string_view name) {
    if (object.kind != JsonValue::Kind::object) return nullptr;
    const auto found = object.object.find(std::string(name));
    return found == object.object.end() ? nullptr : &found->second;
}

bool string_member(const JsonValue& object, std::string_view name,
                  std::string& output, bool required = true) {
    const auto* value = member(object, name);
    if (value == nullptr) return !required;
    if (value->kind != JsonValue::Kind::string) return false;
    output = value->scalar;
    return true;
}

bool uint64_member(const JsonValue& object, std::string_view name,
                   std::uint64_t& output, bool required = true) {
    const auto* value = member(object, name);
    if (value == nullptr) return !required;
    if (value->kind != JsonValue::Kind::number || value->scalar.empty() ||
        value->scalar.front() == '-') return false;
    const auto* begin = value->scalar.data();
    const auto* end = begin + value->scalar.size();
    const auto parsed = std::from_chars(begin, end, output);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

bool parse_json_object(std::string_view input, JsonValue& output,
                       std::string* error) {
    JsonReader reader(input);
    if (!reader.parse(output, error) ||
        output.kind != JsonValue::Kind::object) {
        set_error(error, "JSON root must be an object");
        return false;
    }
    return true;
}

std::string json_escape(std::string_view value) {
    std::string output;
    output.reserve(value.size() + 8);
    for (const unsigned char character : value) {
        switch (character) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (character < 0x20u) {
                constexpr char digits[] = "0123456789abcdef";
                output += "\\u00";
                output.push_back(digits[(character >> 4) & 0x0f]);
                output.push_back(digits[character & 0x0f]);
            } else {
                output.push_back(static_cast<char>(character));
            }
        }
    }
    return output;
}

bool valid_relative_asset(std::string_view value) {
    if (value.empty() || value.size() > 512 || value.find('\0') != std::string_view::npos) {
        return false;
    }
    if (value.find("://") != std::string_view::npos ||
        value.starts_with("javascript:") || value.starts_with("data:") ||
        value.starts_with('/') || value.starts_with('\\') ||
        (value.size() >= 2 && std::isalpha(static_cast<unsigned char>(value[0])) &&
         value[1] == ':')) {
        return false;
    }
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto slash = value.find_first_of("/\\", start);
        const auto component = value.substr(start, slash == std::string_view::npos
                                                       ? value.size() - start
                                                       : slash - start);
        if (component == "." || component == ".." || component.empty()) {
            return false;
        }
        if (component.back() == '.' || component.back() == ' ') {
            return false;
        }
        for (const unsigned char character : component) {
            if (character < 0x20u || character == 0x7fu ||
                character == ':' || character == '*' || character == '"' ||
                character == '<' || character == '>' || character == '|' ||
                character == '?') {
                return false;
            }
        }
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    return true;
}

bool ascii_case_insensitive_equal(std::string_view left,
                                  std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    const auto fold = [](unsigned char character) noexcept {
        return character >= static_cast<unsigned char>('A') &&
                       character <= static_cast<unsigned char>('Z')
                   ? static_cast<unsigned char>(character - 'A' + 'a')
                   : character;
    };
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto lhs = static_cast<unsigned char>(left[index]);
        const auto rhs = static_cast<unsigned char>(right[index]);
        if (fold(lhs) != fold(rhs)) return false;
    }
    return true;
}

// These files belong to the deployment boundary, not to the browser-visible
// exam surface. Keep the comparison case-insensitive because the package is
// ultimately unpacked on a Windows case-insensitive filesystem and a manifest
// must not bypass the rule by changing the spelling.
bool is_private_package_asset(std::string_view value) noexcept {
    return ascii_case_insensitive_equal(value, "manifest.json") ||
           ascii_case_insensitive_equal(value, "manifest.p7s");
}

bool valid_declared_asset(std::string_view value) noexcept {
    return valid_relative_asset(value) && !is_private_package_asset(value);
}

int uri_hex_digit(unsigned char character) noexcept {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
}

[[maybe_unused]] bool decode_uri_path(std::wstring_view encoded,
                                      std::string& output) {
    const auto utf8 = wide_to_utf8(encoded);
    if (utf8.empty()) return false;
    output.clear();
    output.reserve(utf8.size());
    for (std::size_t index = 0; index < utf8.size();) {
        const auto character = static_cast<unsigned char>(utf8[index]);
        if (character == '%') {
            if (index + 2 >= utf8.size()) return false;
            const int high = uri_hex_digit(
                static_cast<unsigned char>(utf8[index + 1]));
            const int low = uri_hex_digit(
                static_cast<unsigned char>(utf8[index + 2]));
            if (high < 0 || low < 0) return false;
            const auto decoded = static_cast<unsigned char>((high << 4) | low);
            // Encoded separators must not be able to create a new path
            // component after validation. Other Windows-invalid characters
            // are rejected by valid_relative_asset below.
            if (decoded == 0 || decoded == '/' || decoded == '\\' ||
                decoded == '?' || decoded == '#') {
                return false;
            }
            output.push_back(static_cast<char>(decoded));
            index += 3;
            continue;
        }
        if (character < 0x20u || character == 0x7fu) return false;
        output.push_back(static_cast<char>(character));
        ++index;
    }
    if (output.empty() || output.find('\0') != std::string::npos ||
        utf8_to_wide(output).empty()) {
        return false;
    }
    return true;
}

bool programming_id(std::string_view text) {
    const auto alphanumeric = [](unsigned char byte) {
        return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9');
    };
    if (text.empty() || text.size() > exam::programming::kMaximumIdBytes ||
        (!alphanumeric(static_cast<unsigned char>(text.front())) && text.front() != '_')) return false;
    return std::all_of(text.begin(), text.end(), [&](unsigned char byte) {
        return alphanumeric(byte) || byte == '_' || byte == '.' || byte == '-';
    });
}

bool public_fields(const JsonValue& value, const std::set<std::string>& allowed) {
    return value.kind == JsonValue::Kind::object && std::all_of(value.object.begin(), value.object.end(),
        [&](const auto& item) { return allowed.contains(item.first); });
}

bool programming_text(const JsonValue& object, const char* field, std::size_t maximum,
                      bool required = true, bool allow_empty = false) {
    std::string text;
    return string_member(object, field, text, required) && (!required || allow_empty || !text.empty()) &&
        text.size() <= maximum && std::all_of(text.begin(), text.end(), [](unsigned char byte) {
            return (byte >= 32u && byte != 127u) || byte == '\t' || byte == '\n' || byte == '\r';
        });
}

const std::set<std::string> kProgrammingPackageFiles{
    "manifest.json", "manifest.p7s", "exam/web/index.html", "exam/web/app.js", "exam/web/styles.css",
    "exam/shared/programming.js", "exam/web/assets/nstu-brand.png"};

bool programming_manifest_shape(const JsonValue& manifest) {
    const std::set<std::string> fields{"id", "title", "subject", "candidate", "durationSeconds", "contentMode",
        "programmingVersion", "sessionMode", "languages", "questions", "documents", "allowedOrigins"};
    std::string id, mode, session;
    std::uint64_t version = 0, duration = 0;
    if (!public_fields(manifest, fields) || !string_member(manifest, "id", id) || !programming_id(id) ||
        !programming_text(manifest, "title", 240) || !programming_text(manifest, "subject", 120, false, true) ||
        !programming_text(manifest, "candidate", 160, false, true) ||
        !string_member(manifest, "contentMode", mode) || mode != "computer_science" ||
        !string_member(manifest, "sessionMode", session) || (session != "exam" && session != "contest" && session != "practice") ||
        !uint64_member(manifest, "programmingVersion", version) || version != 1 ||
        !uint64_member(manifest, "durationSeconds", duration) || duration < 60 || duration > 86400) return false;
    for (const auto* field : {"documents", "allowedOrigins"}) {
        const auto* value = member(manifest, field);
        if (value && (value->kind != JsonValue::Kind::array || !value->array.empty())) return false;
    }
    const auto* languages = member(manifest, "languages");
    if (!languages || languages->kind != JsonValue::Kind::array || languages->array.size() != 2 ||
        languages->array[0].kind != JsonValue::Kind::string || languages->array[0].scalar != "cpp20" ||
        languages->array[1].kind != JsonValue::Kind::string || languages->array[1].scalar != "python3") return false;
    const auto* questions = member(manifest, "questions");
    if (!questions || questions->kind != JsonValue::Kind::array || questions->array.empty() || questions->array.size() > 64) return false;
    std::set<std::string> ids;
    const std::set<std::string> problem_fields{"id", "type", "revision", "prompt", "instruction", "points", "inputDescription",
        "outputDescription", "constraints", "explanation", "samples", "timeLimitMs", "memoryLimitMiB"};
    for (const auto& question : questions->array) {
        std::string problem_id, type;
        std::uint64_t revision = 0, time = 0, memory = 0, points = 100;
        if (!public_fields(question, problem_fields) || !string_member(question, "id", problem_id) ||
            !programming_id(problem_id) || !ids.insert(problem_id).second ||
            !string_member(question, "type", type) || type != "programming" ||
            !uint64_member(question, "revision", revision) || revision == 0 || revision > UINT32_MAX ||
            !programming_text(question, "prompt", 8000) || !uint64_member(question, "points", points, false) || points > 1000 ||
            !uint64_member(question, "timeLimitMs", time) || time < 100 || time > 10000 ||
            !uint64_member(question, "memoryLimitMiB", memory) || memory < 16 || memory > 512) return false;
        for (const auto* field : {"instruction", "inputDescription", "outputDescription", "constraints", "explanation"}) {
            if (!programming_text(question, field, 4000, false, true)) return false;
        }
        const auto* samples = member(question, "samples");
        if (!samples || samples->kind != JsonValue::Kind::array || samples->array.empty() || samples->array.size() > 8) return false;
        std::set<std::string> sample_ids;
        for (const auto& sample : samples->array) {
            std::string sample_id;
            if (!public_fields(sample, {"id", "input", "output"}) || !string_member(sample, "id", sample_id) ||
                !programming_id(sample_id) || !sample_ids.insert(sample_id).second ||
                !programming_text(sample, "input", 4096, true, true) ||
                !programming_text(sample, "output", 4096, true, true)) return false;
        }
    }
    return true;
}

bool validate_manifest_shape(const JsonValue& manifest, std::string* error) {
    std::string content_mode;
    if (string_member(manifest, "contentMode", content_mode, false) && content_mode == "computer_science") {
        if (programming_manifest_shape(manifest)) return true;
        set_error(error, "CS manifest violates the public programming contract");
        return false;
    }
    std::string id;
    std::string title;
    std::uint64_t duration = 0;
    const auto* questions = member(manifest, "questions");
    if (!string_member(manifest, "id", id) || id.empty() || id.size() > 96 ||
        !string_member(manifest, "title", title) || title.empty() ||
        !uint64_member(manifest, "durationSeconds", duration) || duration < 60 ||
        duration > 86400 || questions == nullptr ||
        questions->kind != JsonValue::Kind::array || questions->array.empty() ||
        questions->array.size() > 500) {
        set_error(error, "exam manifest required fields are invalid");
        return false;
    }
    std::set<std::string> question_ids;
    for (const auto& question : questions->array) {
        std::string question_id;
        std::string type;
        std::string prompt;
        if (!string_member(question, "id", question_id) || question_id.empty() ||
            question_id.size() > 96 || !question_ids.insert(question_id).second ||
            !string_member(question, "type", type) ||
            (type != "multiple_choice" && type != "short_answer" &&
             type != "essay" && type != "listening" && type != "reading") ||
            !string_member(question, "prompt", prompt) || prompt.empty() ||
            prompt.size() > 8000) {
            set_error(error, "exam manifest question is invalid");
            return false;
        }
        for (const auto* field : {"audio", "pdf"}) {
            std::string asset;
            if (!string_member(question, field, asset, false)) {
                set_error(error, "exam manifest asset field is invalid");
                return false;
            }
            if (!asset.empty() && !valid_declared_asset(asset)) {
                set_error(error, "exam manifest contains a non-local asset path");
                return false;
            }
        }
        const auto* options = member(question, "options");
        const bool needs_options = type == "multiple_choice" ||
                                    type == "listening" ||
                                    type == "reading";
        if (needs_options &&
            (options == nullptr || options->kind != JsonValue::Kind::array ||
             options->array.size() < 2 || options->array.size() > 12)) {
            set_error(error, "exam manifest question options are invalid");
            return false;
        }
        if (options != nullptr) {
            if (options->kind != JsonValue::Kind::array ||
                options->array.size() > 12) {
                set_error(error, "exam manifest question options are invalid");
                return false;
            }
            for (const auto& option : options->array) {
                if (option.kind != JsonValue::Kind::string ||
                    option.scalar.empty() || option.scalar.size() > 2000) {
                    set_error(error, "exam manifest option text is invalid");
                    return false;
                }
            }
        }
    }
    const auto* documents = member(manifest, "documents");
    if (documents != nullptr) {
        if (documents->kind != JsonValue::Kind::array || documents->array.size() > 128) {
            set_error(error, "exam manifest documents are invalid");
            return false;
        }
        for (const auto& document : documents->array) {
            std::string url;
            if (!string_member(document, "url", url) ||
                !valid_declared_asset(url)) {
                set_error(error, "exam manifest document path is not local");
                return false;
            }
        }
    }
    const auto* allowed_origins = member(manifest, "allowedOrigins");
    if (allowed_origins != nullptr) {
        if (allowed_origins->kind != JsonValue::Kind::array ||
            allowed_origins->array.size() > 32) {
            set_error(error, "exam manifest allowedOrigins is invalid");
            return false;
        }
        std::set<std::string> seen_origins;
        for (const auto& item : allowed_origins->array) {
            if (item.kind != JsonValue::Kind::string ||
                item.scalar.empty() || item.scalar.size() > 253) {
                set_error(error, "exam manifest allowed origin item is invalid");
                return false;
            }
            if (!seen_origins.insert(item.scalar).second) {
                set_error(error, "exam manifest allowedOrigins contains duplicate entries");
                return false;
            }
            std::string hostname;
            if (!parse_origin_hostname(item.scalar, hostname)) {
                set_error(error, "exam manifest allowed origin item format is invalid");
                return false;
            }
        }
    }
    return true;
}

// The virtual-host mapping points at the unpacked package because the browser
// needs package-relative media paths. Keep a separate allowlist for every
// request so a package page cannot read manifest.json, answer material, or
// other files that are not part of the declared web surface.
bool collect_manifest_assets(const std::string& manifest_json,
                             std::set<std::string>& assets,
                             std::string* error) {
    JsonValue manifest;
    if (!parse_json_object(manifest_json, manifest, error) ||
        !validate_manifest_shape(manifest, error)) {
        return false;
    }
    std::string content_mode;
    if (string_member(manifest, "contentMode", content_mode, false) && content_mode == "computer_science") {
        assets.insert("exam/shared/programming.js");
    }
    const auto* questions = member(manifest, "questions");
    if (questions == nullptr || questions->kind != JsonValue::Kind::array) {
        set_error(error, "exam manifest question list is invalid");
        return false;
    }
    for (const auto& question : questions->array) {
        for (const auto* field : {"audio", "pdf"}) {
            std::string asset;
            if (!string_member(question, field, asset, false)) {
                set_error(error, "exam manifest asset field is invalid");
                return false;
            }
            if (!asset.empty()) assets.insert(std::move(asset));
        }
    }
    const auto* documents = member(manifest, "documents");
    if (documents != nullptr) {
        for (const auto& document : documents->array) {
            std::string asset;
            if (!string_member(document, "url", asset) ||
                !valid_declared_asset(asset)) {
                set_error(error, "exam manifest document path is invalid");
                return false;
            }
            assets.insert(std::move(asset));
        }
    }
    return true;
}

void collect_manifest_question_ids(const std::string& manifest_json,
                                   std::set<std::string>& out) {
    JsonValue manifest;
    if (!parse_json_object(manifest_json, manifest, nullptr)) return;
    const auto* questions = member(manifest, "questions");
    if (questions != nullptr && questions->kind == JsonValue::Kind::array) {
        for (const auto& question : questions->array) {
            std::string qid;
            if (string_member(question, "id", qid, false) && !qid.empty()) {
                out.insert(std::move(qid));
            }
        }
    }
}

bool validate_manifest_asset_files(const std::filesystem::path& package_root,
                                   const std::string& manifest_json,
                                   std::string* error) {
    std::set<std::string> assets;
    if (!collect_manifest_assets(manifest_json, assets, error)) return false;

    for (const auto& asset : assets) {
        // The declaration validator above rejects these names. Keep this
        // check local as well so this helper remains fail-closed if its call
        // sites or manifest parser are changed independently later.
        if (is_private_package_asset(asset)) {
            set_error(error, "exam manifest references a private package file");
            return false;
        }
        const auto wide_asset = utf8_to_wide(asset);
        if (wide_asset.empty()) {
            set_error(error, "exam manifest asset path is not valid UTF-8");
            return false;
        }
        const auto candidate = package_root / std::filesystem::path(wide_asset);
        std::error_code path_error;
        const auto canonical = std::filesystem::weakly_canonical(
            candidate, path_error);
        if (path_error || !path_is_within(package_root, canonical) ||
            path_has_reparse_point(candidate)) {
            set_error(error, "exam manifest asset path is outside the package");
            return false;
        }
        const auto status = std::filesystem::status(candidate, path_error);
        if (path_error || !std::filesystem::is_regular_file(status)) {
            set_error(error, "exam manifest references a missing asset");
            return false;
        }
    }
    return true;
}

struct ContextIdentity {
    std::string package_id;
    security::Sha256Digest package_digest{};
    security::ClientId client_id{};
    exam::SessionId session_id{};
    std::string candidate_id;
};

bool parse_context(std::string_view json, std::string_view manifest_package_id,
                   ContextIdentity& output, std::string* error,
                   bool ordinary_user = false) {
    JsonValue root;
    if (!parse_json_object(json, root, error)) return false;
    if (ordinary_user) {
        const auto* synchronized = member(root, "synchronizedStart");
        if (!public_fields(root, {"packageId", "packageDigestHex", "clientIdHex",
                "sessionIdHex", "candidateId", "synchronizedStart"}) ||
            !programming_text(root, "candidateId", 128) ||
            synchronized == nullptr || synchronized->kind != JsonValue::Kind::boolean ||
            !synchronized->boolean) {
            set_error(error, "ordinary exam context must contain only its bound synchronized identity");
            return false;
        }
    }
    std::string package_id;
    std::string digest;
    std::string client;
    std::string session;
    if (!string_member(root, "packageId", package_id) ||
        !string_member(root, "packageDigestHex", digest) ||
        !string_member(root, "clientIdHex", client) ||
        !string_member(root, "sessionIdHex", session) ||
        !string_member(root, "candidateId", output.candidate_id) ||
        package_id.empty() ||
        package_id.size() > exam::kMaximumPackageIdBytes ||
        output.candidate_id.empty() || output.candidate_id.size() > 128 ||
        !parse_hex(digest, output.package_digest) ||
        !parse_hex(client, output.client_id) ||
        !parse_hex(session, output.session_id) ||
        is_zero_digest(output.package_digest) ||
        is_zero_digest(output.client_id) || is_zero_digest(output.session_id)) {
        set_error(error, "exam context identity is invalid");
        return false;
    }
    if (package_id != manifest_package_id) {
        set_error(error, "exam context package id does not match the manifest");
        return false;
    }
    if (ordinary_user && (digest != hex_string(output.package_digest) ||
        client != hex_string(output.client_id) || session != hex_string(output.session_id))) {
        set_error(error, "ordinary exam context requires canonical lowercase identity hex");
        return false;
    }
    output.package_id = std::move(package_id);
    return true;
}

bool parse_manifest_and_context(const ExamHostOptions& options,
                                ExamPackageReport& report,
                                ContextIdentity& context,
                                std::string* error) {
    if (!exam_host_window_policy(options.policy)) {
        set_error(error, "exam host policy is invalid");
        return false;
    }
    const bool ordinary = options.policy == ExamHostPolicy::ordinary_user;
    if (ordinary) {
        if (!options.allowed_data_root.empty() || !options.require_digest ||
            options.expected_digest_hex.empty()) {
            set_error(error, "ordinary exam host requires an explicit user policy and package pin");
            return false;
        }
        if (!validate_exam_user_data_path(options.allowed_package_root,
                options.package_root, error) ||
            !validate_exam_user_data_path(options.allowed_user_data_root,
                options.user_data_root, error)) return false;
        std::error_code path_error;
        const auto packages = std::filesystem::weakly_canonical(options.allowed_package_root, path_error);
        if (path_error) { set_error(error, "ordinary package policy is unavailable"); return false; }
        const auto profiles = std::filesystem::weakly_canonical(options.allowed_user_data_root, path_error);
        if (path_error || path_is_within(packages, profiles) || path_is_within(profiles, packages)) {
            set_error(error, "ordinary package and profile policies must be separate");
            return false;
        }
    } else if (!options.allowed_data_root.empty() &&
        !validate_exam_path_policy(options.allowed_data_root,
                                   options.package_root, options.web_root, {},
                                   error)) {
        return false;
    }
    if (!ordinary && !options.allowed_user_data_root.empty() &&
        !validate_exam_user_data_path(options.allowed_user_data_root,
                                      options.user_data_root, error)) {
        return false;
    }
    if (!inspect_exam_package(options.package_root, options.web_root,
                              options.expected_digest_hex,
                              options.require_digest, report, error)) {
        return false;
    }
    JsonValue manifest;
    if (!parse_json_object(report.manifest_json, manifest, error) ||
        !validate_manifest_shape(manifest, error)) {
        return false;
    }
    if (ordinary) {
        std::string mode;
        if (!validate_programming_manifest(report.manifest_json, error) ||
            !string_member(manifest, "sessionMode", mode) || mode != "contest") {
            set_error(error, "ordinary exam host requires a public programming contest package");
            return false;
        }
    }
    std::string package_id;
    if (!string_member(manifest, "id", package_id)) {
        set_error(error, "exam manifest id is missing");
        return false;
    }
    if (!parse_context(options.context_json, package_id, context, error, ordinary)) {
        return false;
    }
    security::Sha256Digest package_digest{};
    if (!parse_hex(report.digest_hex, package_digest) ||
        !security::constant_time_equal(package_digest, context.package_digest)) {
        set_error(error, "exam context package digest does not match the package");
        return false;
    }
    return true;
}

bool same_context(const exam::AnswerEvent& event, const ContextIdentity& context) {
    return event.package_id == context.package_id &&
           event.package_digest == context.package_digest &&
           event.client_id == context.client_id &&
           event.session_id == context.session_id &&
           event.candidate_id == context.candidate_id;
}

bool same_context(const exam::StateRequest& request,
                  const ContextIdentity& context) {
    return request.package_id == context.package_id &&
           request.package_digest == context.package_digest &&
           request.client_id == context.client_id &&
           request.session_id == context.session_id &&
           request.candidate_id == context.candidate_id;
}

[[maybe_unused]] bool same_context(const exam::ExamBeginCommand& begin,
                                   const ContextIdentity& context) {
    return begin.package_id == context.package_id &&
           begin.package_digest == context.package_digest &&
           begin.client_id == context.client_id &&
           begin.session_id == context.session_id &&
           begin.candidate_id == context.candidate_id;
}

[[maybe_unused]] bool same_context(const exam::TelemetryEnable& enable,
                                   const ContextIdentity& context) {
    return enable.package_id == context.package_id &&
           enable.package_digest == context.package_digest &&
           enable.client_id == context.client_id &&
           enable.session_id == context.session_id &&
           enable.candidate_id == context.candidate_id;
}

[[maybe_unused]] bool parse_answer_event(const JsonValue& root, const ContextIdentity& context,
                        exam::AnswerEvent& output, std::string* error) {
    const auto* value = member(root, "event");
    const auto& event = value != nullptr ? *value : root;
    std::string package_id;
    std::string digest;
    std::string client;
    std::string session;
    std::string kind;
    std::string answer;
    std::string previous_hash;
    std::string question_id;
    std::string candidate;
    std::uint64_t revision = 0;
    std::uint64_t sequence = 0;
    std::uint64_t client_time = 0;
    if (!string_member(event, "packageId", package_id) ||
        !string_member(event, "packageDigestHex", digest) ||
        !string_member(event, "clientIdHex", client) ||
        !string_member(event, "sessionIdHex", session) ||
        !string_member(event, "candidateId", candidate) ||
        !string_member(event, "questionId", question_id, false) ||
        !uint64_member(event, "questionRevision", revision) ||
        !uint64_member(event, "sequence", sequence) ||
        !uint64_member(event, "clientTimeUnixMilliseconds", client_time) ||
        !string_member(event, "kind", kind) ||
        !string_member(event, "answer", answer, false) ||
        !string_member(event, "previousEventHashHex", previous_hash, false)) {
        set_error(error, "exam answer event fields are invalid");
        return false;
    }
    if (revision == 0 ||
        revision > std::numeric_limits<std::uint32_t>::max()) {
        set_error(error, "exam answer question revision is out of range");
        return false;
    }
    // `answer` and `previousEventHashHex` are optional only for finalize/first
    // events; normalize absent values to their protocol defaults.
    if (member(event, "answer") == nullptr) answer.clear();
    if (member(event, "previousEventHashHex") == nullptr) previous_hash.clear();
    output.package_id = std::move(package_id);
    if (!parse_hex(digest, output.package_digest) ||
        !parse_hex(client, output.client_id) ||
        !parse_hex(session, output.session_id)) {
        set_error(error, "exam answer event identity is not valid hex");
        return false;
    }
    output.candidate_id = std::move(candidate);
    output.question_id = std::move(question_id);
    output.question_revision = static_cast<std::uint32_t>(revision);
    output.sequence = sequence;
    output.client_time_unix_milliseconds = client_time;
    if (kind == "upsert") output.kind = exam::AnswerKind::upsert;
    else if (kind == "clear") output.kind = exam::AnswerKind::clear;
    else if (kind == "finalize") output.kind = exam::AnswerKind::finalize;
    else {
        set_error(error, "exam answer event kind is invalid");
        return false;
    }
    output.answer.assign(reinterpret_cast<const std::byte*>(answer.data()),
                         reinterpret_cast<const std::byte*>(answer.data()) +
                             answer.size());
    if (!parse_hex(previous_hash, output.previous_event_hash) &&
        !previous_hash.empty()) {
        set_error(error, "exam answer predecessor hash is invalid");
        return false;
    }
    if (!same_context(output, context) ||
        !exam::validate_answer_event(output, error)) {
        if (error != nullptr && error->empty()) {
            *error = "exam answer event context does not match";
        }
        return false;
    }
    std::string supplied_hash;
    if (!string_member(event, "eventHashHex", supplied_hash, false)) {
        set_error(error, "exam answer event hash field is invalid");
        return false;
    }
    if (const auto calculated = exam::hash_answer_event(output)) {
        if (!supplied_hash.empty()) {
            security::Sha256Digest supplied{};
            if (!parse_hex(supplied_hash, supplied) ||
                !security::constant_time_equal(*calculated, supplied)) {
                set_error(error, "exam answer event hash does not match");
                return false;
            }
        }
    } else {
        set_error(error, "exam answer event could not be hashed");
        return false;
    }
    return true;
}

[[maybe_unused]] bool parse_state_request(const JsonValue& root, const ContextIdentity& context,
                         exam::StateRequest& output, std::string* error) {
    const auto* value = member(root, "request");
    const auto& request = value != nullptr ? *value : root;
    std::string package_id;
    std::string digest;
    std::string client;
    std::string session;
    if (!string_member(request, "packageId", package_id) ||
        !string_member(request, "packageDigestHex", digest) ||
        !string_member(request, "clientIdHex", client) ||
        !string_member(request, "sessionIdHex", session) ||
        !string_member(request, "candidateId", output.candidate_id) ||
        !parse_hex(digest, output.package_digest) ||
        !parse_hex(client, output.client_id) ||
        !parse_hex(session, output.session_id)) {
        set_error(error, "exam state request fields are invalid");
        return false;
    }
    output.package_id = std::move(package_id);
    if (!same_context(output, context)) {
        set_error(error, "exam state request context does not match");
        return false;
    }
    return true;
}

std::string ack_status_name(exam::AnswerAckStatus status) {
    switch (status) {
    case exam::AnswerAckStatus::accepted: return "accepted";
    case exam::AnswerAckStatus::duplicate: return "duplicate";
    case exam::AnswerAckStatus::rejected: return "rejected";
    case exam::AnswerAckStatus::conflict: return "conflict";
    case exam::AnswerAckStatus::gap: return "gap";
    case exam::AnswerAckStatus::unavailable: return "unavailable";
    }
    return "rejected";
}

[[maybe_unused]] std::string make_ack_json(const exam::AnswerAck& ack) {
    return "{\"type\":\"exam_answer_ack\",\"ack\":{\"status\":\"" +
           ack_status_name(ack.status) + "\",\"sessionIdHex\":\"" +
           hex_string(ack.session_id) + "\",\"sequence\":" +
           std::to_string(ack.sequence) +
           ",\"highestContiguousSequence\":" +
           std::to_string(ack.highest_contiguous_sequence) +
           ",\"serverTimeUnixMilliseconds\":" +
           std::to_string(ack.server_time_unix_milliseconds) +
           ",\"eventHashHex\":\"" + hex_string(ack.event_hash) +
           "\",\"stateHashHex\":\"" + hex_string(ack.state_hash) + "\"}}";
}

[[maybe_unused]] std::string make_begin_json(
    const exam::ExamBeginCommand& begin) {
    return "{\"type\":\"exam_begin\",\"begin\":{"
           "\"packageId\":\"" + json_escape(begin.package_id) +
           "\",\"packageDigestHex\":\"" + hex_string(begin.package_digest) +
           "\",\"clientIdHex\":\"" + hex_string(begin.client_id) +
           "\",\"sessionIdHex\":\"" + hex_string(begin.session_id) +
           "\",\"candidateId\":\"" + json_escape(begin.candidate_id) +
           "\",\"serverStartUnixMilliseconds\":" +
           std::to_string(begin.server_start_unix_milliseconds) +
           ",\"durationSeconds\":" + std::to_string(begin.duration_seconds) +
           "}}";
}

[[maybe_unused]] std::string make_telemetry_enable_json(
    const exam::TelemetryEnable& enable, std::uint64_t collector_id,
    std::uint64_t collector_elapsed_milliseconds, bool startup_retry) {
    return "{\"type\":\"exam_telemetry_enable\",\"enable\":{"
           "\"packageId\":\"" + json_escape(enable.package_id) +
           "\",\"packageDigestHex\":\"" + hex_string(enable.package_digest) +
           "\",\"clientIdHex\":\"" + hex_string(enable.client_id) +
           "\",\"sessionIdHex\":\"" + hex_string(enable.session_id) +
           "\",\"candidateId\":\"" + json_escape(enable.candidate_id) +
           "\",\"enabled\":true,"
           "\"collectorId\":" + std::to_string(collector_id) +
           ",\"collectorElapsedMilliseconds\":" + std::to_string(collector_elapsed_milliseconds) +
           (startup_retry ? ",\"collectorStartupRetry\":true" : "") + "}}";
}

[[maybe_unused]] std::string make_state_json(const exam::StateResponse& response) {
    std::string json = "{\"type\":\"exam_state_response\",\"response\":{";
    json += "\"packageId\":\"" + json_escape(response.package_id) +
            "\",\"packageDigestHex\":\"" + hex_string(response.package_digest) +
            "\",\"clientIdHex\":\"" + hex_string(response.client_id) +
            "\",\"sessionIdHex\":\"" + hex_string(response.session_id) +
            "\",\"candidateId\":\"" + json_escape(response.candidate_id) +
            "\",\"highestContiguousSequence\":" +
            std::to_string(response.highest_contiguous_sequence) +
            ",\"finalized\":" + (response.finalized ? "true" : "false") +
            ",\"stateHashHex\":\"" + hex_string(response.state_hash) +
            "\",\"lastEventHashHex\":\"" + hex_string(response.last_event_hash) +
            "\",\"chunkIndex\":" + std::to_string(response.chunk_index) +
            ",\"chunkCount\":" + std::to_string(response.chunk_count) +
            ",\"answers\":[";
    for (std::size_t index = 0; index < response.answers.size(); ++index) {
        const auto& answer = response.answers[index];
        if (index != 0) json.push_back(',');
        const std::string bytes(reinterpret_cast<const char*>(answer.answer.data()),
                                 answer.answer.size());
        json += "{\"questionId\":\"" + json_escape(answer.question_id) +
                "\",\"questionRevision\":" +
                std::to_string(answer.question_revision) +
                ",\"sequence\":" + std::to_string(answer.sequence) +
                ",\"kind\":\"" +
                (answer.kind == exam::AnswerKind::clear ? "clear" : "upsert") +
                "\",\"answer\":\"" + json_escape(bytes) +
                "\",\"eventHashHex\":\"" + hex_string(answer.event_hash) +
                "\"}";
    }
    json += "]}}";
    return json;
}

} // namespace

bool validate_programming_manifest(std::string_view manifest_json, std::string* error) {
    JsonValue manifest;
    if (manifest_json.size() > 128u * 1024u || !parse_json_object(manifest_json, manifest, error) ||
        !programming_manifest_shape(manifest)) {
        set_error(error, "CS manifest violates the public programming contract");
        return false;
    }
    return true;
}

bool validate_programming_policy_probe_json(std::string_view json, std::uint32_t exit_code) {
    JsonValue reply;
    std::uint64_t version = 0;
    std::string status, code;
    return exit_code == 0 && json.size() <= 4096 && parse_json_object(json, reply, nullptr) &&
        public_fields(reply, {"version", "status", "code"}) && reply.object.size() == 3 &&
        uint64_member(reply, "version", version) && version == 1 &&
        string_member(reply, "status", status) && status == "verified" &&
        string_member(reply, "code", code) && code == "policy_verified";
}

namespace {

struct PolicyHandle {
    HANDLE value = nullptr;
    explicit PolicyHandle(HANDLE handle = nullptr) : value(handle) {}
    ~PolicyHandle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    PolicyHandle(const PolicyHandle&) = delete;
    PolicyHandle& operator=(const PolicyHandle&) = delete;
    bool valid() const noexcept { return value && value != INVALID_HANDLE_VALUE; }
};

struct PolicyAttributes {
    LPPROC_THREAD_ATTRIBUTE_LIST value = nullptr;
    ~PolicyAttributes() { if (value) DeleteProcThreadAttributeList(value); }
};

bool policy_trusted_sid(PSID sid) {
    if (!sid || !IsValidSid(sid)) return false;
    if (IsWellKnownSid(sid, WinLocalSystemSid) || IsWellKnownSid(sid, WinBuiltinAdministratorsSid)) return true;
    PSID installer = nullptr;
    const bool converted = ConvertStringSidToSidW(
        L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464", &installer) != FALSE;
    const bool trusted = converted && EqualSid(sid, installer);
    if (installer) LocalFree(installer);
    return trusted;
}

bool policy_protected_handle(HANDLE handle, bool ancestor) {
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetSecurityInfo(handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                        &owner, nullptr, &dacl, nullptr, &descriptor) != ERROR_SUCCESS) return false;
    bool valid = policy_trusted_sid(owner) && dacl != nullptr;
    const DWORD write_mask = WRITE_DAC | WRITE_OWNER | DELETE | FILE_DELETE_CHILD | FILE_WRITE_DATA |
        FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES | GENERIC_ALL | GENERIC_WRITE |
        (ancestor ? 0 : FILE_APPEND_DATA);
    for (DWORD index = 0; valid && index < dacl->AceCount; ++index) {
        void* raw = nullptr;
        if (!GetAce(dacl, index, &raw)) { valid = false; break; }
        const auto* header = static_cast<const ACE_HEADER*>(raw);
        if (header->AceFlags & INHERIT_ONLY_ACE) continue;
        if (header->AceType == ACCESS_ALLOWED_ACE_TYPE) {
            const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
            if ((ace->Mask & write_mask) && !policy_trusted_sid(const_cast<DWORD*>(&ace->SidStart))) valid = false;
        } else if (header->AceType != ACCESS_DENIED_ACE_TYPE) {
            valid = false;
        }
    }
    LocalFree(descriptor);
    return valid;
}

bool policy_protected_path(const std::filesystem::path& path, std::string_view expected_hash = {}) {
    if (!path.is_absolute() || path.native().find_first_of(L"*?\"") != std::wstring::npos) return false;
    auto current = path.root_path();
    PolicyHandle root(CreateFileW(current.c_str(), READ_CONTROL,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION root_info{};
    if (!root.valid() || !GetFileInformationByHandle(root.value, &root_info) ||
        (root_info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || !policy_protected_handle(root.value, true)) return false;
    for (const auto& component : path.relative_path()) {
        current /= component;
        const bool leaf = current == path;
        PolicyHandle handle(CreateFileW(current.c_str(), GENERIC_READ | READ_CONTROL,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        BY_HANDLE_FILE_INFORMATION info{};
        if (!handle.valid() || !GetFileInformationByHandle(handle.value, &info) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || !policy_protected_handle(handle.value, !leaf)) return false;
        if (leaf && !expected_hash.empty()) {
            if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || info.nFileSizeHigh != 0 ||
                info.nFileSizeLow == 0 || info.nFileSizeLow > 256u * 1024u) return false;
            std::vector<std::byte> bytes(info.nFileSizeLow);
            DWORD read = 0;
            if (!ReadFile(handle.value, bytes.data(), info.nFileSizeLow, &read, nullptr) || read != info.nFileSizeLow) return false;
            const auto digest = security::sha256(bytes);
            if (!digest || hex_string(*digest) != expected_hash) return false;
        }
    }
    return true;
}

bool policy_student_account(std::uint32_t session_id) {
    HANDLE raw = nullptr;
    if (!WTSQueryUserToken(session_id, &raw)) return false;
    PolicyHandle token(raw);
    DWORD size = 0;
    (void)GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
    if (size == 0 || size > 4096) return false;
    std::vector<std::byte> bytes(size);
    if (!GetTokenInformation(token.value, TokenUser, bytes.data(), size, &size)) return false;
    const auto* user = reinterpret_cast<const TOKEN_USER*>(bytes.data());
    wchar_t configured_sid[184]{};
    DWORD sid_bytes = sizeof(configured_sid);
    HKEY configuration = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NSTU\\ComputerScience", 0,
                     KEY_QUERY_VALUE | KEY_WOW64_64KEY, &configuration) != ERROR_SUCCESS) return false;
    const auto read = RegGetValueW(configuration, nullptr, L"StudentSid", RRF_RT_REG_SZ,
                                 nullptr, configured_sid, &sid_bytes);
    RegCloseKey(configuration);
    if (read != ERROR_SUCCESS) return false;
    PSID configured = nullptr;
    const bool converted = ConvertStringSidToSidW(configured_sid, &configured) != FALSE;
    const bool matches = converted && EqualSid(user->User.Sid, configured);
    if (configured) LocalFree(configured);
    TOKEN_ELEVATION elevation{};
    TOKEN_ELEVATION_TYPE type{};
    return matches && GetTokenInformation(token.value, TokenElevation, &elevation, sizeof(elevation), &size) &&
        !elevation.TokenIsElevated && GetTokenInformation(token.value, TokenElevationType, &type, sizeof(type), &size) &&
        type == TokenElevationTypeDefault && !policy_trusted_sid(user->User.Sid);
}

bool run_policy_probe(const std::filesystem::path& executable, const std::filesystem::path& script,
                      const std::filesystem::path& windows) {
    SECURITY_ATTRIBUTES security_attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE reader = nullptr, writer = nullptr;
    if (!CreatePipe(&reader, &writer, &security_attributes, 4096)) return false;
    PolicyHandle output(reader), input(writer);
    PolicyHandle null_input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      &security_attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    PolicyHandle job(CreateJobObjectW(nullptr, nullptr));
    if (!null_input.valid() || !job.valid() || !SetHandleInformation(output.value, HANDLE_FLAG_INHERIT, 0)) return false;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    if (!SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) return false;
    SIZE_T attribute_bytes = 0;
    (void)InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
    if (attribute_bytes == 0) return false;
    std::vector<std::byte> attribute_storage(attribute_bytes);
    PolicyAttributes attributes;
    const auto attribute_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
    if (!InitializeProcThreadAttributeList(attribute_list, 1, 0, &attribute_bytes)) return false;
    attributes.value = attribute_list;
    std::array<HANDLE, 2> inherited{input.value, null_input.value};
    const bool attributes_set = UpdateProcThreadAttribute(attributes.value, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        inherited.data(), sizeof(inherited), nullptr, nullptr) != FALSE;
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.StartupInfo.wShowWindow = SW_HIDE;
    startup.StartupInfo.hStdOutput = input.value;
    startup.StartupInfo.hStdError = input.value;
    startup.StartupInfo.hStdInput = null_input.value;
    startup.lpAttributeList = attributes.value;
    std::wstring command = L"\"" + executable.wstring() + L"\" -NoLogo -NoProfile -NonInteractive -File \"" + script.wstring() + L"\"";
    std::wstring environment = L"SystemRoot=" + windows.wstring();
    environment.push_back(L'\0');
    environment += L"PSModulePath=" + (executable.parent_path() / L"Modules").wstring();
    environment.push_back(L'\0');
    environment.push_back(L'\0');
    PROCESS_INFORMATION process{};
    const bool created = attributes_set && CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
        environment.data(), executable.parent_path().c_str(), &startup.StartupInfo, &process);
    if (!created) return false;
    PolicyHandle child(process.hProcess), thread(process.hThread);
    CloseHandle(input.value);
    input.value = nullptr;
    bool valid = AssignProcessToJobObject(job.value, child.value) && ResumeThread(thread.value) != static_cast<DWORD>(-1);
    std::string reply;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    DWORD wait = WAIT_TIMEOUT;
    do {
        DWORD available = 0;
        if (!PeekNamedPipe(output.value, nullptr, 0, nullptr, &available, nullptr)) {
            if (GetLastError() != ERROR_BROKEN_PIPE) valid = false;
        } else if (available) {
            if (available > 4096 - reply.size()) { valid = false; break; }
            std::array<char, 4096> buffer{};
            DWORD read = 0;
            if (!ReadFile(output.value, buffer.data(), available, &read, nullptr)) { valid = false; break; }
            reply.append(buffer.data(), read);
        }
        wait = WaitForSingleObject(child.value, 20);
        if (wait == WAIT_OBJECT_0) {
            DWORD remaining = 0;
            if (PeekNamedPipe(output.value, nullptr, 0, nullptr, &remaining, nullptr) && remaining) continue;
            break;
        }
        if (wait != WAIT_TIMEOUT || std::chrono::steady_clock::now() >= deadline) { valid = false; break; }
    } while (valid);
    DWORD exit_code = 1;
    if (!valid || wait != WAIT_OBJECT_0) {
        (void)TerminateJobObject(job.value, 1);
        (void)TerminateProcess(child.value, 1);
        (void)WaitForSingleObject(child.value, 5000);
        return false;
    }
    return GetExitCodeProcess(child.value, &exit_code) && validate_programming_policy_probe_json(reply, exit_code);
}

} // namespace

bool probe_programming_workstation_policy(std::uint32_t student_session_id) {
    try {
        BOOL wow64 = FALSE;
        if (!IsWow64Process(GetCurrentProcess(), &wow64) || wow64) return false;
        HANDLE raw = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return false;
        PolicyHandle token(raw);
        std::array<std::byte, 256> identity{};
        DWORD size = 0;
        if (!GetTokenInformation(token.value, TokenUser, identity.data(), static_cast<DWORD>(identity.size()), &size) ||
            !IsWellKnownSid(reinterpret_cast<const TOKEN_USER*>(identity.data())->User.Sid, WinLocalSystemSid) ||
            !policy_student_account(student_session_id)) return false;
        std::array<wchar_t, 32768> module{}, system{}, windows{};
        const auto module_length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
        const auto system_length = GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
        const auto windows_length = GetWindowsDirectoryW(windows.data(), static_cast<UINT>(windows.size()));
        if (module_length == 0 || module_length >= module.size() || system_length == 0 || system_length >= system.size() ||
            windows_length == 0 || windows_length >= windows.size()) return false;
        const auto root = std::filesystem::path(module.data()).parent_path() / L"cs-policy";
        const auto script = root / L"verify-cs-policy.ps1";
        const auto helper = root / L"cs-policy-functions.ps1";
        const auto executable = std::filesystem::path(system.data()) / L"WindowsPowerShell/v1.0/powershell.exe";
        if (!policy_protected_path(script, "1816f7fe314ac06be4986b8a3ba7b5c8397b6309a06b7b5ef77e7224f4e8a3d8") ||
            !policy_protected_path(helper, "562d35edf0ba67d36227062bc7c851f09cca889f71406f2a7dda54fcaadce35c") ||
            !policy_protected_path(executable)) return false;
        return run_policy_probe(executable, script, std::filesystem::path(windows.data()));
    } catch (...) { return false; }
}

bool track_browser_programming_request(
    std::map<std::string, exam::programming::Request>& pending,
    const exam::programming::Request& request) {
    using exam::programming::Operation;
    try {
        const auto wire = exam::programming::encode_request(request);
        if (wire.empty()) return false;
        const auto existing = pending.find(request.request_id);
        if (existing != pending.end()) return exam::programming::encode_request(existing->second) == wire;
        const auto context = exam::encode_state_request(request.context);
        const auto previous = std::find_if(pending.begin(), pending.end(), [&](const auto& entry) {
            const auto& saved = entry.second;
            return request.operation == Operation::draft && saved.operation == Operation::draft &&
                exam::encode_state_request(saved.context) == context && saved.problem_id == request.problem_id &&
                saved.problem_revision == request.problem_revision && saved.language == request.language;
        });
        if (previous != pending.end() && (request.draft_revision < previous->second.draft_revision ||
            (request.draft_revision == previous->second.draft_revision && request.source != previous->second.source))) return false;
        if (previous == pending.end() && pending.size() >= 128) return false;
        pending.emplace(request.request_id, request);
        if (previous != pending.end()) pending.erase(previous);
        return true;
    } catch (const std::exception&) { return false; }
}

std::optional<exam::programming::Request> parse_browser_programming_request(
    std::string_view json, std::string_view manifest_json, const exam::StateRequest& trusted_context, std::string* error) {
    using exam::programming::Operation;
    JsonValue root, manifest;
    if (!parse_json_object(json, root, error) || !validate_programming_manifest(manifest_json, error) ||
        !parse_json_object(manifest_json, manifest, error)) return std::nullopt;
    std::string type, operation;
    exam::programming::Request request;
    request.context = trusted_context;
    if (!string_member(root, "type", type) || type != "exam_programming_request" ||
        !string_member(root, "operation", operation) || !string_member(root, "requestId", request.request_id)) return std::nullopt;
    bool known = false;
    for (const auto op : {Operation::draft, Operation::submit, Operation::run, Operation::cancel,
                          Operation::recover, Operation::finalize, Operation::ready}) {
        if (exam::programming::operation_name(op) == operation) { request.operation = op; known = true; break; }
    }
    if (!known) return std::nullopt;
    const std::set<std::string> identity_fields{"packageId", "packageDigestHex", "clientIdHex", "sessionIdHex", "candidateId"};
    auto allowed = identity_fields;
    allowed.insert({"type", "context", "programmingVersion", "operation", "requestId"});
    std::uint64_t version = 1;
    if (!uint64_member(root, "programmingVersion", version, false) || version != 1) return std::nullopt;
    const auto check_context = [&](const JsonValue& value) {
        std::string package, candidate, digest, client, session;
        exam::StateRequest claimed;
        return string_member(value, "packageId", package) && string_member(value, "candidateId", candidate) &&
            package == trusted_context.package_id && candidate == trusted_context.candidate_id &&
            string_member(value, "packageDigestHex", digest) && parse_hex(digest, claimed.package_digest) &&
            claimed.package_digest == trusted_context.package_digest &&
            string_member(value, "clientIdHex", client) && parse_hex(client, claimed.client_id) && claimed.client_id == trusted_context.client_id &&
            string_member(value, "sessionIdHex", session) && parse_hex(session, claimed.session_id) && claimed.session_id == trusted_context.session_id;
    };
    const auto* nested = member(root, "context");
    const bool flat = std::any_of(identity_fields.begin(), identity_fields.end(), [&](const auto& field) { return member(root, field) != nullptr; });
    if ((!nested && !flat) || (nested && (!public_fields(*nested, identity_fields) || !check_context(*nested))) ||
        (flat && !check_context(root))) return std::nullopt;
    if (request.operation != Operation::ready && request.operation != Operation::finalize) {
        allowed.insert({"problemId", "problemRevision", "language"});
        std::uint64_t revision = 0;
        if (!string_member(root, "problemId", request.problem_id) || !string_member(root, "language", request.language) ||
            !uint64_member(root, "problemRevision", revision) || revision == 0 || revision > UINT32_MAX) return std::nullopt;
        request.problem_revision = static_cast<std::uint32_t>(revision);
        const auto* questions = member(manifest, "questions");
        const auto question = std::find_if(questions->array.begin(), questions->array.end(), [&](const auto& value) {
            std::string id;
            std::uint64_t expected_revision = 0;
            return string_member(value, "id", id) && id == request.problem_id &&
                uint64_member(value, "revision", expected_revision) && expected_revision == revision;
        });
        if (question == questions->array.end()) return std::nullopt;
        if (request.operation == Operation::run) {
            allowed.insert("sampleId");
            if (!string_member(root, "sampleId", request.sample_id)) return std::nullopt;
            const auto* samples = member(*question, "samples");
            if (std::none_of(samples->array.begin(), samples->array.end(), [&](const auto& sample) {
                    std::string id;
                    return string_member(sample, "id", id) && id == request.sample_id;
                })) return std::nullopt;
        }
    }
    if (request.operation == Operation::draft || request.operation == Operation::submit || request.operation == Operation::run) {
        allowed.insert({"source", "draftRevision"});
        if (!string_member(root, "source", request.source) || !uint64_member(root, "draftRevision", request.draft_revision)) return std::nullopt;
    }
    if (request.operation == Operation::cancel) {
        allowed.insert("targetRunId");
        if (!string_member(root, "targetRunId", request.target_run_id)) return std::nullopt;
    }
    if (request.operation == Operation::finalize) {
        allowed.insert({"receiptIds", "noSourceProblemIds"});
        for (const auto& field : {std::pair{"receiptIds", &request.receipt_ids}, std::pair{"noSourceProblemIds", &request.no_source_problem_ids}}) {
            const auto* array = member(root, field.first);
            if (!array || array->kind != JsonValue::Kind::array || array->array.size() > exam::programming::kMaximumInventoryEntries) return std::nullopt;
            for (const auto& entry : array->array) {
                if (entry.kind != JsonValue::Kind::string) return std::nullopt;
                field.second->push_back(entry.scalar);
            }
        }
    }
    if (!public_fields(root, allowed) || !exam::programming::validate_request(request)) return std::nullopt;
    return request;
}

namespace {
bool same_capture_context(const exam::StateRequest& a, const exam::StateRequest& b) noexcept {
    return a.package_id == b.package_id && a.package_digest == b.package_digest &&
        a.client_id == b.client_id && a.session_id == b.session_id && a.candidate_id == b.candidate_id;
}

bool capture_hex(std::string_view value, std::size_t size) noexcept {
    return value.size() == size && std::all_of(value.begin(), value.end(), [](char byte) {
        return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
    });
}

bool capture_id(std::string_view value) {
    // Match the shared JS safeId exclusion of inherited Object.prototype names.
    static const std::set<std::string> reserved{"__proto__", "constructor",
        "__defineGetter__", "__defineSetter__", "__lookupGetter__", "__lookupSetter__",
        "hasOwnProperty", "isPrototypeOf", "propertyIsEnumerable", "toLocaleString", "toString", "valueOf"};
    return programming_id(value) && !reserved.contains(std::string(value));
}

std::string capture_message(std::string_view type, const exam::StateRequest& context, std::string_view nonce) {
    return "{\"type\":\"" + std::string(type) + "\",\"context\":" +
        exam::programming::context_json(context) + ",\"nonce\":\"" + std::string(nonce) + "\"}";
}
} // namespace

bool ExamHostCloseCaptureState::start(exam::StateRequest context, std::string_view manifest_json,
                                      std::uint64_t generation) {
    if (!nonce_.empty() || !cancellation_nonce_.empty() || generation == 0 ||
        (generation_ != 0 && generation <= generation_) || exam::encode_state_request(context).empty()) return false;
    JsonValue manifest;
    std::string package, mode;
    if (!validate_programming_manifest(manifest_json) || !parse_json_object(manifest_json, manifest, nullptr) ||
        !string_member(manifest, "id", package) || package != context.package_id ||
        !string_member(manifest, "sessionMode", mode) || mode != "contest") return false;
    std::map<std::string, std::uint32_t> problems;
    for (const auto& question : member(manifest, "questions")->array) {
        std::string id; std::uint64_t revision = 0;
        if (!string_member(question, "id", id) || !capture_id(id) ||
            !uint64_member(question, "revision", revision)) return false;
        problems.emplace(std::move(id), static_cast<std::uint32_t>(revision));
    }
    context_ = std::move(context); problems_ = std::move(problems); generation_ = generation;
    report_.reset(); issued_nonces_.clear();
    return true;
}

void ExamHostCloseCaptureState::stop() noexcept {
    nonce_.clear(); cancellation_nonce_.clear(); report_.reset(); problems_.clear();
    generation_ = 0; context_ = {}; issued_nonces_.clear();
}

bool ExamHostCloseCaptureState::prepare(const std::function<bool(std::string_view)>& post, Clock::time_point now) {
    if (generation_ == 0 || !nonce_.empty() || !cancellation_nonce_.empty() || issued_nonces_.size() >= 256 || !post) return false;
    security::Sha256Digest random{};
    if (!security::generate_random(random)) return false;
    const auto nonce = hex_string(random);
    if (!issued_nonces_.insert(nonce).second) return false;
    nonce_ = nonce; report_.reset(); deadline_ = now + std::chrono::seconds(5);
    const auto json = capture_message("exam_close_capture_request", context_, nonce_);
    // A throwing/failed post may have delivered. Keep the nonce for explicit cancellation.
    try { return post(json); } catch (...) { return false; }
}

bool ExamHostCloseCaptureState::cancel(const std::function<bool(std::string_view)>& post) {
    if (cancelling_) return false;
    invalidate();
    if (cancellation_nonce_.empty() || !post) return false;
    const auto nonce = cancellation_nonce_;
    const auto json = capture_message("exam_close_capture_cancel", context_, nonce);
    cancelling_ = true;
    bool posted = false;
    try { posted = post(json); } catch (...) {}
    cancelling_ = false;
    if (posted && cancellation_nonce_ == nonce) cancellation_nonce_.clear();
    return posted;
}

void ExamHostCloseCaptureState::invalidate() noexcept {
    if (!nonce_.empty()) {
        cancellation_nonce_.swap(nonce_);
        nonce_.clear();
    }
    report_.reset();
}

bool ExamHostCloseCaptureState::expired(Clock::time_point now) const noexcept {
    return !nonce_.empty() && !report_ && now >= deadline_;
}

bool ExamHostCloseCaptureState::accept(std::string_view json, Clock::time_point now) try {
    if (nonce_.empty() || expired(now) || json.size() > 64u * 1024u ||
        (report_ && !report_->complete)) return false;
    JsonValue root;
    if (!parse_json_object(json, root, nullptr)) return false;
    std::string type, nonce;
    if (!string_member(root, "type", type) ||
        (type != "exam_close_capture_complete" && type != "exam_close_capture_failed") ||
        !string_member(root, "nonce", nonce) || !capture_hex(nonce, 64) || nonce != nonce_) return false;
    const bool complete = type == "exam_close_capture_complete";
    if (complete && report_) return false;
    auto fields = std::set<std::string>{"type", "context", "nonce", "localSaved", "documents"};
    if (!complete) fields.insert("reason");
    const auto* identity = member(root, "context");
    std::string package, candidate, digest, client, session;
    if (!public_fields(root, fields) || root.object.size() != fields.size() || !identity ||
        !public_fields(*identity, {"packageId", "packageDigestHex", "clientIdHex", "sessionIdHex", "candidateId"}) ||
        identity->object.size() != 5 || !string_member(*identity, "packageId", package) || package != context_.package_id ||
        !string_member(*identity, "candidateId", candidate) || candidate != context_.candidate_id ||
        !string_member(*identity, "packageDigestHex", digest) || digest != hex_string(context_.package_digest) ||
        !string_member(*identity, "clientIdHex", client) || client != hex_string(context_.client_id) ||
        !string_member(*identity, "sessionIdHex", session) || session != hex_string(context_.session_id)) return false;
    const auto* saved = member(root, "localSaved");
    const auto* documents = member(root, "documents");
    if (!saved || saved->kind != JsonValue::Kind::boolean || (complete && !saved->boolean) ||
        !documents || documents->kind != JsonValue::Kind::array || documents->array.size() > 128) return false;
    ExamCloseCaptureReport report{context_, generation_, nonce_, complete, saved->boolean, {}, {}};
    if (!complete && (!string_member(root, "reason", report.reason) ||
        (report.reason != "composition_pending" && report.reason != "local_storage_unavailable" &&
         report.reason != "document_unavailable" && report.reason != "native_enqueue_unavailable"))) return false;
    std::set<std::pair<std::string, std::string>> identities;
    std::set<std::string> requests;
    for (const auto& entry : documents->array) {
        ExamCloseCaptureDocument document;
        std::uint64_t revision = 0;
        if (!public_fields(entry, {"problemId", "problemRevision", "language", "draftRevision", "sourceDigest", "requestId", "status"}) ||
            entry.object.size() != 7 || !string_member(entry, "problemId", document.problem_id) || !capture_id(document.problem_id) ||
            !uint64_member(entry, "problemRevision", revision) || revision == 0 || revision > UINT32_MAX ||
            !problems_.contains(document.problem_id) || problems_.at(document.problem_id) != revision ||
            !string_member(entry, "language", document.language) || (document.language != "cpp20" && document.language != "python3") ||
            !uint64_member(entry, "draftRevision", document.draft_revision) || document.draft_revision == 0 ||
            document.draft_revision > exam::programming::kMaximumDraftRevision ||
            !string_member(entry, "sourceDigest", document.source_digest) || !capture_hex(document.source_digest, 64) ||
            !string_member(entry, "requestId", document.request_id) || !capture_id(document.request_id) ||
            !string_member(entry, "status", document.status) ||
            (document.status != "forwarded" && document.status != "received" && document.status != "queued") ||
            (complete && document.status == "queued") || !identities.emplace(document.problem_id, document.language).second ||
            !requests.insert(document.request_id).second) return false;
        document.problem_revision = static_cast<std::uint32_t>(revision);
        report.documents.push_back(std::move(document));
    }
    report_ = std::move(report);
    return true;
} catch (...) {
    invalidate();
    return false;
}

bool ExamHostCloseCaptureState::is_current_complete(const ExamCloseCaptureReport& report) const noexcept {
    return report_ && report_->complete && report.complete && report.local_saved && report.reason.empty() &&
        !nonce_.empty() && report.nonce == nonce_ && report.generation == generation_ &&
        same_capture_context(report.context, context_) && report.documents == report_->documents;
}

bool ExamHostCloseCaptureState::preserve(const std::function<bool(const ExamCloseCaptureReport&)>& predicate) {
    if (preserving_ || !report_ || !is_current_complete(*report_) || !predicate) return false;
    const auto captured = *report_;
    preserving_ = true;
    bool preserved = false;
    try { preserved = predicate(captured); } catch (...) {}
    preserving_ = false;
    return preserved && is_current_complete(captured);
}

bool ExamHostContestLifecycleState::accept(const ExamHostContestLifecycle& value) {
    constexpr std::uint64_t maximum = 9007199254740991ULL;
    const bool active_clock = value.server_start != 0 || value.deadline != 0;
    if (value.revision == 0 || value.revision > maximum || value.server_time == 0 || value.server_time > maximum ||
        value.server_start > maximum || value.deadline > maximum ||
        value.phase < ExamHostContestPhase::draft || value.phase > ExamHostContestPhase::closed ||
        (active_clock && (value.server_start == 0 || value.deadline <= value.server_start ||
            value.server_time < value.server_start || value.deadline - value.server_start < 60000 ||
            value.deadline - value.server_start > 86400000)) ||
        (value.phase == ExamHostContestPhase::running && !active_clock) ||
        (value.phase < ExamHostContestPhase::running && active_clock)) return false;
    exam::ExamReadyReport identity{value.context.package_id, value.context.candidate_id, value.context.package_digest,
                                  value.context.client_id, value.context.session_id};
    if (!exam::validate_exam_ready_report(identity)) return false;
    if (current_) {
        const auto& previous = *current_;
        if (!same_capture_context(previous.context, value.context) || value.revision < previous.revision ||
            value.server_time < previous.server_time ||
            (value.revision == previous.revision && value.phase != previous.phase) ||
            (previous.phase == ExamHostContestPhase::closed && value.phase != ExamHostContestPhase::closed) ||
            (previous.phase == ExamHostContestPhase::running && value.phase < ExamHostContestPhase::running) ||
            ((previous.server_start != 0 || previous.phase == ExamHostContestPhase::closed) &&
                (value.server_start != previous.server_start || value.deadline != previous.deadline))) return false;
    }
    std::optional<ExamHostContestLifecycle> next{value};
    current_.swap(next);
    ++publication_;
    return true;
}

std::string ExamHostContestLifecycleState::json() const {
    if (!current_) return {};
    const auto& value = *current_;
    const char* phase = "Draft";
    switch (value.phase) {
    case ExamHostContestPhase::draft: break;
    case ExamHostContestPhase::joining: phase = "Joining"; break;
    case ExamHostContestPhase::prepared: phase = "Prepared"; break;
    case ExamHostContestPhase::running: phase = "Running"; break;
    case ExamHostContestPhase::closed: phase = "Closed"; break;
    }
    return "{\"type\":\"exam_contest_lifecycle\",\"context\":" + exam::programming::context_json(value.context) +
        ",\"phase\":\"" + phase + "\",\"revision\":" + std::to_string(value.revision) +
        ",\"serverStart\":" + std::to_string(value.server_start) + ",\"deadline\":" + std::to_string(value.deadline) +
        ",\"serverTime\":" + std::to_string(value.server_time) + "}";
}

bool ExamHostContestLifecycleState::publish(const std::function<bool(std::string_view)>& post) const {
    if (!current_ || !post) return false;
    const auto publication = publication_;
    const auto message = json();
    return post(message) && publication_ == publication;
}

bool validate_exam_user_data_path(
    const std::filesystem::path& allowed_user_data_root,
    const std::filesystem::path& user_data_root,
    std::string* error) {
    if (allowed_user_data_root.empty() ||
        !allowed_user_data_root.is_absolute()) {
        set_error(error, "exam user-data policy root must be absolute");
        return false;
    }
    if (user_data_root.empty() || !user_data_root.is_absolute()) {
        set_error(error, "exam user-data root must be an absolute path");
        return false;
    }
    if (path_has_reparse_point(allowed_user_data_root) ||
        path_has_reparse_point(user_data_root)) {
        set_error(error, "exam user-data path is reparse-backed");
        return false;
    }

    std::error_code fs_error;
    const auto policy_root = std::filesystem::weakly_canonical(
        allowed_user_data_root, fs_error);
    if (fs_error || policy_root.empty() || path_has_reparse_point(policy_root)) {
        set_error(error, "exam user-data policy root is invalid");
        return false;
    }
    const auto user_data =
        std::filesystem::weakly_canonical(user_data_root, fs_error);
    if (fs_error || user_data.empty() || user_data == policy_root ||
        !path_is_within(policy_root, user_data)) {
        set_error(error, "exam user-data root is outside its policy root");
        return false;
    }
    std::error_code status_error;
    const auto status = std::filesystem::status(user_data, status_error);
    if (!status_error && std::filesystem::exists(status) &&
        !std::filesystem::is_directory(status)) {
        set_error(error, "exam user-data root is not a directory");
        return false;
    }
    if (!status_error && path_has_reparse_point(user_data)) {
        set_error(error, "exam user-data root is reparse-backed");
        return false;
    }
    return true;
}

bool validate_exam_path_policy(
    const std::filesystem::path& allowed_data_root,
    const std::filesystem::path& package_root,
    const std::filesystem::path& web_root,
    const std::filesystem::path& user_data_root,
    std::string* error) {
    if (allowed_data_root.empty() || !allowed_data_root.is_absolute()) {
        set_error(error, "NSTU data root must be an absolute path");
        return false;
    }
    if (path_has_reparse_point(allowed_data_root) ||
        path_has_reparse_point(package_root) ||
        (!web_root.empty() && path_has_reparse_point(web_root))) {
        set_error(error, "NSTU exam path is reparse-backed");
        return false;
    }

    std::error_code fs_error;
    const auto data_root =
        std::filesystem::weakly_canonical(allowed_data_root, fs_error);
    if (fs_error || !std::filesystem::is_directory(data_root) ||
        path_has_reparse_point(data_root)) {
        set_error(error, "NSTU data root is unavailable or reparse-backed");
        return false;
    }

    const auto package_area =
        std::filesystem::weakly_canonical(data_root / L"exams", fs_error);
    if (fs_error || !std::filesystem::is_directory(package_area) ||
        path_has_reparse_point(package_area)) {
        set_error(error, "NSTU exam package directory is unavailable");
        return false;
    }

    if (package_root.empty() || !package_root.is_absolute()) {
        set_error(error, "exam package root must be an absolute path");
        return false;
    }
    const auto package =
        std::filesystem::weakly_canonical(package_root, fs_error);
    if (fs_error || !std::filesystem::is_directory(package) ||
        path_has_reparse_point(package) || package == package_area ||
        !path_is_within(package_area, package)) {
        set_error(error, "exam package root is outside the NSTU data root");
        return false;
    }

    const auto effective_web_root =
        web_root.empty() ? package / L"exam" / L"web" : web_root;
    if (!effective_web_root.is_absolute()) {
        set_error(error, "exam web root must be an absolute path");
        return false;
    }
    const auto web =
        std::filesystem::weakly_canonical(effective_web_root, fs_error);
    if (fs_error || !std::filesystem::is_directory(web) ||
        path_has_reparse_point(web) || web == package ||
        !path_is_within(package, web)) {
        set_error(error, "exam web root is outside the package");
        return false;
    }

    if (!user_data_root.empty() &&
        !validate_exam_user_data_path(data_root / L"exam-user-data",
                                      user_data_root, error)) {
        return false;
    }
    return true;
}

bool extract_exam_lockdown_policy(
    std::string_view manifest_json, ExamLockdownPolicy& out,
    std::string* error) {
    out.allowed_origins.clear();
    out.duration_seconds = 0;
    out.programming_mode.clear();

    JsonValue manifest;
    if (!parse_json_object(manifest_json, manifest, error)) {
        return false;
    }
    std::uint64_t duration = 0;
    if (!uint64_member(manifest, "durationSeconds", duration) ||
        duration < 60 || duration > 86400) {
        set_error(error, "manifest durationSeconds is invalid or out of bounds");
        return false;
    }
    out.duration_seconds = static_cast<std::uint32_t>(duration);
    std::string content_mode;
    if (string_member(manifest, "contentMode", content_mode) && content_mode == "computer_science") {
        if (!validate_programming_manifest(manifest_json, error) ||
            !string_member(manifest, "sessionMode", out.programming_mode)) return false;
    }

    const auto* allowed_origins = member(manifest, "allowedOrigins");
    if (allowed_origins == nullptr) {
        return true;
    }

    if (allowed_origins->kind != JsonValue::Kind::array ||
        allowed_origins->array.size() > 32) {
        set_error(error, "manifest allowedOrigins is not an array or exceeds 32 items");
        return false;
    }

    std::set<std::string> seen;
    for (const auto& item : allowed_origins->array) {
        if (item.kind != JsonValue::Kind::string || item.scalar.empty() ||
            item.scalar.size() > 253) {
            set_error(error, "manifest allowed origin entry is invalid");
            return false;
        }
        if (!seen.insert(item.scalar).second) {
            set_error(error, "manifest allowedOrigins contains duplicate entries");
            return false;
        }
        std::string host;
        if (!parse_origin_hostname(item.scalar, host)) {
            set_error(error, "manifest allowed origin entry format is invalid");
            return false;
        }
        out.allowed_origins.push_back(item.scalar);
    }

    return true;
}

bool inspect_exam_package(const std::filesystem::path& package_root,
                          const std::filesystem::path& web_root,
                          std::string_view expected_digest_hex,
                          bool require_digest, ExamPackageReport& report,
                          std::string* error) {
    if (package_root.empty() || !package_root.is_absolute()) {
        set_error(error, "exam package root must be an absolute path");
        return false;
    }
    if (path_has_reparse_point(package_root) ||
        (!web_root.empty() && path_has_reparse_point(web_root))) {
        set_error(error, "exam package path is reparse-backed");
        return false;
    }
    std::error_code fs_error;
    const auto root = std::filesystem::weakly_canonical(package_root, fs_error);
    if (fs_error || !std::filesystem::is_directory(root) ||
        path_has_reparse_point(root)) {
        set_error(error, "exam package root is unavailable or reparse-backed");
        return false;
    }
    const auto effective_web_root = web_root.empty() ? root / L"exam" / L"web"
                                                      : web_root;
    if (!effective_web_root.is_absolute() ||
        !path_is_within(root, effective_web_root)) {
        set_error(error, "exam web root is outside the package");
        return false;
    }
    const auto canonical_web_root =
        std::filesystem::weakly_canonical(effective_web_root, fs_error);
    if (fs_error || canonical_web_root == root ||
        !std::filesystem::is_directory(canonical_web_root) ||
        path_has_reparse_point(canonical_web_root)) {
        set_error(error, "exam web root is outside the package");
        return false;
    }
    const auto manifest_path = root / L"manifest.json";
    const auto page_path = effective_web_root / L"index.html";
    std::vector<std::byte> page_bytes;
    if (!read_file_bounded(page_path, kMaximumManifestBytes, page_bytes, error)) {
        set_error(error, "exam web entry point is unavailable");
        return false;
    }
    std::vector<std::byte> manifest_data;
    if (!read_file_bounded(manifest_path, kMaximumManifestBytes, manifest_data,
                           error)) {
        return false;
    }
    report.manifest_json.assign(reinterpret_cast<const char*>(manifest_data.data()),
                                manifest_data.size());
    JsonValue package_manifest;
    std::string content_mode;
    const bool programming_package = parse_json_object(report.manifest_json, package_manifest, error) &&
        string_member(package_manifest, "contentMode", content_mode, false) && content_mode == "computer_science";
    if (programming_package && !validate_programming_manifest(report.manifest_json, error)) return false;
    if (!validate_manifest_asset_files(root, report.manifest_json, error)) {
        return false;
    }

    struct PackageFile {
        std::string relative;
        std::filesystem::path path;
        std::uintmax_t size = 0;
    };
    std::vector<PackageFile> files;
    std::uintmax_t total_bytes = 0;
    std::filesystem::recursive_directory_iterator iterator(
        root, std::filesystem::directory_options::none, fs_error);
    const std::filesystem::recursive_directory_iterator end;
    for (; !fs_error && iterator != end; iterator.increment(fs_error)) {
        const auto current = iterator->path();
        if (path_has_reparse_point(current)) {
            set_error(error, "exam package contains a reparse point");
            return false;
        }
        if (!iterator->is_regular_file(fs_error)) {
            if (fs_error) break;
            continue;
        }
        const auto relative = std::filesystem::relative(current, root, fs_error);
        if (fs_error || relative.empty() ||
            relative.generic_string().find("..") == 0) {
            set_error(error, "exam package contains an invalid relative path");
            return false;
        }
        if (programming_package && !kProgrammingPackageFiles.contains(relative.generic_string())) {
            set_error(error, "CS package contains an unknown or private file");
            return false;
        }
        const auto size = iterator->file_size(fs_error);
        if (fs_error || size > kMaximumPackageBytes ||
            total_bytes > kMaximumPackageBytes - size ||
            files.size() >= kMaximumPackageFiles) {
            set_error(error, "exam package exceeds its bounded file quota");
            return false;
        }
        total_bytes += size;
        files.push_back({relative.generic_string(), current, size});
    }
    if (fs_error || files.empty()) {
        set_error(error, "exam package enumeration failed");
        return false;
    }
    std::sort(files.begin(), files.end(),
              [](const auto& left, const auto& right) {
                  return left.relative < right.relative;
              });
    std::vector<std::byte> canonical;
    canonical.reserve(static_cast<std::size_t>(total_bytes) + files.size() * 32);
    auto append_u64 = [&canonical](std::uint64_t value) {
        for (unsigned index = 0; index < 8; ++index) {
            canonical.push_back(static_cast<std::byte>(value & 0xffu));
            value >>= 8u;
        }
    };
    for (const auto& file : files) {
        std::vector<std::byte> bytes;
        if (!read_file_bounded(file.path, kMaximumPackageBytes, bytes, error)) {
            return false;
        }
        append_u64(file.relative.size());
        canonical.insert(canonical.end(),
                         reinterpret_cast<const std::byte*>(file.relative.data()),
                         reinterpret_cast<const std::byte*>(file.relative.data()) +
                             file.relative.size());
        append_u64(bytes.size());
        canonical.insert(canonical.end(), bytes.begin(), bytes.end());
    }
    const auto digest = security::sha256(canonical);
    if (!digest) {
        set_error(error, "exam package digest calculation failed");
        return false;
    }
    report.digest_hex = hex_string(*digest);
    if (require_digest && expected_digest_hex.empty()) {
        set_error(error, "exam package digest pin is required");
        return false;
    }
    if (!expected_digest_hex.empty()) {
        security::Sha256Digest expected{};
        if (!parse_hex(expected_digest_hex, expected) ||
            !security::constant_time_equal(expected, *digest)) {
            set_error(error, "exam package digest does not match its pin");
            return false;
        }
    }
    report.package_root = root;
    report.web_root = canonical_web_root;
    report.page_path = std::filesystem::weakly_canonical(page_path);
    return true;
}

struct ExamHost::Impl {
    ExamHost* api = nullptr;
    HWND owner = nullptr;
    HWND window = nullptr;
    ExamHostOptions options;
    ExamHostWindowPolicy window_policy;
    ExamPackageReport package;
    ContextIdentity context;
    ExamHostCallbacks callbacks;
    ExamHostState state = ExamHostState::idle;
    HHOOK keyboard_hook = nullptr;
    bool stopping = false;
    bool close_pending = false;
    bool editable_document_created = false;
    std::uint64_t document_generation = 0;
    ExamHostCloseCaptureState close_capture;
    ExamHostContestLifecycleState contest_lifecycle;
    bool contest_lifecycle_pending = false;
    bool preserve_bridge_enable = false;
    std::uint64_t command_publication = 0;
    std::set<std::string> allowed_package_assets;
    std::set<std::string> valid_question_ids;
    bool ready_reported = false;
    bool programming_package = false;
    std::uint32_t contest_duration_seconds = 0;
    std::map<std::string, exam::programming::Request> programming_pending;
    std::uint64_t programming_pending_revision = 0;
    // Incremented whenever an exam host operation is stopped/restarted.  The
    // WebView2 API completes asynchronously, so callbacks from an older
    // operation must never mutate a newly-started host.
    std::uint64_t operation_generation = 0;

    ExamHostTelemetryState telemetry;

#if NSTU_HAS_WEBVIEW2
    HMODULE loader = nullptr;
    Microsoft::WRL::ComPtr<ICoreWebView2Environment> environment;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller;
    Microsoft::WRL::ComPtr<ICoreWebView2> webview;
    Microsoft::WRL::ComPtr<ICoreWebView2NavigationStartingEventHandler>
        navigation_handler;
    Microsoft::WRL::ComPtr<ICoreWebView2NewWindowRequestedEventHandler>
        new_window_handler;
    Microsoft::WRL::ComPtr<ICoreWebView2NavigationCompletedEventHandler>
        navigation_completed_handler;
    Microsoft::WRL::ComPtr<ICoreWebView2WebMessageReceivedEventHandler>
        message_handler;
    Microsoft::WRL::ComPtr<ICoreWebView2ProcessFailedEventHandler>
        process_failed_handler;
    EventRegistrationToken navigation_token{};
    EventRegistrationToken new_window_token{};
    EventRegistrationToken navigation_completed_token{};
    EventRegistrationToken message_token{};
    EventRegistrationToken process_failed_token{};
    Microsoft::WRL::ComPtr<ICoreWebView2WebResourceRequestedEventHandler>
        web_resource_handler;
    EventRegistrationToken web_resource_token{};
    bool web_resource_filter_added = false;
    bool webview_ready = false;
    UINT_PTR initialization_timer_id = 0;
    std::uint64_t initialization_generation = 0;
    ULONGLONG initialization_deadline_ms = 0;
    ULONGLONG last_blocked_resource_notice_ms = 0;
#endif
};

namespace {

ExamHost::Impl* g_keyboard_host = nullptr;
ExamShortcutCounter g_test_blocked_shortcuts;

[[maybe_unused]] bool callback_is_current(const ExamHost::Impl& host,
                                          std::uint64_t generation) noexcept {
    return !host.stopping && host.state != ExamHostState::failed &&
           host.operation_generation == generation;
}

void notify_status(ExamHost::Impl& host, std::string message) {
    if (host.callbacks.status) {
        try {
            host.callbacks.status(std::move(message));
        } catch (...) {
            // Status reporting is an observer callback. It must never unwind
            // through a Win32 message loop or a WebView2 COM ABI boundary.
            OutputDebugStringA("NSTU exam status callback threw an exception.\n");
        }
    }
}

void notify_capture(ExamHost::Impl& host) noexcept {
    try {
        const auto callback = host.callbacks.capture_updated;
        if (callback) callback(host.close_capture.report());
    } catch (...) { notify_status(host, "Close capture notification failed; the editor remains open."); }
}

void cancel_initialization_watchdog(ExamHost::Impl& host) noexcept {
#if NSTU_HAS_WEBVIEW2
    if (host.window != nullptr && host.initialization_timer_id != 0) {
        (void)KillTimer(host.window, host.initialization_timer_id);
    }
    host.initialization_timer_id = 0;
    host.initialization_generation = 0;
    host.initialization_deadline_ms = 0;
#else
    (void)host;
#endif
}

[[maybe_unused]] bool begin_initialization_watchdog(
    ExamHost::Impl& host, std::uint64_t generation) noexcept {
#if NSTU_HAS_WEBVIEW2
    if (host.window == nullptr || generation == 0) return false;
    cancel_initialization_watchdog(host);
    // Keep the id in a private range and derive it from the operation. A
    // queued WM_TIMER from an older operation can therefore be rejected by
    // both its id and the generation check in the window procedure.
    const auto id = static_cast<UINT_PTR>(
        kInitializationTimerBase | (generation & 0x0fffu));
    if (id == 0 || SetTimer(host.window, id, kInitializationTimerPeriodMs,
                            nullptr) == 0) {
        return false;
    }
    host.initialization_timer_id = id;
    host.initialization_generation = generation;
    host.initialization_deadline_ms = GetTickCount64() +
                                      kInitializationTimeoutMs;
    return true;
#else
    (void)host;
    (void)generation;
    return true;
#endif
}

void set_host_state(ExamHost::Impl& host, ExamHostState state,
                    std::string message = {}) {
    const bool entering_failed = state == ExamHostState::failed &&
                                 host.state != ExamHostState::failed;
    host.state = state;
    if (state == ExamHostState::failed) {
        host.telemetry.stop();
        if (!host.window_policy.managed_lockdown && host.api) (void)host.api->cancel_capture();
#if NSTU_HAS_WEBVIEW2
        host.webview_ready = false;
#endif
    }
    if (state == ExamHostState::running || state == ExamHostState::failed) {
        cancel_initialization_watchdog(host);
    }
    if (!message.empty()) notify_status(host, std::move(message));
    if (entering_failed && host.window != nullptr && !host.stopping) {
        // The normal agent callback posts a status message to its owner.  A
        // standalone caller may not have an owner, so also queue a host-local
        // cleanup request.  The window procedure only honors this internal
        // form while the host is still failed, preventing stale messages from
        // stopping a later exam operation.
        (void)PostMessageW(host.window, kHostMessageExamStop, 1, 0);
    }
}

LRESULT CALLBACK exam_keyboard_proc(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION && g_keyboard_host != nullptr &&
        (wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN)) {
        const auto* key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam);
        const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0 ||
                         (key->flags & LLKHF_ALTDOWN) != 0;
        const bool control = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool recovery = alt && control && shift && key->vkCode == VK_F12;
        if (recovery) {
            // WM_CLOSE is intentionally ignored by the kiosk window so an
            // accidental close request cannot end an active exam. The
            // technician recovery chord uses the explicit UI stop message.
            PostMessageW(g_keyboard_host->window, kHostMessageExamStop, 0, 0);
            return 1;
        }
        const bool blocked = key->vkCode == VK_LWIN || key->vkCode == VK_RWIN ||
                             (alt && (key->vkCode == VK_TAB ||
                                      key->vkCode == VK_ESCAPE ||
                                      key->vkCode == VK_F4)) ||
                             (control && key->vkCode == VK_ESCAPE);
        if (blocked) {
            g_keyboard_host->telemetry.record_blocked_shortcuts();
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

void resize_exam_window(ExamHost::Impl& host) {
    if (host.window == nullptr) return;
    if (!host.window_policy.managed_lockdown) {
#if NSTU_HAS_WEBVIEW2
        if (host.controller != nullptr) {
            RECT bounds{};
            GetClientRect(host.window, &bounds);
            (void)host.controller->put_Bounds(bounds);
            (void)host.controller->NotifyParentWindowPositionChanged();
        }
#endif
        return;
    }
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN));
    const int height = std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));
    SetWindowPos(host.window, HWND_TOPMOST, x, y, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
#if NSTU_HAS_WEBVIEW2
    if (host.controller != nullptr) {
        RECT bounds{0, 0, width, height};
        (void)host.controller->put_Bounds(bounds);
        (void)host.controller->NotifyParentWindowPositionChanged();
    }
#endif
}

void enable_per_monitor_dpi_awareness() noexcept {
    using SetDpiAwarenessContextFn = DPI_AWARENESS_CONTEXT(WINAPI*) (
        DPI_AWARENESS_CONTEXT);
    const auto user32 = GetModuleHandleW(L"user32.dll");
    if (user32 == nullptr) return;
    const auto set_context =
        load_function<SetDpiAwarenessContextFn>(
            user32, "SetThreadDpiAwarenessContext");
    if (set_context != nullptr) {
        (void)set_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
}

LRESULT CALLBACK exam_window_proc(HWND window, UINT message, WPARAM wparam,
                                  LPARAM lparam) {
    auto* host = reinterpret_cast<ExamHost::Impl*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        host = static_cast<ExamHost::Impl*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(host));
        if (host != nullptr) host->window = window;
    }
    if (host == nullptr) return DefWindowProcW(window, message, wparam, lparam);
    switch (message) {
    case WM_TIMER:
        if (wparam == kBridgeTimerId) {
            if (host->api != nullptr) {
                host->api->drain_bridge();
                host->api->enforce_foreground();
            }
            return 0;
        }
        if (wparam == kExamAliveTimerId) {
            if (host->window_policy.managed_lockdown &&
                (host->state == ExamHostState::preparing ||
                host->state == ExamHostState::initializing ||
                host->state == ExamHostState::running)) {
                if (host->callbacks.send_to_service) {
                    host->callbacks.send_to_service(
                        {AgentMessageType::exam_alive, {}});
                }
            }
            return 0;
        }
#if NSTU_HAS_WEBVIEW2
        if (wparam == host->initialization_timer_id) {
            if (host->initialization_generation == host->operation_generation &&
                host->state == ExamHostState::initializing &&
                host->initialization_deadline_ms != 0 &&
                GetTickCount64() >= host->initialization_deadline_ms) {
                set_host_state(
                    *host, ExamHostState::failed,
                    "WebView2 initialization timed out; the exam host was stopped.");
            }
            return 0;
        }
#endif
        break;
    case WM_DISPLAYCHANGE:
        if (!host->window_policy.managed_lockdown) break;
        resize_exam_window(*host);
        return 0;
    case WM_DPICHANGED:
        if (!host->window_policy.managed_lockdown) {
            const auto* suggested = reinterpret_cast<const RECT*>(lparam);
            if (suggested != nullptr) {
                SetWindowPos(window, nullptr, suggested->left, suggested->top,
                    suggested->right - suggested->left, suggested->bottom - suggested->top,
                    SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
        resize_exam_window(*host);
        return 0;
    case WM_SIZE:
        if (!host->window_policy.managed_lockdown) resize_exam_window(*host);
        break;
    case WM_ACTIVATE:
    case WM_KILLFOCUS:
        if (!host->window_policy.managed_lockdown) break;
        if (!host->stopping && host->state == ExamHostState::running) {
            SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            SetForegroundWindow(window);
        }
        return 0;
    case WM_SYSCOMMAND:
        if (!host->window_policy.managed_lockdown) break;
        if ((wparam & 0xfff0u) == SC_CLOSE ||
            (wparam & 0xfff0u) == SC_MINIMIZE ||
            (wparam & 0xfff0u) == SC_MAXIMIZE) {
            return 0;
        }
        break;
    case WM_CLOSE:
        if (!host->window_policy.managed_lockdown && host->api != nullptr) {
            (void)host->api->request_close();
        }
        // Only the authenticated host/service path calls ExamHost::stop().
        return 0;
    case kHostMessageExamStop:
        // wParam==0 is the explicit technician/service/recovery request. The
        // internal failure-cleanup request uses a nonzero marker and is
        // ignored if a new operation has already replaced the failed one.
        if (wparam == 0 || host->state == ExamHostState::failed) {
            if (host->api != nullptr) {
                if (host->window_policy.managed_lockdown) host->api->stop(wparam == 0 ? 0 : host->command_publication);
                else (void)host->api->request_close();
            }
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        host->window = nullptr;
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

bool register_exam_window_class(HINSTANCE instance) {
    WNDCLASSEXW klass{};
    klass.cbSize = sizeof(klass);
    klass.hInstance = instance;
    klass.lpfnWndProc = exam_window_proc;
    klass.lpszClassName = kExamWindowClass;
    klass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    klass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    if (RegisterClassExW(&klass) != 0) return true;
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

#if NSTU_HAS_WEBVIEW2

using CreateEnvironmentWithOptionsFn = HRESULT(STDAPICALLTYPE*) (
    PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions*,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);
using GetAvailableBrowserVersionFn = HRESULT(STDAPICALLTYPE*) (
    PCWSTR, LPWSTR*);

// MinGW does not emit __uuidof specializations for the SDK's C++ interface
// declarations.  Use the GUID constants published by WebView2.h explicitly
// so the optional host links with both MSVC and MinGW.
template <typename Interface>
const IID& webview_interface_iid() noexcept;

#define NSTU_WEBVIEW_IID(interface_name)                                      \
    template <>                                                               \
    const IID& webview_interface_iid<interface_name>() noexcept {             \
        return IID_##interface_name;                                          \
    }

NSTU_WEBVIEW_IID(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler)
NSTU_WEBVIEW_IID(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)
NSTU_WEBVIEW_IID(ICoreWebView2NavigationStartingEventHandler)
NSTU_WEBVIEW_IID(ICoreWebView2NewWindowRequestedEventHandler)
NSTU_WEBVIEW_IID(ICoreWebView2WebMessageReceivedEventHandler)
NSTU_WEBVIEW_IID(ICoreWebView2WebResourceRequestedEventHandler)
NSTU_WEBVIEW_IID(ICoreWebView2NavigationCompletedEventHandler)
NSTU_WEBVIEW_IID(ICoreWebView2ProcessFailedEventHandler)
NSTU_WEBVIEW_IID(ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler)

#undef NSTU_WEBVIEW_IID

// The Microsoft WRL package supplies Callback<> on MSVC, but the MinGW
// compatibility headers intentionally expose only ComPtr.  Keep the host
// portable by providing the small COM adapter needed by WebView2 event
// interfaces directly.  The adapter owns its callable through COM reference
// counting and therefore has the same lifetime semantics as WRL::Callback.
template <typename Interface, typename... Arguments>
class WebViewCallback final : public Interface {
public:
    explicit WebViewCallback(std::function<HRESULT(Arguments...)> function)
        : function_(std::move(function)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                              void** object) override {
        if (object == nullptr) return E_POINTER;
        *object = nullptr;
        if (riid == IID_IUnknown || riid == webview_interface_iid<Interface>()) {
            *object = static_cast<Interface*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return references_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG previous = references_.fetch_sub(
            1, std::memory_order_acq_rel);
        if (previous == 1) {
            delete this;
            return 0;
        }
        return previous - 1;
    }

    HRESULT STDMETHODCALLTYPE Invoke(Arguments... arguments) override {
        if (!function_) return E_UNEXPECTED;
        try {
            return function_(std::forward<Arguments>(arguments)...);
        } catch (...) {
            // Never allow a C++ exception to cross the COM callback ABI. A
            // failing callback is reported to WebView2 as a failed HRESULT;
            // the host's process-failure/status path performs cleanup.
            return E_FAIL;
        }
    }

private:
    std::atomic<ULONG> references_{1};
    std::function<HRESULT(Arguments...)> function_;
};

template <typename Interface, typename... Arguments, typename Callable>
Microsoft::WRL::ComPtr<Interface> make_webview_callback(Callable&& function) {
    std::function<HRESULT(Arguments...)> adapted(
        std::forward<Callable>(function));
    auto* callback = new (std::nothrow)
        WebViewCallback<Interface, Arguments...>(std::move(adapted));
    if (callback == nullptr) return {};
    Microsoft::WRL::ComPtr<Interface> result;
    result.Attach(callback);
    return result;
}

bool relative_is_web_path(const ExamHost::Impl& host,
                          std::string_view relative) {
    std::error_code relative_error;
    const auto web_relative = std::filesystem::relative(
        host.package.web_root, host.package.package_root, relative_error);
    if (relative_error || web_relative.empty() || web_relative == ".") {
        return false;
    }
    auto web_prefix = web_relative.generic_string();
    if (web_prefix.empty() || web_prefix == "." ||
        web_prefix.starts_with("..")) {
        return false;
    }
    if (!web_prefix.ends_with('/')) web_prefix.push_back('/');
    return relative.size() > web_prefix.size() &&
           relative.starts_with(web_prefix);
}

bool allowed_exam_web_uri(const ExamHost::Impl& host,
                          std::wstring_view uri) {
    constexpr std::wstring_view prefix = L"https://nstu.exam/";
    if (uri.size() <= prefix.size() ||
        _wcsnicmp(uri.data(), prefix.data(), prefix.size()) != 0) {
        return false;
    }
    const auto path = uri.substr(prefix.size());
    // Navigation and message origins must be a package web path, not merely
    // an arbitrary file mapped by the virtual host. Query/fragment suffixes
    // are intentionally rejected so the browser and native resource policy
    // observe the same canonical path.
    if (path.find_first_of(L"?#\\") != std::wstring_view::npos) {
        return false;
    }
    std::string relative;
    if (!decode_uri_path(path, relative) || !valid_relative_asset(relative)) {
        return false;
    }
    return relative_is_web_path(host, relative);
}

bool allowed_package_resource(const ExamHost::Impl& host,
                              std::wstring_view uri) {
    constexpr std::wstring_view prefix = L"https://nstu.exam/";
    if (uri.size() <= prefix.size() ||
        _wcsnicmp(uri.data(), prefix.data(), prefix.size()) != 0) {
        return false;
    }
    const auto path = uri.substr(prefix.size());
    // Query strings, fragments, and backslashes are rejected at this boundary.
    // Percent escapes are decoded once below, then validated as ordinary
    // relative paths so encoded traversal and alternate separators cannot
    // bypass the allowlist.
    if (path.find_first_of(L"?#\\") != std::wstring_view::npos) {
        return false;
    }
    std::string relative;
    if (!decode_uri_path(path, relative) ||
        !valid_relative_asset(relative)) {
        return false;
    }
    if (is_private_package_asset(relative)) return false;
    if (relative_is_web_path(host, relative)) return true;
    return host.allowed_package_assets.contains(relative);
}

HRESULT block_web_resource(ExamHost::Impl& host,
                           ICoreWebView2WebResourceRequestedEventArgs* args) {
    if (args == nullptr || host.environment == nullptr) return E_INVALIDARG;
    Microsoft::WRL::ComPtr<ICoreWebView2WebResourceResponse> response;
    constexpr wchar_t headers[] =
        L"Content-Length: 0\r\nCache-Control: no-store\r\n";
    const HRESULT create_result = host.environment->CreateWebResourceResponse(
        nullptr, 403, L"Blocked", headers, &response);
    if (FAILED(create_result) || response == nullptr) return create_result;
    return args->put_Response(response.Get());
}

HRESULT on_web_resource_requested(
    ExamHost::Impl& host,
    ICoreWebView2WebResourceRequestedEventArgs* args) {
    if (args == nullptr) return E_INVALIDARG;
    Microsoft::WRL::ComPtr<ICoreWebView2WebResourceRequest> request;
    if (FAILED(args->get_Request(&request)) || request == nullptr) {
        return block_web_resource(host, args);
    }
    LPWSTR raw_uri = nullptr;
    const HRESULT uri_result = request->get_Uri(&raw_uri);
    const std::wstring uri = raw_uri != nullptr ? raw_uri : L"";
    if (raw_uri != nullptr) CoTaskMemFree(raw_uri);
    if (FAILED(uri_result) || !allowed_package_resource(host, uri)) {
        // A page can issue many rejected subresource requests at once. Keep
        // the diagnostic useful without turning the status queue into a
        // request-amplification channel.
        const auto now = GetTickCount64();
        if (host.last_blocked_resource_notice_ms == 0 ||
            now - host.last_blocked_resource_notice_ms >= 1000) {
            host.last_blocked_resource_notice_ms = now;
            notify_status(host,
                          "Exam network or private-package request blocked.");
        }
        return block_web_resource(host, args);
    }
    return S_OK;
}

bool post_webview_json(ExamHost::Impl& host, std::string_view json) {
    if (host.webview == nullptr || json.empty()) return false;
    const auto wide = utf8_to_wide(json);
    return !wide.empty() && SUCCEEDED(host.webview->PostWebMessageAsJson(wide.c_str()));
}

void post_telemetry_enable(ExamHost::Impl& host, bool maintenance = false) noexcept {
    if (!host.window_policy.managed_lockdown || !host.webview_ready || host.webview == nullptr) return;
    try {
        const auto post = [&host](std::string_view json) { return post_webview_json(host, json); };
        if (maintenance) (void)host.telemetry.maintain_browser_startup(post);
        else (void)host.telemetry.post_browser_enable(post);
    } catch (...) {
        // Telemetry publication failure must not alter answer availability.
    }
}

HRESULT on_navigation_starting(ExamHost::Impl& host,
                               ICoreWebView2NavigationStartingEventArgs* args) {
    if (args == nullptr) {
        notify_status(host, "WebView2 supplied an invalid navigation event.");
        return E_INVALIDARG;
    }
    LPWSTR raw_uri = nullptr;
    const HRESULT result = args->get_Uri(&raw_uri);
    const std::wstring uri = raw_uri != nullptr ? raw_uri : L"";
    if (raw_uri != nullptr) CoTaskMemFree(raw_uri);
    if (FAILED(result) || !allowed_exam_web_uri(host, uri)) {
        (void)args->put_Cancel(TRUE);
        notify_status(host, "Exam navigation was blocked by the local policy.");
        return S_OK;
    }
    if (!host.window_policy.managed_lockdown) {
        // Reload/replacement could discard live drafts, including a frozen capture.
        // The owner must preserve and close this host before opening another page.
        if (host.editable_document_created) {
            (void)args->put_Cancel(TRUE);
            notify_status(host, "Preserve current drafts before reopening the editor.");
            return S_OK;
        }
        host.close_capture.stop();
        const exam::StateRequest context{host.context.package_id, host.context.package_digest,
            host.context.client_id, host.context.session_id, host.context.candidate_id};
        if (!host.close_capture.start(context, host.package.manifest_json, ++host.document_generation)) {
            (void)args->put_Cancel(TRUE);
            set_host_state(host, ExamHostState::failed, "Close capture context is unavailable.");
            return S_OK;
        }
    }
    // Navigation starts a new generation before any document can send data.
    host.webview_ready = false;
    if (host.window_policy.managed_lockdown) host.telemetry.advance_collector();
    return S_OK;
}

HRESULT on_web_message(ExamHost::Impl& host,
                       ICoreWebView2WebMessageReceivedEventArgs* args) {
    if (args == nullptr) {
        if (!host.window_policy.managed_lockdown) host.close_capture.invalidate();
        notify_status(host, "WebView2 supplied an invalid message event.");
        return E_INVALIDARG;
    }
    LPWSTR raw_source = nullptr;
    LPWSTR raw_json = nullptr;
    const auto source_result = args->get_Source(&raw_source);
    const auto json_result = args->get_WebMessageAsJson(&raw_json);
    const auto release = [](wchar_t* value) noexcept { CoTaskMemFree(value); };
    const std::unique_ptr<wchar_t, decltype(release)> owned_source(raw_source, release);
    const std::unique_ptr<wchar_t, decltype(release)> owned_json(raw_json, release);
    if (FAILED(source_result) || FAILED(json_result)) {
        if (!host.window_policy.managed_lockdown) host.close_capture.invalidate();
        return S_OK;
    }
    const std::wstring source = raw_source != nullptr ? raw_source : L"";
    const std::wstring json_wide = raw_json != nullptr ? raw_json : L"";
    if (!allowed_exam_web_uri(host, source)) {
        notify_status(host, "Exam message from an untrusted source was ignored.");
        return S_OK;
    }
    const auto json = wide_to_utf8(json_wide);
    if (json.empty() || json.size() > kMaximumJsonBytes) return S_OK;
    JsonValue root;
    std::string parse_error;
    if (!parse_json_object(json, root, &parse_error)) {
        notify_status(host, "Exam message JSON was rejected.");
        return S_OK;
    }
    std::string type;
    if (!string_member(root, "type", type)) return S_OK;
    if (type == "exam_close_capture_complete" || type == "exam_close_capture_failed") {
        if (!host.window_policy.managed_lockdown && host.programming_package && host.webview_ready &&
            host.close_capture.accept(json)) notify_capture(host);
        return S_OK;
    }
    if (type == "exam_programming_request" && host.programming_package) {
        exam::StateRequest trusted{host.context.package_id, host.context.package_digest, host.context.client_id,
                                  host.context.session_id, host.context.candidate_id};
        const auto request = parse_browser_programming_request(json, host.package.manifest_json, trusted, &parse_error);
        if (!request || (host.window_policy.managed_lockdown
                ? !host.callbacks.send_to_service : !host.callbacks.enqueue_programming_request)) {
            notify_status(host, "Programming request rejected by the native host.");
            return S_OK;
        }
        if (!host.window_policy.managed_lockdown) host.editable_document_created = true;
        if (!track_browser_programming_request(host.programming_pending, *request)) {
            (void)post_webview_json(host, exam::programming::unavailable_response_json(*request, "native_queue_unavailable"));
            return S_OK;
        }
        ++host.programming_pending_revision;
        if (host.window_policy.managed_lockdown) {
            host.callbacks.send_to_service({AgentMessageType::exam_programming_request, exam::programming::encode_request(*request)});
        } else {
            bool accepted = false;
            try { accepted = host.callbacks.enqueue_programming_request(*request); }
            catch (...) { notify_status(host, "Programming request queue failed; source remains pending."); }
            if (!accepted) {
                (void)post_webview_json(host, exam::programming::unavailable_response_json(*request, "native_queue_unavailable"));
            }
        }
    } else if (type == "exam_answer_event" && !host.programming_package) {
        exam::AnswerEvent event;
        if (!parse_answer_event(root, host.context, event, &parse_error)) {
            notify_status(host, "Exam answer event was rejected: " + parse_error);
            return S_OK;
        }
        const auto payload = exam::encode_answer_event(event);
        if (payload.empty()) {
            notify_status(host, "Exam answer event could not be encoded.");
            return S_OK;
        }
        if (host.callbacks.send_to_service) {
            host.callbacks.send_to_service(
                {AgentMessageType::exam_answer_event, payload});
        }
    } else if (type == "exam_state_request" && !host.programming_package) {
        exam::StateRequest request;
        if (!parse_state_request(root, host.context, request, &parse_error)) {
            notify_status(host, "Exam state request was rejected: " + parse_error);
            return S_OK;
        }
        const auto payload = exam::encode_state_request(request);
        if (payload.empty()) return S_OK;
        if (host.callbacks.send_to_service) {
            host.callbacks.send_to_service(
                {AgentMessageType::exam_state_request, payload});
        }
    } else if (type == "exam_ready") {
        if (!host.window_policy.managed_lockdown) host.editable_document_created = true;
        // The page signals that its DOM/assets are usable, but the identity in
        // the browser message is not trusted. Build ERDY exclusively from the
        // immutable, native-validated context and emit it at most once.
        if (!host.ready_reported && (host.window_policy.managed_lockdown
                ? static_cast<bool>(host.callbacks.send_to_service) : static_cast<bool>(host.callbacks.report_ready))) {
            exam::ExamReadyReport report;
            report.package_id = host.context.package_id;
            report.candidate_id = host.context.candidate_id;
            report.package_digest = host.context.package_digest;
            report.client_id = host.context.client_id;
            report.session_id = host.context.session_id;
            const auto payload = exam::encode_exam_ready_report(report);
            if (!payload.empty()) {
                if (host.window_policy.managed_lockdown) {
                    host.callbacks.send_to_service({AgentMessageType::exam_ready, payload});
                    host.ready_reported = true;
                } else {
                    try { host.ready_reported = host.callbacks.report_ready(report); }
                    catch (...) { notify_status(host, "Contest readiness queue failed."); }
                }
                if (host.ready_reported) post_webview_json(host, "{\"type\":\"exam_host_ready\"}");
            } else {
                notify_status(host, "Exam readiness could not be encoded.");
            }
        }
    } else if (type == "exam_telemetry_samples") {
        if (!host.window_policy.managed_lockdown || !host.telemetry.enabled() || !host.webview_ready) {
            // Collection remains off without a matching enable.
            return S_OK;
        }
        std::vector<exam::TelemetrySample> samples;
        if (!host.telemetry.forward_browser_samples(
                json, host.valid_question_ids, ExamHostTelemetryState::Clock::now(),
                samples, &parse_error)) {
            notify_status(host, "Exam telemetry rejected: " + parse_error);
            return S_OK;
        }
        const auto payload = encode_exam_telemetry_samples_payload(samples);
        if (!payload.empty() && host.callbacks.send_to_service) {
            host.callbacks.send_to_service(
                {AgentMessageType::exam_telemetry_samples, payload});
        }
    }
    return S_OK;
}

HRESULT on_new_window_requested(
    ExamHost::Impl& host, ICoreWebView2NewWindowRequestedEventArgs* args) {
    if (args == nullptr) {
        notify_status(host, "WebView2 supplied an invalid popup event.");
        return E_INVALIDARG;
    }
    // Exam content is confined to the single authenticated host window.  Do
    // not let target=_blank/window.open create an unmanaged browser surface.
    (void)args->put_Handled(TRUE);
    notify_status(host, "Exam popup was blocked by the local policy.");
    return S_OK;
}

HRESULT on_process_failed(ExamHost::Impl& host,
                          ICoreWebView2ProcessFailedEventArgs* args) {
    (void)args;
    // A browser/render/GPU process failure invalidates the kiosk boundary.
    // Mark the host failed and let the agent UI thread perform orderly
    // shutdown through its status-message path.
    host.webview_ready = false;
    set_host_state(host, ExamHostState::failed,
                   host.window_policy.managed_lockdown ? "WebView2 process failure detected; exam host stopped." :
                   "WebView2 process failed; drafts require recovery before closing.");
    return S_OK;
}

void configure_webview(ExamHost::Impl& host) {
    Microsoft::WRL::ComPtr<ICoreWebView2Settings> settings;
    if (host.webview != nullptr && SUCCEEDED(host.webview->get_Settings(&settings)) &&
        settings != nullptr) {
        (void)settings->put_IsScriptEnabled(TRUE);
        (void)settings->put_AreDefaultScriptDialogsEnabled(FALSE);
        (void)settings->put_IsStatusBarEnabled(FALSE);
        (void)settings->put_AreDevToolsEnabled(FALSE);
        (void)settings->put_AreDefaultContextMenusEnabled(FALSE);
        (void)settings->put_IsZoomControlEnabled(FALSE);
        (void)settings->put_IsBuiltInErrorPageEnabled(FALSE);
    }
}

void begin_webview(ExamHost::Impl& host, std::uint64_t generation);

bool package_snapshot_is_current(ExamHost::Impl& host) {
    ExamPackageReport refreshed;
    std::string error;
    if (!inspect_exam_package(host.package.package_root,
                              host.package.web_root,
                              host.package.digest_hex,
                              true, refreshed, &error)) {
        notify_status(host,
                      "Exam package changed or failed validation before launch.");
        return false;
    }
    // The digest covers every regular file and relative name in the package.
    // Rechecking immediately before virtual-host mapping closes the common
    // validation-to-navigation gap; deployment staging remains responsible
    // for eliminating races against an untrusted writable package directory.
    if (refreshed.digest_hex != host.package.digest_hex ||
        refreshed.package_root != host.package.package_root ||
        refreshed.web_root != host.package.web_root ||
        refreshed.page_path != host.package.page_path) {
        notify_status(host,
                      "Exam package changed after validation; launch aborted.");
        return false;
    }
    return true;
}

void on_environment_created(ExamHost::Impl& host, std::uint64_t generation,
                            HRESULT error, ICoreWebView2Environment* environment) {
    if (!callback_is_current(host, generation)) return;
    if (FAILED(error) || environment == nullptr || host.window == nullptr) {
        set_host_state(host, ExamHostState::failed,
                       "WebView2 environment initialization failed.");
        return;
    }
    host.environment = environment;
    host.controller = nullptr;
    auto callback = make_webview_callback<
        ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
        HRESULT, ICoreWebView2Controller*>(
        [&host, generation](HRESULT result,
                            ICoreWebView2Controller* controller) -> HRESULT {
            if (!callback_is_current(host, generation)) return S_OK;
            if (FAILED(result) || controller == nullptr) {
                set_host_state(host, ExamHostState::failed,
                               "WebView2 controller initialization failed.");
                return S_OK;
            }
            host.controller = controller;
            RECT bounds{};
            GetClientRect(host.window, &bounds);
            if (FAILED(host.controller->put_Bounds(bounds))) {
                set_host_state(host, ExamHostState::failed,
                               "WebView2 bounds could not be configured.");
                return S_OK;
            }
            if (FAILED(host.controller->get_CoreWebView2(&host.webview)) ||
                host.webview == nullptr) {
                set_host_state(host, ExamHostState::failed,
                               "WebView2 core object could not be created.");
                return S_OK;
            }
            configure_webview(host);
            if (!package_snapshot_is_current(host)) {
                set_host_state(host, ExamHostState::failed,
                               "Exam package changed during host initialization.");
                return S_OK;
            }
            Microsoft::WRL::ComPtr<ICoreWebView2_3> core3;
            const HRESULT core3_result = host.webview->QueryInterface(
                IID_ICoreWebView2_3,
                reinterpret_cast<void**>(core3.ReleaseAndGetAddressOf()));
            if (FAILED(core3_result) || core3 == nullptr ||
                FAILED(core3->SetVirtualHostNameToFolderMapping(
                    kExamVirtualHost, host.package.package_root.c_str(),
                    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW))) {
                set_host_state(host, ExamHostState::failed,
                               "WebView2 local package mapping failed.");
                return S_OK;
            }
            host.navigation_handler = make_webview_callback<
                ICoreWebView2NavigationStartingEventHandler,
                ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs*>(
                [&host, generation](ICoreWebView2*,
                                    ICoreWebView2NavigationStartingEventArgs* args)
                    -> HRESULT {
                    if (!callback_is_current(host, generation)) return S_OK;
                    return on_navigation_starting(host, args);
                });
            host.new_window_handler = make_webview_callback<
                ICoreWebView2NewWindowRequestedEventHandler,
                ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs*>(
                [&host, generation](ICoreWebView2*,
                                    ICoreWebView2NewWindowRequestedEventArgs* args)
                    -> HRESULT {
                    if (!callback_is_current(host, generation)) return S_OK;
                    return on_new_window_requested(host, args);
                });
            host.message_handler = make_webview_callback<
                ICoreWebView2WebMessageReceivedEventHandler,
                ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs*>(
                [&host, generation](ICoreWebView2*,
                                    ICoreWebView2WebMessageReceivedEventArgs* args)
                    -> HRESULT {
                    if (!callback_is_current(host, generation)) return S_OK;
                    try { return on_web_message(host, args); }
                    catch (...) {
                        if (!host.window_policy.managed_lockdown) host.close_capture.invalidate();
                        return E_FAIL;
                    }
                });
            host.web_resource_handler = make_webview_callback<
                ICoreWebView2WebResourceRequestedEventHandler,
                ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs*>(
                [&host, generation](ICoreWebView2*,
                                    ICoreWebView2WebResourceRequestedEventArgs*
                                        args) -> HRESULT {
                    if (!callback_is_current(host, generation)) return S_OK;
                    return on_web_resource_requested(host, args);
                });
            if (host.web_resource_handler == nullptr) {
                set_host_state(host, ExamHostState::failed,
                               "WebView2 resource policy handler could not be created.");
                return S_OK;
            }
            host.navigation_completed_handler = make_webview_callback<
                ICoreWebView2NavigationCompletedEventHandler,
                ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*>(
                [&host, generation](ICoreWebView2*,
                                    ICoreWebView2NavigationCompletedEventArgs* args)
                    -> HRESULT {
                    if (!callback_is_current(host, generation)) return S_OK;
                    BOOL success = FALSE;
                    if (args != nullptr) (void)args->get_IsSuccess(&success);
                    if (!success) {
                        host.webview_ready = false;
                        set_host_state(host, ExamHostState::failed,
                                       "Exam page navigation failed.");
                        return S_OK;
                    }
                    host.webview_ready = true;
                    if (!host.window_policy.managed_lockdown) host.editable_document_created = true;
                    set_host_state(host, ExamHostState::running,
                                   "Exam host is ready.");
                    post_webview_json(host, "{\"type\":\"exam_host_ready\"}");
                    post_telemetry_enable(host);
                    return S_OK;
                });
            host.process_failed_handler = make_webview_callback<
                ICoreWebView2ProcessFailedEventHandler,
                ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs*>(
                [&host, generation](ICoreWebView2*,
                                    ICoreWebView2ProcessFailedEventArgs* args)
                    -> HRESULT {
                    if (!callback_is_current(host, generation)) return S_OK;
                    return on_process_failed(host, args);
                });
            if (FAILED(host.webview->add_NavigationStarting(
                    host.navigation_handler.Get(), &host.navigation_token)) ||
                FAILED(host.webview->add_NewWindowRequested(
                    host.new_window_handler.Get(), &host.new_window_token)) ||
                FAILED(host.webview->add_WebMessageReceived(
                    host.message_handler.Get(), &host.message_token)) ||
                FAILED(host.webview->add_WebResourceRequested(
                    host.web_resource_handler.Get(), &host.web_resource_token)) ||
                FAILED(host.webview->add_NavigationCompleted(
                    host.navigation_completed_handler.Get(),
                    &host.navigation_completed_token)) ||
                FAILED(host.webview->add_ProcessFailed(
                    host.process_failed_handler.Get(),
                    &host.process_failed_token))) {
                set_host_state(host, ExamHostState::failed,
                               "WebView2 event handlers could not be installed.");
                return S_OK;
            }
            if (FAILED(host.webview->AddWebResourceRequestedFilter(
                    L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL))) {
                set_host_state(host, ExamHostState::failed,
                               "WebView2 resource policy filter could not be installed.");
                return S_OK;
            }
            host.web_resource_filter_added = true;
            const auto manifest = utf8_to_wide(host.package.manifest_json);
            const auto context = utf8_to_wide(host.options.context_json);
            if (manifest.empty() || context.empty()) {
                set_host_state(host, ExamHostState::failed,
                               "Exam bootstrap data is not valid UTF-8.");
                return S_OK;
            }
            // Bootstrap values are host-authenticated inputs.  Install them as
            // non-configurable, deeply frozen properties so package JavaScript
            // cannot replace the trust anchor before app.js captures it.  JSON
            // parsing also avoids interpreting package keys as object-literal
            // syntax at this native-to-browser boundary.
            const auto manifest_literal = javascript_string_literal(manifest);
            const auto context_literal = javascript_string_literal(context);
            const auto asset_base_literal = javascript_string_literal(
                L"https://nstu.exam/");
            const std::wstring script =
                L"(function(){"
                L"function deepFreeze(value){"
                L"if(value===null||typeof value!==\"object\"||Object.isFrozen(value))return value;"
                L"for(const key of Object.getOwnPropertyNames(value))deepFreeze(value[key]);"
                L"return Object.freeze(value);"
                L"}"
                L"const manifest=deepFreeze(JSON.parse(" + manifest_literal + L"));"
                L"const context=deepFreeze(JSON.parse(" + context_literal + L"));"
                L"const assetBase=" + asset_base_literal + L";"
                L"Object.defineProperty(window,\"NSTU_EXAM_MANIFEST\",{value:manifest,writable:false,configurable:false,enumerable:false});"
                L"Object.defineProperty(window,\"NSTU_EXAM_CONTEXT\",{value:context,writable:false,configurable:false,enumerable:false});"
                L"Object.defineProperty(window,\"NSTU_EXAM_ASSET_BASE\",{value:assetBase,writable:false,configurable:false,enumerable:false});"
                + (host.window_policy.managed_lockdown ? std::wstring{} :
                   L"Object.defineProperty(window,\"NSTU_EXAM_HOST_POLICY\",{value:\"ordinary_user\",writable:false,configurable:false,enumerable:false});") +
                L"})();";
            auto script_callback = make_webview_callback<
                ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler,
                HRESULT, LPCWSTR>(
                [](HRESULT, LPCWSTR) -> HRESULT { return S_OK; });
            if (FAILED(host.webview->AddScriptToExecuteOnDocumentCreated(
                    script.c_str(), script_callback.Get()))) {
                set_host_state(host, ExamHostState::failed,
                               "Exam bootstrap script could not be installed.");
                return S_OK;
            }
            std::error_code relative_error;
            const auto relative = std::filesystem::relative(
                host.package.page_path, host.package.package_root, relative_error);
            if (relative_error || relative.empty()) {
                set_host_state(host, ExamHostState::failed,
                               "Exam page path could not be resolved.");
                return S_OK;
            }
            std::wstring uri = L"https://nstu.exam/" + relative.generic_wstring();
            if (FAILED(host.webview->Navigate(uri.c_str()))) {
                set_host_state(host, ExamHostState::failed,
                               "Exam page navigation could not be started.");
            }
            return S_OK;
        });
    if (FAILED(host.environment->CreateCoreWebView2Controller(
            host.window, callback.Get()))) {
        set_host_state(host, ExamHostState::failed,
                       "WebView2 controller request failed.");
    }
}

void begin_webview(ExamHost::Impl& host, std::uint64_t generation) {
    if (!callback_is_current(host, generation)) return;
    std::filesystem::path user_data = host.options.user_data_root;
    if (user_data.empty()) {
        if (!host.options.allowed_user_data_root.empty()) {
            set_host_state(host, ExamHostState::failed,
                           "WebView2 user-data path is missing.");
            return;
        }
        wchar_t temporary[MAX_PATH]{};
        const DWORD length = GetTempPathW(MAX_PATH, temporary);
        if (length == 0 || length >= MAX_PATH) {
            set_host_state(host, ExamHostState::failed,
                           "WebView2 user-data path could not be resolved.");
            return;
        }
        user_data = std::filesystem::path(temporary) / L"NSTU" / L"exam-webview";
    }
    if (!host.options.allowed_user_data_root.empty()) {
        std::string path_error;
        if (!validate_exam_user_data_path(host.options.allowed_user_data_root,
                                          user_data, &path_error)) {
            set_host_state(host, ExamHostState::failed,
                           path_error.empty()
                               ? "WebView2 user-data path is outside its policy."
                               : path_error);
            return;
        }
    }
    std::error_code directory_error;
    std::filesystem::create_directories(user_data, directory_error);
    if (directory_error ||
        (!host.options.allowed_user_data_root.empty() &&
         !validate_exam_user_data_path(host.options.allowed_user_data_root,
                                       user_data, nullptr))) {
        set_host_state(host, ExamHostState::failed,
                       "WebView2 user-data directory could not be prepared.");
        return;
    }
    const auto executable = std::filesystem::path([&] {
        wchar_t buffer[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
        return std::wstring(buffer, length);
    }());
    const auto local_loader = executable.parent_path() / L"WebView2Loader.dll";
    // Only load the loader shipped beside the signed NSTU agent. Falling back
    // to an arbitrary system search path would allow an unrelated DLL (or a
    // stale runtime copy) to decide which browser implementation is started.
    const DWORD loader_attributes =
        GetFileAttributesW(local_loader.c_str());
    if (local_loader.empty() || loader_attributes == INVALID_FILE_ATTRIBUTES ||
        (loader_attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        path_has_reparse_point(local_loader)) {
        set_host_state(host, ExamHostState::failed,
                       "WebView2Loader.dll is missing or reparse-backed beside NSTU.");
        return;
    }
    host.loader = LoadLibraryExW(
        local_loader.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
            LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (host.loader == nullptr) {
        set_host_state(host, ExamHostState::failed,
                       "WebView2Loader.dll is not installed with NSTU.");
        return;
    }
    const auto create = load_function<CreateEnvironmentWithOptionsFn>(
        host.loader, "CreateCoreWebView2EnvironmentWithOptions");
    if (create == nullptr) {
        set_host_state(host, ExamHostState::failed,
                       "WebView2 loader does not expose the required API.");
        return;
    }
    const auto get_browser_version =
        load_function<GetAvailableBrowserVersionFn>(
            host.loader, "GetAvailableCoreWebView2BrowserVersionString");
    if (get_browser_version == nullptr) {
        set_host_state(host, ExamHostState::failed,
                       "WebView2 loader cannot probe the installed runtime.");
        return;
    }
    LPWSTR browser_version_raw = nullptr;
    const HRESULT browser_version_result =
        get_browser_version(nullptr, &browser_version_raw);
    const std::wstring browser_version =
        browser_version_raw != nullptr ? browser_version_raw : L"";
    if (browser_version_raw != nullptr) CoTaskMemFree(browser_version_raw);
    if (FAILED(browser_version_result) || browser_version.empty()) {
        set_host_state(host, ExamHostState::failed,
                       "A compatible WebView2 Runtime is not installed.");
        return;
    }
    notify_status(host, "WebView2 Runtime detected: " +
                           wide_to_utf8(browser_version));
    auto callback = make_webview_callback<
        ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
        HRESULT, ICoreWebView2Environment*>(
        [&host, generation](HRESULT result,
                            ICoreWebView2Environment* environment) -> HRESULT {
            if (!callback_is_current(host, generation)) return S_OK;
            on_environment_created(host, generation, result, environment);
            return S_OK;
        });
    if (FAILED(create(nullptr, user_data.c_str(), nullptr, callback.Get()))) {
        set_host_state(host, ExamHostState::failed,
                       "WebView2 environment request failed.");
    }
}

#endif // NSTU_HAS_WEBVIEW2

} // namespace

ExamHost::ExamHost() : impl_(std::make_unique<Impl>()) {
    impl_->api = this;
}

ExamHost::~ExamHost() { stop(); }

std::optional<ExamHostWindowPolicy> exam_host_window_policy(ExamHostPolicy policy) noexcept {
    switch (policy) {
    case ExamHostPolicy::managed: return ExamHostWindowPolicy{};
    case ExamHostPolicy::ordinary_user: return ExamHostWindowPolicy{WS_OVERLAPPEDWINDOW, 0, false};
    }
    return std::nullopt;
}

bool validate_exam_host_options(const ExamHostOptions& options,
                               ExamPackageReport& report, std::string* error) {
    ContextIdentity context;
    return parse_manifest_and_context(options, report, context, error);
}

bool ExamHost::start(HWND owner, const ExamHostOptions& options,
                     ExamHostCallbacks callbacks, std::string* error,
                     std::uint64_t command_publication) {
    if (impl_ != nullptr && impl_->options.policy == ExamHostPolicy::ordinary_user &&
        impl_->state != ExamHostState::idle && impl_->callbacks.before_close && !request_close()) {
        set_error(error, "previous ordinary exam host has unfinished close preparation");
        return false;
    }
    if (impl_ != nullptr) impl_->preserve_bridge_enable = true;
    stop(command_publication);
    if (impl_ == nullptr) impl_ = std::make_unique<Impl>();
    impl_->preserve_bridge_enable = false;
    impl_->command_publication = command_publication;
    impl_->api = this;
    [[maybe_unused]] const std::uint64_t generation =
        ++impl_->operation_generation;
    impl_->owner = owner;
    impl_->options = options;
    impl_->callbacks = std::move(callbacks);
    impl_->ready_reported = false;
    impl_->editable_document_created = false;
    impl_->programming_pending.clear();
    ++impl_->programming_pending_revision;
#if NSTU_HAS_WEBVIEW2
    impl_->webview_ready = false;
    impl_->last_blocked_resource_notice_ms = 0;
#endif
    set_host_state(*impl_, ExamHostState::preparing);
    const auto window_policy = exam_host_window_policy(options.policy);
    if (!window_policy) {
        set_error(error, "exam host policy is invalid");
        set_host_state(*impl_, ExamHostState::failed, "Exam host policy is invalid.");
        return false;
    }
    impl_->window_policy = *window_policy;
    if (!impl_->window_policy.managed_lockdown &&
        (impl_->callbacks.send_to_service || !impl_->callbacks.enqueue_programming_request ||
         !impl_->callbacks.report_ready || !impl_->callbacks.before_close)) {
        set_error(error, "ordinary exam host requires typed room callbacks and a preservation gate");
        set_host_state(*impl_, ExamHostState::failed, "Ordinary exam host callbacks are invalid.");
        return false;
    }
    if (!parse_manifest_and_context(options, impl_->package, impl_->context,
                                    error)) {
        set_host_state(*impl_, ExamHostState::failed,
                       error != nullptr ? *error : "Exam package validation failed.");
        return false;
    }
    impl_->allowed_package_assets.clear();
    if (!collect_manifest_assets(impl_->package.manifest_json,
                                 impl_->allowed_package_assets, error)) {
        set_host_state(*impl_, ExamHostState::failed,
                       error != nullptr ? *error :
                           "Exam package asset policy validation failed.");
        return false;
    }
    impl_->valid_question_ids.clear();
    impl_->programming_package = validate_programming_manifest(impl_->package.manifest_json);
    impl_->contest_duration_seconds = 0;
    if (!impl_->window_policy.managed_lockdown && impl_->programming_package) {
        ExamLockdownPolicy policy;
        if (!extract_exam_lockdown_policy(impl_->package.manifest_json, policy, error)) {
            set_host_state(*impl_, ExamHostState::failed, "Contest duration is unavailable.");
            return false;
        }
        impl_->contest_duration_seconds = policy.duration_seconds;
    }
    collect_manifest_question_ids(impl_->package.manifest_json, impl_->valid_question_ids);
    exam::StateRequest telemetry_context;
    telemetry_context.package_id = impl_->context.package_id;
    telemetry_context.package_digest = impl_->context.package_digest;
    telemetry_context.client_id = impl_->context.client_id;
    telemetry_context.session_id = impl_->context.session_id;
    telemetry_context.candidate_id = impl_->context.candidate_id;
    if (impl_->window_policy.managed_lockdown && !impl_->telemetry.start(std::move(telemetry_context))) {
        set_error(error, "exam telemetry context is invalid");
        stop(command_publication);
        return false;
    }
    set_host_state(*impl_, ExamHostState::initializing);
    enable_per_monitor_dpi_awareness();
    const auto instance = GetModuleHandleW(nullptr);
    if (!register_exam_window_class(instance)) {
        set_host_state(*impl_, ExamHostState::failed,
                       "Exam host window class registration failed.");
        set_error(error, "exam host window class registration failed");
        return false;
    }
    const bool managed = impl_->window_policy.managed_lockdown;
    const int x = managed ? GetSystemMetrics(SM_XVIRTUALSCREEN) : CW_USEDEFAULT;
    const int y = managed ? GetSystemMetrics(SM_YVIRTUALSCREEN) : CW_USEDEFAULT;
    const int width = managed ? std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN)) : CW_USEDEFAULT;
    const int height = managed ? std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN)) : CW_USEDEFAULT;
    impl_->window = CreateWindowExW(
        impl_->window_policy.extended_style, kExamWindowClass, kExamWindowTitle,
        impl_->window_policy.style, x, y, width, height, owner, nullptr, instance, impl_.get());
    if (impl_->window == nullptr) {
        set_host_state(*impl_, ExamHostState::failed,
                       "Exam host window creation failed.");
        set_error(error, "exam host window creation failed");
        return false;
    }
    if (SetTimer(impl_->window, kBridgeTimerId, kBridgeTimerPeriodMs, nullptr) == 0) {
        set_host_state(*impl_, ExamHostState::failed,
                       "Exam host bridge timer could not be installed.");
        set_error(error, "exam host bridge timer could not be installed");
        stop(command_publication);
        return false;
    }
    if (managed) {
        (void)SetTimer(impl_->window, kExamAliveTimerId, kExamAliveTimerPeriodMs, nullptr);
    }
#if NSTU_HAS_WEBVIEW2
    if (!begin_initialization_watchdog(*impl_, generation)) {
        set_host_state(*impl_, ExamHostState::failed,
                       "WebView2 initialization watchdog could not be installed.");
        set_error(error, "WebView2 initialization watchdog could not be installed");
        stop(command_publication);
        return false;
    }
#endif
    exam::StateRequest bridge_context;
    bridge_context.package_id = impl_->context.package_id;
    bridge_context.package_digest = impl_->context.package_digest;
    bridge_context.client_id = impl_->context.client_id;
    bridge_context.session_id = impl_->context.session_id;
    bridge_context.candidate_id = impl_->context.candidate_id;
    if (!exam_bridge().prepare_for_start(bridge_context, command_publication)) {
        set_error(error, "exam bridge context preparation failed");
        stop(command_publication);
        return false;
    }
    if (managed) {
        impl_->keyboard_hook = SetWindowsHookExW(WH_KEYBOARD_LL, exam_keyboard_proc, instance, 0);
        if (impl_->keyboard_hook == nullptr) {
            set_host_state(*impl_, ExamHostState::failed,
                           "Exam keyboard hook could not be installed.");
            set_error(error, "exam keyboard hook could not be installed");
            stop(command_publication);
            return false;
        }
        g_keyboard_host = impl_.get();
    }
    resize_exam_window(*impl_);
    ShowWindow(impl_->window, SW_SHOW);
    if (managed) {
        SetForegroundWindow(impl_->window);
        SetFocus(impl_->window);
    }
#if NSTU_HAS_WEBVIEW2
    begin_webview(*impl_, generation);
    if (impl_->state == ExamHostState::failed) {
        if (error != nullptr && error->empty()) {
            *error = "WebView2 host initialization failed";
        }
        stop(command_publication);
        return false;
    }
#else
    set_host_state(*impl_, ExamHostState::failed,
                   "This build does not include the WebView2 host module.");
    notify_status(*impl_, "Install the WebView2-enabled internal build to run exams.");
    set_error(error, "this build does not include the WebView2 host module");
    stop(command_publication);
    return false;
#endif
    return true;
}

void ExamHost::stop(std::uint64_t command_publication) noexcept {
    if (impl_ == nullptr) return;
    impl_->telemetry.stop();
    g_test_blocked_shortcuts.reset();
    ++impl_->operation_generation;
    impl_->stopping = true;
    impl_->close_capture.stop();
    impl_->contest_lifecycle.reset();
    impl_->contest_lifecycle_pending = false;
#if NSTU_HAS_WEBVIEW2
    impl_->webview_ready = false;
    impl_->last_blocked_resource_notice_ms = 0;
    // Kill the watchdog before releasing the browser objects.  DestroyWindow
    // also removes window timers, but doing this explicitly keeps the timer
    // state coherent if teardown is interrupted or the window is already
    // gone.
    cancel_initialization_watchdog(*impl_);
#endif
    if (impl_->keyboard_hook != nullptr) {
        UnhookWindowsHookEx(impl_->keyboard_hook);
        impl_->keyboard_hook = nullptr;
    }
    if (g_keyboard_host == impl_.get()) g_keyboard_host = nullptr;
#if NSTU_HAS_WEBVIEW2
    if (impl_->webview != nullptr) {
        if (impl_->navigation_token.value != 0)
            (void)impl_->webview->remove_NavigationStarting(impl_->navigation_token);
        if (impl_->new_window_token.value != 0)
            (void)impl_->webview->remove_NewWindowRequested(
                impl_->new_window_token);
        if (impl_->navigation_completed_token.value != 0)
            (void)impl_->webview->remove_NavigationCompleted(
                impl_->navigation_completed_token);
        if (impl_->message_token.value != 0)
            (void)impl_->webview->remove_WebMessageReceived(impl_->message_token);
        if (impl_->process_failed_token.value != 0)
            (void)impl_->webview->remove_ProcessFailed(
                impl_->process_failed_token);
        if (impl_->web_resource_token.value != 0)
            (void)impl_->webview->remove_WebResourceRequested(
                impl_->web_resource_token);
        if (impl_->web_resource_filter_added) {
            (void)impl_->webview->RemoveWebResourceRequestedFilter(
                L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
        }
    }
    impl_->navigation_token = {};
    impl_->new_window_token = {};
    impl_->navigation_completed_token = {};
    impl_->message_token = {};
    impl_->process_failed_token = {};
    impl_->web_resource_token = {};
    impl_->web_resource_filter_added = false;
    if (impl_->controller != nullptr) (void)impl_->controller->Close();
    impl_->webview.Reset();
    impl_->controller.Reset();
    impl_->environment.Reset();
    impl_->navigation_handler.Reset();
    impl_->new_window_handler.Reset();
    impl_->navigation_completed_handler.Reset();
    impl_->message_handler.Reset();
    impl_->process_failed_handler.Reset();
    impl_->web_resource_handler.Reset();
    if (impl_->loader != nullptr) {
        FreeLibrary(impl_->loader);
        impl_->loader = nullptr;
    }
#endif
    if (impl_->preserve_bridge_enable) {
        exam_bridge().clear_responses();
    } else if (command_publication != 0) {
        exam_bridge().clear_through_publication(command_publication);
    } else {
        exam_bridge().clear();
    }
    if (impl_->window != nullptr) {
        KillTimer(impl_->window, kBridgeTimerId);
        KillTimer(impl_->window, kExamAliveTimerId);
        DestroyWindow(impl_->window);
        impl_->window = nullptr;
    }
    impl_->callbacks = {};
    impl_->state = ExamHostState::idle;
    impl_->stopping = false;
}

bool ExamHost::request_close() noexcept {
    if (impl_ == nullptr || impl_->stopping || impl_->close_pending ||
        impl_->options.policy != ExamHostPolicy::ordinary_user || !impl_->callbacks.before_close) return false;
    impl_->close_pending = true;
    // A host that never created an editable document may close an empty failed
    // window through its owner's predicate, without inventing a browser capture.
    std::optional<ExamCloseCaptureReport> captured;
    try {
        captured = impl_->close_capture.report();
        if (impl_->editable_document_created && (!captured || !impl_->close_capture.is_current_complete(*captured))) {
            (void)prepare_capture();
            impl_->close_pending = false;
            return false;
        }
    } catch (...) { impl_->close_pending = false; return false; }
    const auto generation = impl_->operation_generation;
    const auto document_generation = impl_->document_generation;
    const auto pending_revision = impl_->programming_pending_revision;
    bool preserved = false;
    try {
        const auto before_close = impl_->callbacks.before_close;
        // Owner callbacks can enqueue/cancel/replace reentrantly. Keep their input
        // stable and recheck the live capture after the callback before teardown.
        const auto pending = impl_->programming_pending;
        if (impl_->editable_document_created) {
            preserved = impl_->close_capture.preserve([&](const ExamCloseCaptureReport&) { return before_close(pending); });
        } else preserved = before_close(pending);
    } catch (...) {
        notify_status(*impl_, "Close preparation failed; the editor remains open.");
    }
    impl_->close_pending = false;
    if (!preserved || generation != impl_->operation_generation || document_generation != impl_->document_generation ||
        pending_revision != impl_->programming_pending_revision ||
        (impl_->editable_document_created && (!captured || !impl_->close_capture.is_current_complete(*captured)))) return false;
    stop(impl_->command_publication);
    return true;
}

bool ExamHost::prepare_capture() noexcept {
#if NSTU_HAS_WEBVIEW2
    if (impl_ == nullptr || impl_->stopping || impl_->window_policy.managed_lockdown || !impl_->programming_package ||
        !impl_->editable_document_created || !impl_->webview_ready || !impl_->webview) return false;
    try {
        return impl_->close_capture.prepare([&](std::string_view json) { return post_webview_json(*impl_, json); });
    } catch (...) { return false; }
#else
    return false;
#endif
}

bool ExamHost::cancel_capture() noexcept {
    if (impl_ == nullptr || impl_->stopping || impl_->window_policy.managed_lockdown) return false;
    try {
        return impl_->close_capture.cancel([&](std::string_view json) {
            notify_capture(*impl_); // Native evidence is already consumed before JS can release editing.
#if NSTU_HAS_WEBVIEW2
            return impl_->webview && post_webview_json(*impl_, json);
#else
            (void)json;
            return false;
#endif
        });
    } catch (...) { return false; }
}

std::optional<ExamCloseCaptureReport> ExamHost::capture_report() const {
    return impl_ == nullptr ? std::nullopt : impl_->close_capture.report();
}

void ExamHost::drain_bridge() {
    if (impl_ == nullptr) return;
    if (impl_->close_capture.expired()) (void)cancel_capture();
#if NSTU_HAS_WEBVIEW2
    if (!impl_->webview_ready || impl_->webview == nullptr) return;
    if (impl_->contest_lifecycle_pending && !impl_->window_policy.managed_lockdown) {
        try {
            if (impl_->contest_lifecycle.publish([&](std::string_view message) {
                return post_webview_json(*impl_, message);
            })) impl_->contest_lifecycle_pending = false;
        } catch (...) { impl_->close_capture.invalidate(); }
    }
    if (const auto enable_msg = exam_bridge().try_pop_telemetry_enable(
            impl_->command_publication)) {
        const auto enable = exam::decode_telemetry_enable(enable_msg->payload);
        if (!enable.has_value() || !apply_telemetry_enable(*enable)) {
            notify_status(*impl_, "Exam telemetry enable rejected.");
        }
    }
    while (const auto message = exam_bridge().try_pop()) {
        if (!impl_->window_policy.managed_lockdown &&
            message->type != AgentMessageType::exam_programming_response &&
            message->type != AgentMessageType::exam_begin) continue;
        if (message->type == AgentMessageType::exam_answer_ack) {
            const auto ack = exam::decode_answer_ack(message->payload);
            if (ack.has_value() && ack->session_id == impl_->context.session_id) {
                post_webview_json(*impl_, make_ack_json(*ack));
            }
        } else if (message->type == AgentMessageType::exam_state_response) {
            const auto response = exam::decode_state_response(message->payload);
            if (response.has_value() &&
                response->package_id == impl_->context.package_id &&
                response->package_digest == impl_->context.package_digest &&
                response->client_id == impl_->context.client_id &&
                response->session_id == impl_->context.session_id &&
                response->candidate_id == impl_->context.candidate_id) {
                post_webview_json(*impl_, make_state_json(*response));
            }
        } else if (message->type == AgentMessageType::exam_programming_response && impl_->programming_package) {
            const std::string json(reinterpret_cast<const char*>(message->payload.data()), message->payload.size());
            const auto response = exam::programming::parse_response_json(json);
            if (!response) continue;
            const auto pending = impl_->programming_pending.find(response->request_id);
            if (pending == impl_->programming_pending.end()) continue;
            const auto validated = exam::programming::parse_response_json(json, &pending->second);
            if (validated && post_webview_json(*impl_, validated->json)) {
                impl_->programming_pending.erase(pending);
                ++impl_->programming_pending_revision;
            }
        } else if (message->type == AgentMessageType::exam_begin) {
            const auto begin = exam::decode_exam_begin_command(message->payload);
            if (begin.has_value() && same_context(*begin, impl_->context)) {
                post_webview_json(*impl_, make_begin_json(*begin));
            }
        }
    }
#else
    exam_bridge().clear_through_publication(impl_->command_publication);
#endif
#if NSTU_HAS_WEBVIEW2
    // Answer/control bridge work above keeps priority over calibration retries.
    post_telemetry_enable(*impl_, true);
#endif
    if (impl_->window_policy.managed_lockdown && impl_->telemetry.enabled() && impl_->callbacks.send_to_service) {
        const auto now_mono = impl_->telemetry.native_elapsed();
        if (!now_mono) return;
        const auto now_wall = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        if (const auto shortcut_sample = impl_->telemetry.drain_blocked_shortcuts(
                *now_mono, now_wall)) {
            const auto payload = encode_exam_telemetry_samples_payload(
                std::span<const exam::TelemetrySample>(&*shortcut_sample, 1));
            if (!payload.empty()) {
                impl_->callbacks.send_to_service(
                    {AgentMessageType::exam_telemetry_samples, payload});
            }
        }
    }
}

bool ExamHost::apply_contest_lifecycle(const ExamHostContestLifecycle& lifecycle) noexcept {
    if (!impl_ || impl_->stopping ||
        (impl_->state != ExamHostState::initializing && impl_->state != ExamHostState::running && impl_->state != ExamHostState::degraded) ||
        impl_->window_policy.managed_lockdown ||
        !impl_->programming_package || !same_context(lifecycle.context, impl_->context) ||
        (lifecycle.server_start != 0 && lifecycle.deadline - lifecycle.server_start !=
            static_cast<std::uint64_t>(impl_->contest_duration_seconds) * 1000)) return false;
    try {
        if (!impl_->contest_lifecycle.accept(lifecycle)) return false;
        impl_->contest_lifecycle_pending = true;
        return true;
    } catch (...) { impl_->close_capture.invalidate(); return false; }
}

void ExamHost::enforce_foreground() noexcept {
    if (impl_ == nullptr || impl_->window == nullptr || impl_->stopping ||
        impl_->state != ExamHostState::running || !impl_->window_policy.managed_lockdown) return;
    SetWindowPos(impl_->window, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (GetForegroundWindow() != impl_->window) SetForegroundWindow(impl_->window);
}

bool ExamHost::active() const noexcept {
    return impl_ != nullptr && impl_->state == ExamHostState::running;
}

ExamHostState ExamHost::state() const noexcept {
    return impl_ == nullptr ? ExamHostState::idle : impl_->state;
}

HWND ExamHost::window() const noexcept {
    return impl_ == nullptr ? nullptr : impl_->window;
}

std::uint64_t ExamHost::command_publication() const noexcept {
    return impl_ != nullptr ? impl_->command_publication : 0;
}

bool ExamHost::handle_window_message(UINT message, WPARAM wparam,
                                     LPARAM lparam, LRESULT& result) noexcept {
    if (impl_ == nullptr) return false;
    if (message == kHostMessageExamReady) {
        drain_bridge();
        enforce_foreground();
        result = 0;
        return true;
    }
    if (message == kHostMessageExamStop) {
        stop(wparam == 0 ? 0 : impl_->command_publication);
        result = 0;
        return true;
    }
    (void)wparam;
    (void)lparam;
    return false;
}

bool ExamHost::apply_telemetry_enable(
    const nstu::exam::TelemetryEnable& enable) noexcept {
    if (impl_ == nullptr || impl_->stopping || impl_->state == ExamHostState::failed ||
        !impl_->window_policy.managed_lockdown) {
        return false;
    }
    if (!impl_->telemetry.apply_enable(enable)) {
        return false;
    }
#if NSTU_HAS_WEBVIEW2
    post_telemetry_enable(*impl_);
#endif
    return true;
}

bool ExamHost::telemetry_enabled() const noexcept {
    return impl_ != nullptr && impl_->telemetry.enabled();
}

std::uint64_t ExamHost::telemetry_collector_id() const noexcept {
    return impl_ != nullptr ? impl_->telemetry.collector_id() : 0;
}

namespace {
bool parse_browser_telemetry_samples_impl(
    std::string_view json,
    std::uint64_t native_collector_id,
    const std::set<std::string>& allowed_question_ids,
    std::vector<nstu::exam::TelemetrySample>& out_samples,
    std::string* error, bool require_explicit_collector) {
    if (native_collector_id == 0 || native_collector_id > kMaximumExamCollectorId) {
        set_error(error, "invalid native collector id");
        return false;
    }
    if (json.empty() || json.size() > 32u * 1024u) {
        set_error(error, "telemetry json size is out of bounds");
        return false;
    }
    JsonValue root;
    if (!parse_json_object(json, root, error)) {
        return false;
    }
    std::string type;
    if (!string_member(root, "type", type) || type != "exam_telemetry_samples") {
        set_error(error, "telemetry message type is missing or invalid");
        return false;
    }
    for (const auto& [key, _] : root.object) {
        if (key != "type" && key != "samples" && key != "packageId" && key != "candidateId") {
            set_error(error, "unexpected member in telemetry root: " + key);
            return false;
        }
    }
    const auto* samples_node = member(root, "samples");
    if (samples_node == nullptr || samples_node->kind != JsonValue::Kind::array) {
        set_error(error, "telemetry samples array is missing or invalid");
        return false;
    }
    if (samples_node->array.empty() || samples_node->array.size() > 32) {
        set_error(error, "telemetry samples count is out of bounds (1..32)");
        return false;
    }

    static const std::set<std::string> kAllowedSampleFields = {
        "kind", "source", "collectorId", "questionId", "questionRevision",
        "monotonicElapsedMilliseconds", "durationMilliseconds",
        "clientTimeUnixMilliseconds", "numericValue", "count", "reason"
    };

    std::vector<nstu::exam::TelemetrySample> parsed_samples;
    parsed_samples.reserve(samples_node->array.size());

    for (const auto& item : samples_node->array) {
        if (item.kind != JsonValue::Kind::object) {
            set_error(error, "telemetry sample item must be an object");
            return false;
        }
        for (const auto& [field, val] : item.object) {
            if (!kAllowedSampleFields.contains(field)) {
                set_error(error, "forbidden or unknown field in sample: " + field);
                return false;
            }
            if (val.kind == JsonValue::Kind::object || val.kind == JsonValue::Kind::array) {
                set_error(error, "nested structure inside sample field is disallowed");
                return false;
            }
        }
        std::string kind_str;
        if (!string_member(item, "kind", kind_str)) {
            set_error(error, "sample kind is missing");
            return false;
        }
        nstu::exam::TelemetrySample sample;
        if (kind_str == "collector_started") sample.kind = nstu::exam::TelemetrySampleKind::collector_started;
        else if (kind_str == "question_first_view") sample.kind = nstu::exam::TelemetrySampleKind::question_first_view;
        else if (kind_str == "question_dwell") sample.kind = nstu::exam::TelemetrySampleKind::question_dwell;
        else if (kind_str == "question_first_answer") sample.kind = nstu::exam::TelemetrySampleKind::question_first_answer;
        else if (kind_str == "answer_change_summary") sample.kind = nstu::exam::TelemetrySampleKind::answer_change_summary;
        else if (kind_str == "focus_changed") sample.kind = nstu::exam::TelemetrySampleKind::focus_changed;
        else if (kind_str == "visibility_changed") sample.kind = nstu::exam::TelemetrySampleKind::visibility_changed;
        else if (kind_str == "blocked_shortcut_summary") sample.kind = nstu::exam::TelemetrySampleKind::blocked_shortcut_summary;
        else if (kind_str == "exam_submitted") sample.kind = nstu::exam::TelemetrySampleKind::exam_submitted;
        else if (kind_str == "loss_summary") sample.kind = nstu::exam::TelemetrySampleKind::loss_summary;
        else if (kind_str == "clock_discontinuity") sample.kind = nstu::exam::TelemetrySampleKind::clock_discontinuity;
        else {
            set_error(error, "unknown telemetry sample kind: " + kind_str);
            return false;
        }

        std::uint64_t browser_collector_id = 0;
        if (!uint64_member(item, "collectorId", browser_collector_id, require_explicit_collector) ||
            browser_collector_id > kMaximumExamCollectorId ||
            (require_explicit_collector && browser_collector_id == 0)) {
            set_error(error, "invalid collectorId");
            return false;
        }
        {
            if (browser_collector_id != 0 && browser_collector_id != native_collector_id) {
                set_error(error, "stale collector id rejected");
                return false;
            }
        }

        sample.source = nstu::exam::TelemetrySource::browser;
        sample.collector_id = native_collector_id;
        if (!string_member(item, "questionId", sample.question_id, false)) {
            set_error(error, "invalid questionId");
            return false;
        }
        if (sample.question_id.size() > nstu::exam::kMaximumQuestionIdBytes) {
            set_error(error, "questionId exceeds maximum length");
            return false;
        }
        if (!sample.question_id.empty() && !allowed_question_ids.empty()) {
            if (!allowed_question_ids.contains(sample.question_id)) {
                set_error(error, "questionId not in allowed questions");
                return false;
            }
        }

        std::uint64_t rev = 0;
        if (!uint64_member(item, "questionRevision", rev, false) ||
            rev > std::numeric_limits<std::uint32_t>::max()) {
            set_error(error, "invalid questionRevision");
            return false;
        }
        sample.question_revision = static_cast<std::uint32_t>(rev);

        if (sample.question_id.empty() && sample.question_revision != 0) {
            set_error(error, "empty questionId must have questionRevision 0");
            return false;
        }
        if (!sample.question_id.empty() && sample.question_revision == 0) {
            set_error(error, "nonempty questionId must have questionRevision > 0");
            return false;
        }

        if (!uint64_member(item, "monotonicElapsedMilliseconds", sample.monotonic_elapsed_milliseconds, require_explicit_collector) ||
            sample.monotonic_elapsed_milliseconds > kMaximumExamCollectorId) {
            set_error(error, "invalid monotonicElapsedMilliseconds");
            return false;
        }
        std::uint64_t dur = 0;
        if (!uint64_member(item, "durationMilliseconds", dur, false) ||
            dur > std::numeric_limits<std::uint32_t>::max()) {
            set_error(error, "invalid durationMilliseconds");
            return false;
        }
        sample.duration_milliseconds = static_cast<std::uint32_t>(dur);

        if (!uint64_member(item, "clientTimeUnixMilliseconds", sample.client_time_unix_milliseconds) ||
            sample.client_time_unix_milliseconds == 0) {
            set_error(error, "clientTimeUnixMilliseconds is missing or 0");
            return false;
        }

        std::uint64_t num_val = 0;
        if (!uint64_member(item, "numericValue", num_val, false) ||
            num_val > std::numeric_limits<std::uint32_t>::max()) {
            set_error(error, "invalid numericValue");
            return false;
        }
        sample.numeric_value = static_cast<std::uint32_t>(num_val);

        std::uint64_t cnt = 0;
        if (!uint64_member(item, "count", cnt, false) ||
            cnt > std::numeric_limits<std::uint32_t>::max()) {
            set_error(error, "invalid count");
            return false;
        }
        sample.count = static_cast<std::uint32_t>(cnt);

        std::string reason_str;
        if (!string_member(item, "reason", reason_str, false)) {
            set_error(error, "invalid reason");
            return false;
        }
        if (string_member(item, "reason", reason_str, false)) {
            if (reason_str == "none" || reason_str.empty()) sample.reason = nstu::exam::TelemetryReason::none;
            else if (reason_str == "collector_initial") sample.reason = nstu::exam::TelemetryReason::collector_initial;
            else if (reason_str == "collector_reload") sample.reason = nstu::exam::TelemetryReason::collector_reload;
            else if (reason_str == "collector_late_enable") sample.reason = nstu::exam::TelemetryReason::collector_late_enable;
            else if (reason_str == "navigation_button") sample.reason = nstu::exam::TelemetryReason::navigation_button;
            else if (reason_str == "navigation_arrow") sample.reason = nstu::exam::TelemetryReason::navigation_arrow;
            else if (reason_str == "navigation_initial") sample.reason = nstu::exam::TelemetryReason::navigation_initial;
            else if (reason_str == "discontinuity_monotonic") sample.reason = nstu::exam::TelemetryReason::discontinuity_monotonic;
            else if (reason_str == "discontinuity_wall_clock") sample.reason = nstu::exam::TelemetryReason::discontinuity_wall_clock;
            else if (reason_str == "discontinuity_suspend") sample.reason = nstu::exam::TelemetryReason::discontinuity_suspend;
            else if (reason_str == "focus_blur") sample.reason = nstu::exam::TelemetryReason::focus_blur;
            else if (reason_str == "focus_gain") sample.reason = nstu::exam::TelemetryReason::focus_gain;
            else if (reason_str == "visibility_hidden") sample.reason = nstu::exam::TelemetryReason::visibility_hidden;
            else if (reason_str == "visibility_visible") sample.reason = nstu::exam::TelemetryReason::visibility_visible;
            else if (reason_str == "shortcut_system") sample.reason = nstu::exam::TelemetryReason::shortcut_system;
            else if (reason_str == "shortcut_devtools") sample.reason = nstu::exam::TelemetryReason::shortcut_devtools;
            else if (reason_str == "shortcut_clipboard") sample.reason = nstu::exam::TelemetryReason::shortcut_clipboard;
            else if (reason_str == "submit_manual") sample.reason = nstu::exam::TelemetryReason::submit_manual;
            else if (reason_str == "submit_timeout") sample.reason = nstu::exam::TelemetryReason::submit_timeout;
            else if (reason_str == "loss_queue_full") sample.reason = nstu::exam::TelemetryReason::loss_queue_full;
            else if (reason_str == "loss_rate_limit") sample.reason = nstu::exam::TelemetryReason::loss_rate_limit;
            else {
                set_error(error, "unknown reason: " + reason_str);
                return false;
            }
        } else {
            std::uint64_t raw_reason = 0;
            if (uint64_member(item, "reason", raw_reason, false)) {
                if (!nstu::exam::valid_telemetry_reason(static_cast<nstu::exam::TelemetryReason>(raw_reason))) {
                    set_error(error, "invalid numeric reason");
                    return false;
                }
                sample.reason = static_cast<nstu::exam::TelemetryReason>(raw_reason);
            }
        }

        if (!nstu::exam::validate_telemetry_sample(sample)) {
            set_error(error, "validate_telemetry_sample failed");
            return false;
        }

        parsed_samples.push_back(std::move(sample));
    }

    out_samples = std::move(parsed_samples);
    return true;
}

} // namespace

bool parse_browser_telemetry_samples(
    std::string_view json, std::uint64_t native_collector_id,
    const std::set<std::string>& allowed_question_ids,
    std::vector<exam::TelemetrySample>& out_samples, std::string* error) {
    return parse_browser_telemetry_samples_impl(json, native_collector_id,
        allowed_question_ids, out_samples, error, false);
}

std::string make_exam_telemetry_enable_json(
    const exam::TelemetryEnable& enable, std::uint64_t collector_id,
    std::uint64_t collector_elapsed_milliseconds, bool startup_retry) {
    if (!exam::validate_telemetry_enable(enable) || collector_id == 0 ||
        collector_id > kMaximumExamCollectorId ||
        collector_elapsed_milliseconds > kMaximumExamCollectorId) return {};
    return make_telemetry_enable_json(enable, collector_id, collector_elapsed_milliseconds, startup_retry);
}

namespace {

inline void append_u16_le(std::vector<std::byte>& out, std::uint16_t value) {
    out.push_back(static_cast<std::byte>(value & 0xffu));
    out.push_back(static_cast<std::byte>((value >> 8u) & 0xffu));
}

inline bool read_u16_le(std::span<const std::byte> in, std::size_t& offset, std::uint16_t& out) {
    if (offset + 2 > in.size()) return false;
    out = static_cast<std::uint16_t>(
        static_cast<std::uint8_t>(in[offset]) |
        (static_cast<std::uint8_t>(in[offset + 1]) << 8u));
    offset += 2;
    return true;
}

} // namespace

std::vector<std::byte> encode_exam_telemetry_samples_payload(
    std::span<const nstu::exam::TelemetrySample> samples) {
    if (samples.empty() || samples.size() > 32) {
        return {};
    }
    std::vector<std::byte> wire;
    wire.reserve(64 * samples.size());
    append_u16_le(wire, static_cast<std::uint16_t>(samples.size()));
    for (const auto& sample : samples) {
        const auto sample_bytes = nstu::exam::encode_telemetry_sample(sample);
        if (sample_bytes.empty() || sample_bytes.size() > 0xffffu) {
            return {};
        }
        append_u16_le(wire, static_cast<std::uint16_t>(sample_bytes.size()));
        wire.insert(wire.end(), sample_bytes.begin(), sample_bytes.end());
    }
    return wire;
}

std::optional<std::vector<nstu::exam::TelemetrySample>>
decode_exam_telemetry_samples_payload(
    std::span<const std::byte> payload,
    std::string* error) {
    if (payload.size() < 2) {
        set_error(error, "telemetry samples payload too short");
        return std::nullopt;
    }
    std::size_t offset = 0;
    std::uint16_t count = 0;
    if (!read_u16_le(payload, offset, count) || count == 0 || count > 32) {
        set_error(error, "invalid telemetry sample count");
        return std::nullopt;
    }
    std::vector<nstu::exam::TelemetrySample> samples;
    samples.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        std::uint16_t len = 0;
        if (!read_u16_le(payload, offset, len)) {
            set_error(error, "truncated sample length");
            return std::nullopt;
        }
        if (offset + len > payload.size()) {
            set_error(error, "truncated sample body");
            return std::nullopt;
        }
        const auto sample_span = payload.subspan(offset, len);
        offset += len;
        const auto sample = nstu::exam::decode_telemetry_sample(sample_span, error);
        if (!sample.has_value()) {
            return std::nullopt;
        }
        samples.push_back(*sample);
    }
    if (offset != payload.size()) {
        set_error(error, "trailing bytes in telemetry samples payload");
        return std::nullopt;
    }
    return samples;
}

void record_blocked_shortcut_for_testing(std::uint32_t count) noexcept {
    // This existing codec-test fixture simulates an enabled hook recording.
    if (!g_test_blocked_shortcuts.enabled()) g_test_blocked_shortcuts.reset(true);
    g_test_blocked_shortcuts.record(count);
}

namespace {
std::optional<exam::TelemetrySample> make_shortcut_sample(
                        ExamShortcutCounter& shortcuts, std::uint64_t collector_id,
                        std::uint64_t monotonic_elapsed_ms,
                        std::uint64_t client_time_unix_ms) noexcept {
    const auto count = shortcuts.drain();
    if (count == 0) return std::nullopt;
    nstu::exam::TelemetrySample sample;
    sample.kind = nstu::exam::TelemetrySampleKind::blocked_shortcut_summary;
    sample.source = nstu::exam::TelemetrySource::native;
    sample.collector_id = collector_id;
    sample.count = count;
    sample.reason = nstu::exam::TelemetryReason::shortcut_system;
    sample.monotonic_elapsed_milliseconds = monotonic_elapsed_ms;
    sample.client_time_unix_milliseconds = client_time_unix_ms;
    return sample;
}
} // namespace

std::optional<exam::TelemetrySample> drain_blocked_shortcuts(
    std::uint64_t collector_id, std::uint64_t monotonic_elapsed_ms,
    std::uint64_t client_time_unix_ms) noexcept {
    return make_shortcut_sample(g_test_blocked_shortcuts, collector_id,
                               monotonic_elapsed_ms, client_time_unix_ms);
}

std::uint64_t allocate_exam_telemetry_collector_id() noexcept {
    static std::atomic<std::uint64_t> next_id([]() noexcept {
        for (unsigned attempt = 0; attempt < 8; ++attempt) {
            std::array<std::byte, sizeof(std::uint64_t)> random{};
            if (!security::generate_random(random)) return std::uint64_t{0};
            std::uint64_t seed = 0;
            std::memcpy(&seed, random.data(), sizeof(seed));
            seed &= kMaximumExamCollectorId;
            if (seed != 0 && seed < kMaximumExamCollectorId) return seed;
        }
        return std::uint64_t{0};
    }());
    auto id = next_id.load(std::memory_order_relaxed);
    while (id != 0 && id <= kMaximumExamCollectorId) {
        if (next_id.compare_exchange_weak(id, id + 1, std::memory_order_relaxed)) return id;
    }
    return 0;
}

bool ExamHostTelemetryState::start(exam::StateRequest context, std::uint64_t collector_id,
                                   Clock::time_point origin) noexcept {
    stop();
    try {
        // The existing typed codec validates the complete native tuple.
        if (exam::encode_state_request(context).empty()) return false;
    } catch (...) {
        return false;
    }
    context_ = std::move(context);
    generation_origin_ = origin;
    last_native_observation_ = origin;
    browser_rate_window_start_ = origin;
    browser_rate_samples_ = 0;
    collector_id_ = collector_id <= kMaximumExamCollectorId ? collector_id : 0;
    return true;
}

void ExamHostTelemetryState::stop() noexcept {
    clear_browser_clock();
    last_native_observation_.reset();
    browser_rate_window_start_.reset();
    browser_rate_samples_ = 0;
    shortcuts_.reset();
    active_enable_.reset();
    collector_id_ = 0;
    context_ = {};
}

bool ExamHostTelemetryState::apply_enable(const exam::TelemetryEnable& enable) noexcept {
    if (collector_id_ == 0 || !exam::validate_telemetry_enable(enable) ||
        enable.package_id != context_.package_id ||
        enable.package_digest != context_.package_digest ||
        enable.client_id != context_.client_id ||
        enable.session_id != context_.session_id ||
        enable.candidate_id != context_.candidate_id) return false;
    if (active_enable_) return true;
    try {
        active_enable_ = enable;
    } catch (...) {
        return false;
    }
    shortcuts_.reset(true);
    return true;
}

void ExamHostTelemetryState::advance_collector(std::uint64_t collector_id, Clock::time_point origin) noexcept {
    if (collector_id_ == 0) return;
    if (collector_id == 0 || collector_id > kMaximumExamCollectorId || collector_id == collector_id_) {
        stop();
        return;
    }
    collector_id_ = collector_id;
    generation_origin_ = origin;
    last_native_observation_ = origin;
    clear_browser_clock();
    shortcuts_.reset(enabled());
}

void ExamHostTelemetryState::clear_browser_clock() noexcept {
    pending_browser_anchor_.reset();
    last_browser_elapsed_.reset();
    browser_clock_bound_ = false;
    last_startup_attempt_.reset();
    startup_retry_attempts_ = 0;
}

std::optional<std::uint64_t> ExamHostTelemetryState::native_elapsed(Clock::time_point now) noexcept {
    const auto origin_epoch = generation_origin_.time_since_epoch();
    if (collector_id_ == 0 || now < generation_origin_ ||
        (last_native_observation_ && now < *last_native_observation_) ||
        (origin_epoch < Clock::duration::zero() &&
         now.time_since_epoch() > Clock::duration::max() + origin_epoch)) {
        clear_browser_clock();
        return std::nullopt;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - generation_origin_).count();
    if (elapsed < 0 || static_cast<std::uint64_t>(elapsed) > kMaximumExamCollectorId) {
        clear_browser_clock();
        return std::nullopt;
    }
    last_native_observation_ = now;
    return static_cast<std::uint64_t>(elapsed);
}

bool ExamHostTelemetryState::post_browser_enable(
    const std::function<bool(std::string_view)>& post, Clock::time_point now) {
    if (!enabled() || !post) return false;
    if (pending_browser_anchor_) return true; // Preserve the first successful publication.
    const auto anchor = native_elapsed(now);
    if (!anchor) return false;
    const auto json = make_exam_telemetry_enable_json(*active_enable_, collector_id_, *anchor);
    try {
        if (json.empty() || !post(json)) {
            clear_browser_clock();
            return false;
        }
    } catch (...) {
        clear_browser_clock();
        return false;
    }
    pending_browser_anchor_ = *anchor;
    last_startup_attempt_ = now;
    return true;
}

bool ExamHostTelemetryState::maintain_browser_startup(
    const std::function<bool(std::string_view)>& post, Clock::time_point now) {
    if (!enabled() || !post || !pending_browser_anchor_ || browser_clock_bound_ ||
        !last_startup_attempt_ || startup_retry_attempts_ >= 3) return false;
    if (!native_elapsed(now)) return false;
    const auto interval = std::chrono::seconds(1);
    if (*last_startup_attempt_ > Clock::time_point::max() - interval ||
        now < *last_startup_attempt_ + interval) return false;
    // Failed retry publications also consume the finite, throttled budget.
    last_startup_attempt_ = now;
    ++startup_retry_attempts_;
    try {
        const auto json = make_exam_telemetry_enable_json(*active_enable_, collector_id_,
            *pending_browser_anchor_, true);
        return !json.empty() && post(json);
    } catch (...) {
        return false;
    }
}

bool ExamHostTelemetryState::forward_browser_samples(
    std::string_view json, const std::set<std::string>& allowed_question_ids,
    Clock::time_point receipt, std::vector<exam::TelemetrySample>& samples,
    std::string* error) {
    if (!enabled() || !pending_browser_anchor_) {
        set_error(error, "browser clock has no published anchor");
        return false;
    }
    std::vector<exam::TelemetrySample> parsed;
    // Inspect every explicitly supplied raw ID before the parser rebinds identity.
    if (!parse_browser_telemetry_samples_impl(json, collector_id_, allowed_question_ids,
            parsed, error, true)) return false;
    const auto received = native_elapsed(receipt);
    if (!received) {
        set_error(error, "native receipt clock regressed");
        return false;
    }
    const auto interval = std::chrono::seconds(1);
    if (!browser_rate_window_start_ ||
        (*browser_rate_window_start_ <= Clock::time_point::max() - interval &&
         receipt > *browser_rate_window_start_ + interval)) {
        browser_rate_window_start_ = receipt;
        browser_rate_samples_ = 0;
    }
    if (parsed.size() > 256u - browser_rate_samples_) {
        set_error(error, "browser telemetry burst limit exceeded");
        return false; // Retain the pending anchor; UI maintenance can retry startup.
    }
    auto last_elapsed = last_browser_elapsed_.value_or(*pending_browser_anchor_);
    const bool binding = !browser_clock_bound_;
    if (binding && (parsed.front().kind != exam::TelemetrySampleKind::collector_started ||
        parsed.front().monotonic_elapsed_milliseconds != *pending_browser_anchor_)) {
        set_error(error, "browser startup does not match the pending anchor");
        return false;
    }
    for (std::size_t i = 0; i < parsed.size(); ++i) {
        auto& sample = parsed[i];
        if (sample.kind == exam::TelemetrySampleKind::collector_started && (!binding || i != 0)) {
            set_error(error, "duplicate browser startup");
            return false;
        }
        if (sample.monotonic_elapsed_milliseconds < last_elapsed) {
            clear_browser_clock();
            set_error(error, "browser elapsed clock regressed");
            return false;
        }
        last_elapsed = sample.monotonic_elapsed_milliseconds;
    }
    if (binding) {
        const auto anchor = *pending_browser_anchor_;
        if (*received < anchor || *received - anchor > std::numeric_limits<std::uint32_t>::max() - 2ULL) {
            clear_browser_clock();
            set_error(error, "initial browser uncertainty is out of bounds");
            return false;
        }
        // Mapped estimates have an initial signed interval
        // [max(0, estimate-U), estimate+U], U = receipt-anchor+2 ms.
        // This covers delivery and quantization, not later drift or coarsening;
        // cross-source order within the interval remains unknown.
        parsed.front().numeric_value = static_cast<std::uint32_t>(*received - anchor + 2);
        browser_clock_bound_ = true;
    }
    last_browser_elapsed_ = last_elapsed;
    browser_rate_samples_ += static_cast<std::uint32_t>(parsed.size());
    samples = std::move(parsed);
    return true;
}

void ExamHostTelemetryState::record_blocked_shortcuts(std::uint32_t count) noexcept {
    shortcuts_.record(count);
}

std::optional<exam::TelemetrySample> ExamHostTelemetryState::drain_blocked_shortcuts(
    std::uint64_t monotonic_elapsed_ms, std::uint64_t client_time_unix_ms) noexcept {
    if (!enabled()) return std::nullopt;
    return make_shortcut_sample(shortcuts_, collector_id_, monotonic_elapsed_ms,
                               client_time_unix_ms);
}

} // namespace nstu::client
