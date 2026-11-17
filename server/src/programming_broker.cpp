#include "nstu/programming_broker.hpp"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <charconv>
#include <condition_variable>
#include <cwctype>
#include <deque>
#include <future>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

namespace nstu::server {
namespace {

constexpr std::size_t kMaximumQueuedRequests = 64;
constexpr std::size_t kMaximumQueuedBytes = 2u * 1024u * 1024u;
constexpr std::size_t kMaximumHttpRequestBytes = 128u * 1024u;
constexpr std::size_t kMaximumGradingRequestBytes = 4u * 1024u * 1024u;

std::wstring wide(std::string_view value) {
    if (value.empty() || value.size() > kMaximumHttpRequestBytes) return {};
    const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), length) != length) return {};
    return result;
}

struct Endpoint {
    std::wstring host;
    INTERNET_PORT port = 0;
    bool secure = false;
};

std::optional<Endpoint> endpoint(std::string_view url) {
    if (url.empty() || url.size() > 2048) return std::nullopt;
    const auto text = wide(url);
    if (text.empty()) return std::nullopt;
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = std::numeric_limits<DWORD>::max();
    parts.dwHostNameLength = std::numeric_limits<DWORD>::max();
    parts.dwUserNameLength = std::numeric_limits<DWORD>::max();
    parts.dwPasswordLength = std::numeric_limits<DWORD>::max();
    parts.dwUrlPathLength = std::numeric_limits<DWORD>::max();
    parts.dwExtraInfoLength = std::numeric_limits<DWORD>::max();
    if (!WinHttpCrackUrl(text.c_str(), static_cast<DWORD>(text.size()), 0, &parts) ||
        parts.dwHostNameLength == 0 || parts.dwHostNameLength > 253 || parts.dwUserNameLength != 0 ||
        parts.dwPasswordLength != 0 || parts.dwExtraInfoLength != 0 ||
        (parts.dwUrlPathLength != 0 && std::wstring_view(parts.lpszUrlPath, parts.dwUrlPathLength) != L"/")) return std::nullopt;
    Endpoint result{std::wstring(parts.lpszHostName, parts.dwHostNameLength), parts.nPort, parts.nScheme == INTERNET_SCHEME_HTTPS};
    std::transform(result.host.begin(), result.host.end(), result.host.begin(), [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
    const bool loopback = result.host == L"127.0.0.1" || result.host == L"localhost" || result.host == L"::1" || result.host == L"[::1]";
    if (parts.nScheme != INTERNET_SCHEME_HTTPS && (parts.nScheme != INTERNET_SCHEME_HTTP || !loopback)) return std::nullopt;
    return result;
}

struct HttpHandle {
    HINTERNET handle = nullptr;
    explicit HttpHandle(HINTERNET value) : handle(value) {}
    ~HttpHandle() { if (handle) WinHttpCloseHandle(handle); }
    HttpHandle(const HttpHandle&) = delete;
    HttpHandle& operator=(const HttpHandle&) = delete;
};

class AsyncHttpRequest {
public:
    AsyncHttpRequest(HINTERNET handle, std::chrono::steady_clock::time_point deadline) : handle_(handle), deadline_(deadline) {
        auto context = reinterpret_cast<DWORD_PTR>(this);
        bound_ = WinHttpSetOption(handle_, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context)) &&
            WinHttpSetStatusCallback(handle_, callback, WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES, 0) != WINHTTP_INVALID_STATUS_CALLBACK;
    }
    ~AsyncHttpRequest() {
        WinHttpCloseHandle(handle_);
        if (bound_) {
            // HANDLE_CLOSING is the final callback; keep context and I/O buffers alive until it arrives.
            std::unique_lock lock(mutex_);
            changed_.wait(lock, [&] { return closed_; });
        }
    }
    AsyncHttpRequest(const AsyncHttpRequest&) = delete;
    AsyncHttpRequest& operator=(const AsyncHttpRequest&) = delete;
    [[nodiscard]] HINTERNET handle() const noexcept { return handle_; }
    [[nodiscard]] DWORD_PTR context() const noexcept { return reinterpret_cast<DWORD_PTR>(this); }
    template<typename Start> bool run(DWORD completion, Start&& start) {
        {
            std::scoped_lock lock(mutex_);
            if (!bound_ || closed_ || std::chrono::steady_clock::now() >= deadline_) return false;
            completion_ = 0; failed_ = false; bytes_ = 0;
        }
        if (!start()) return false;
        std::unique_lock lock(mutex_);
        return changed_.wait_until(lock, deadline_, [&] { return closed_ || failed_ || completion_ == completion; }) &&
            !closed_ && !failed_ && std::chrono::steady_clock::now() < deadline_;
    }
    [[nodiscard]] DWORD bytes() {
        std::scoped_lock lock(mutex_); return bytes_;
    }
private:
    static void CALLBACK callback(HINTERNET, DWORD_PTR context, DWORD status, void*, DWORD length) {
        auto* request = reinterpret_cast<AsyncHttpRequest*>(context);
        if (!request) return;
        std::scoped_lock lock(request->mutex_);
        if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) request->closed_ = true;
        else if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) request->failed_ = true;
        else {
            request->completion_ = status;
            if (status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE) request->bytes_ = length;
        }
        request->changed_.notify_all();
    }
    HINTERNET handle_;
    std::chrono::steady_clock::time_point deadline_;
    std::mutex mutex_;
    std::condition_variable changed_;
    DWORD completion_ = 0, bytes_ = 0;
    bool bound_ = false, failed_ = false, closed_ = false;
};

std::optional<std::string> http_post(const ProgrammingBrokerConfig& config, std::string_view route, std::string_view body,
    std::optional<std::chrono::steady_clock::time_point> expires = std::nullopt) {
    const auto deadline = expires.value_or(std::chrono::steady_clock::now() + std::chrono::seconds(45));
    const auto destination = endpoint(config.url);
    if (!destination || std::chrono::steady_clock::now() >= deadline ||
        body.size() > (route == "/v1/grading" ? kMaximumGradingRequestBytes : kMaximumHttpRequestBytes)) return std::nullopt;
    HttpHandle session(WinHttpOpen(L"NSTU-CS/1", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC));
    if (!session.handle || !WinHttpSetTimeouts(session.handle, 5000, 5000, 5000, 30000)) return std::nullopt;
    HttpHandle connection(WinHttpConnect(session.handle, destination->host.c_str(), destination->port, 0));
    const auto path = wide(route);
    HttpHandle request(connection.handle ? WinHttpOpenRequest(connection.handle, L"POST", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, destination->secure ? WINHTTP_FLAG_SECURE : 0) : nullptr);
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!request.handle || !WinHttpSetOption(request.handle, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect))) return std::nullopt;
    const auto headers = L"Content-Type: application/json\r\nAuthorization: Bearer " + wide(config.token) + L"\r\n";
    std::string request_body(body);
    std::array<char, 8192> read_buffer{};
    AsyncHttpRequest async(std::exchange(request.handle, nullptr), deadline);
    if (!async.run(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE, [&] {
            return WinHttpSendRequest(async.handle(), headers.c_str(), static_cast<DWORD>(headers.size()), request_body.data(),
                static_cast<DWORD>(request_body.size()), static_cast<DWORD>(request_body.size()), async.context());
        }) || !async.run(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE, [&] { return WinHttpReceiveResponse(async.handle(), nullptr); })) return std::nullopt;
    DWORD status = 0;
    DWORD status_bytes = sizeof(status);
    if (!WinHttpQueryHeaders(async.handle(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_bytes, WINHTTP_NO_HEADER_INDEX) || status != 200) return std::nullopt;
    std::string result;
    for (;;) {
        if (!async.run(WINHTTP_CALLBACK_STATUS_READ_COMPLETE, [&] {
                return WinHttpReadData(async.handle(), read_buffer.data(), static_cast<DWORD>(read_buffer.size()), nullptr);
            })) return std::nullopt;
        const auto read = async.bytes();
        if (read > read_buffer.size() || read > exam::kMaximumExamPayloadBytes - result.size()) return std::nullopt;
        if (read == 0) return result;
        result.append(read_buffer.data(), read);
    }
}

std::string environment(const wchar_t* name, DWORD maximum) {
    const auto length = GetEnvironmentVariableW(name, nullptr, 0);
    if (length == 0 || length > maximum) return {};
    std::wstring value(length, L'\0');
    const auto copied = GetEnvironmentVariableW(name, value.data(), length);
    if (copied == 0 || copied >= length) return {};
    const auto bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(copied), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string result(static_cast<std::size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(copied), result.data(), bytes, nullptr, nullptr) != bytes) return {};
    return result;
}

bool route_allowed(std::string_view route) {
    return route == "/v1/programming" || route == "/v1/sessions" || route == "/v1/begin" ||
        route == "/v1/stop" || route == "/v1/validate-context" || route == "/v1/packages" || route == "/v1/grading" ||
        route == "/v1/contest-rooms/create" || route == "/v1/contest-rooms/members" ||
        route == "/v1/contest-rooms/begin" || route == "/v1/contest-rooms/stop" ||
        route == "/v1/contest-rooms/state" || route == "/v1/contest-rooms/standings";
}

bool request_id_allowed(const exam::StateRequest& context, std::string_view id) {
    exam::programming::Request request;
    request.context = context;
    request.operation = exam::programming::Operation::ready;
    request.request_id = id;
    return exam::programming::validate_request(request);
}

constexpr std::uint64_t kSafeInteger = (1ULL << 53) - 1;
struct RoomJson {
    enum class Kind { text, number, boolean, object, array } kind = Kind::text;
    std::string text;
    std::map<std::string, RoomJson, std::less<>> fields;
    std::vector<RoomJson> items;
};

// The existing flat programming reader's strict scalar rules, with bounded nesting.
class RoomJsonReader {
public:
    explicit RoomJsonReader(std::string_view text, std::size_t maximum = kMaximumHttpRequestBytes) : text_(text), maximum_(maximum) {}
    bool parse(RoomJson& result) {
        if (text_.empty() || text_.size() > maximum_ || !value(result, 0)) return false;
        whitespace(); return position_ == text_.size();
    }
private:
    void whitespace() {
        while (position_ < text_.size() && (text_[position_] == ' ' || text_[position_] == '\t' ||
            text_[position_] == '\r' || text_[position_] == '\n')) ++position_;
    }
    bool take(char c) {
        whitespace();
        if (position_ == text_.size() || text_[position_] != c) return false;
        ++position_; return true;
    }
    bool hex4(std::uint32_t& point) {
        if (text_.size() - position_ < 4) return false;
        point = 0;
        for (unsigned i = 0; i < 4; ++i) {
            const char c = text_[position_++]; point <<= 4u;
            if (c >= '0' && c <= '9') point |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') point |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') point |= static_cast<unsigned>(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool string(std::string& result) {
        if (!take('"')) return false;
        while (position_ < text_.size() && result.size() <= 65536) {
            const auto c = static_cast<unsigned char>(text_[position_++]);
            if (c == '"') return (result.empty() || !wide(result).empty()) &&
                std::all_of(result.begin(), result.end(), [](unsigned char b) {
                    return b != 127 && (b >= 32 || b == '\t' || b == '\n' || b == '\r');
                });
            if (c < 32) return false;
            if (c != '\\') { result.push_back(static_cast<char>(c)); continue; }
            if (position_ == text_.size()) return false;
            switch (text_[position_++]) {
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            case '/': result.push_back('/'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'u': {
                std::uint32_t point = 0;
                if (!hex4(point)) return false;
                if (point >= 0xd800u && point <= 0xdbffu) {
                    if (text_.substr(position_, 2) != "\\u") return false;
                    position_ += 2; std::uint32_t low = 0;
                    if (!hex4(low) || low < 0xdc00u || low > 0xdfffu) return false;
                    point = 0x10000u + ((point - 0xd800u) << 10u) + low - 0xdc00u;
                } else if (point >= 0xdc00u && point <= 0xdfffu) return false;
                if (point <= 0x7fu) result.push_back(static_cast<char>(point));
                else if (point <= 0x7ffu) {
                    result.push_back(static_cast<char>(0xc0u | (point >> 6u)));
                    result.push_back(static_cast<char>(0x80u | (point & 0x3fu)));
                } else if (point <= 0xffffu) {
                    result.push_back(static_cast<char>(0xe0u | (point >> 12u)));
                    result.push_back(static_cast<char>(0x80u | ((point >> 6u) & 0x3fu)));
                    result.push_back(static_cast<char>(0x80u | (point & 0x3fu)));
                } else {
                    result.push_back(static_cast<char>(0xf0u | (point >> 18u)));
                    result.push_back(static_cast<char>(0x80u | ((point >> 12u) & 0x3fu)));
                    result.push_back(static_cast<char>(0x80u | ((point >> 6u) & 0x3fu)));
                    result.push_back(static_cast<char>(0x80u | (point & 0x3fu)));
                }
                break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool value(RoomJson& result, unsigned depth) {
        if (depth > 8 || ++nodes_ > 20000) return false;
        whitespace();
        if (position_ == text_.size()) return false;
        if (text_[position_] == '"') return string(result.text);
        if (take('{')) {
            result.kind = RoomJson::Kind::object;
            if (take('}')) return true;
            do {
                std::string key; RoomJson child;
                if (result.fields.size() >= 40 || !string(key) || key.size() > 64 || !take(':') ||
                    !value(child, depth + 1) || !result.fields.emplace(std::move(key), std::move(child)).second) return false;
                if (take('}')) return true;
            } while (take(','));
            return false;
        }
        if (take('[')) {
            result.kind = RoomJson::Kind::array;
            if (take(']')) return true;
            do {
                RoomJson child;
                if (result.items.size() >= 64 || !value(child, depth + 1)) return false;
                result.items.push_back(std::move(child));
                if (take(']')) return true;
            } while (take(','));
            return false;
        }
        for (const std::string_view literal : {"true", "false"}) {
            if (text_.substr(position_, literal.size()) == literal) {
                result.kind = RoomJson::Kind::boolean; result.text = literal;
                position_ += literal.size(); return true;
            }
        }
        result.kind = RoomJson::Kind::number;
        const auto begin = position_;
        while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
        if (begin == position_ || (text_[begin] == '0' && position_ != begin + 1)) return false;
        result.text = text_.substr(begin, position_ - begin); return true;
    }
    std::string_view text_; std::size_t maximum_; std::size_t position_ = 0; unsigned nodes_ = 0;
};

bool fields(const RoomJson& object, std::string_view allowed, std::size_t count) {
    return object.kind == RoomJson::Kind::object && object.fields.size() == count &&
        std::all_of(object.fields.begin(), object.fields.end(), [&](const auto& field) {
            return field.first.find('|') == std::string::npos && allowed.find("|" + field.first + "|") != std::string_view::npos;
        });
}
const RoomJson* field(const RoomJson& object, std::string_view key) {
    const auto it = object.fields.find(key); return it == object.fields.end() ? nullptr : &it->second;
}
bool text(const RoomJson& object, std::string_view key, std::string& result, std::size_t maximum, bool empty = false) {
    const auto* v = field(object, key);
    if (!v || v->kind != RoomJson::Kind::text || v->text.size() > maximum || (!empty && v->text.empty())) return false;
    result = v->text; return true;
}
bool number(const RoomJson& object, std::string_view key, std::uint64_t& result,
            std::uint64_t minimum = 0, std::uint64_t maximum = kSafeInteger) {
    const auto* v = field(object, key);
    if (!v || v->kind != RoomJson::Kind::number) return false;
    const auto parsed = std::from_chars(v->text.data(), v->text.data() + v->text.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == v->text.data() + v->text.size() && result >= minimum && result <= maximum;
}
bool boolean(const RoomJson& object, std::string_view key, bool& result) {
    const auto* v = field(object, key);
    if (!v || v->kind != RoomJson::Kind::boolean) return false;
    result = v->text == "true"; return true;
}
bool hex_id(std::string_view value, std::size_t size) {
    return value.size() == size && value.find_first_not_of("0123456789abcdef") == std::string_view::npos &&
        value.find_first_not_of('0') != std::string_view::npos;
}
bool identifier(std::string_view value) {
    if (value.empty() || value.size() > 96 || value.front() == '.' || value.front() == '-') return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char b) {
        return (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || (b >= '0' && b <= '9') || b == '_' || b == '.' || b == '-';
    });
}
template<typename Array> std::string hex(const Array& bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : bytes) {
        const auto b = std::to_integer<unsigned>(byte); result.push_back(digits[b >> 4u]); result.push_back(digits[b & 15u]);
    }
    return result;
}
template<typename Array> bool parse_hex(std::string_view value, Array& result) {
    if (!hex_id(value, result.size() * 2)) return false;
    for (std::size_t i = 0; i < result.size(); ++i) {
        unsigned b = 0;
        const auto parsed = std::from_chars(value.data() + i * 2, value.data() + i * 2 + 2, b, 16);
        if (parsed.ec != std::errc{}) return false;
        result[i] = static_cast<std::byte>(b);
    }
    return true;
}
std::string quote(std::string_view value) {
    std::string result = "\"";
    for (const char c : value) {
        if (c == '"' || c == '\\') { result.push_back('\\'); result.push_back(c); }
        else if (c == '\n') result += "\\n";
        else if (c == '\r') result += "\\r";
        else if (c == '\t') result += "\\t";
        else result.push_back(c);
    }
    return result + '"';
}
bool parse_context(const RoomJson& object, exam::StateRequest& context) {
    std::string digest, client, session;
    return fields(object, "|packageId||packageDigestHex||clientIdHex||sessionIdHex||candidateId|", 5) &&
        text(object, "packageId", context.package_id, 96) && identifier(context.package_id) &&
        text(object, "candidateId", context.candidate_id, 128) &&
        text(object, "packageDigestHex", digest, 64) && parse_hex(digest, context.package_digest) &&
        text(object, "clientIdHex", client, 32) && parse_hex(client, context.client_id) &&
        text(object, "sessionIdHex", session, 32) && parse_hex(session, context.session_id) &&
        !exam::encode_state_request(context).empty();
}
bool valid_pins(const ImmutableRoomState& pins) {
    return hex_id(pins.room_id, 32) && hex_id(pins.round_id, 32) && identifier(pins.owner_id) && identifier(pins.package_id) &&
        hex_id(hex(pins.package_digest), 64) && hex_id(hex(pins.grading_digest), 64) &&
        (pins.runtime_identity.empty() || hex_id(pins.runtime_identity, 64)) &&
        (pins.publication_policy == RoomPublicationPolicy::teacher_only || pins.publication_policy == RoomPublicationPolicy::live) &&
        pins.duration_seconds >= 60 && pins.duration_seconds <= 86400;
}
std::string room_body(const ImmutableRoomState& pins) {
    if (!valid_pins(pins)) return {};
    return "{\"programmingVersion\":1,\"roomId\":" + quote(pins.room_id) + ",\"roundId\":" + quote(pins.round_id) + '}';
}
std::optional<RoomState> parse_room(std::string_view json, const ImmutableRoomState& expected,
                                   std::string_view request_id = {}, bool learn_runtime = false) {
    RoomJson object; RoomState result; auto& pins = result.immutable;
    const auto allowed = "|programmingVersion||roomId||roundId||ownerId||packageId||packageDigestHex||gradingDigestHex||runtimeIdentity|"
        "|publicationPolicy||durationSeconds||state||revision||serverStartUnixMilliseconds||deadlineUnixMilliseconds||serverTime||members|";
    std::string package, grading, policy, phase, reply_id;
    std::uint64_t version = 0, duration = 0;
    if (!valid_pins(expected) || !RoomJsonReader(json).parse(object) ||
        !fields(object, std::string(allowed) + (request_id.empty() ? "" : "|requestId|"), request_id.empty() ? 16 : 17) ||
        !number(object, "programmingVersion", version, 1, 1) ||
        !text(object, "roomId", pins.room_id, 32) || pins.room_id != expected.room_id ||
        !text(object, "roundId", pins.round_id, 32) || pins.round_id != expected.round_id ||
        !text(object, "ownerId", pins.owner_id, 96) || pins.owner_id != expected.owner_id ||
        !text(object, "packageId", pins.package_id, 96) || pins.package_id != expected.package_id ||
        !text(object, "packageDigestHex", package, 64) || !parse_hex(package, pins.package_digest) || pins.package_digest != expected.package_digest ||
        !text(object, "gradingDigestHex", grading, 64) || !parse_hex(grading, pins.grading_digest) || pins.grading_digest != expected.grading_digest ||
        !text(object, "runtimeIdentity", pins.runtime_identity, 64, true) ||
        (!pins.runtime_identity.empty() && !hex_id(pins.runtime_identity, 64)) ||
        (!(learn_runtime && expected.runtime_identity.empty()) && pins.runtime_identity != expected.runtime_identity) ||
        !text(object, "publicationPolicy", policy, 16) ||
        policy != (expected.publication_policy == RoomPublicationPolicy::live ? "live" : "teacher_only") ||
        !number(object, "durationSeconds", duration, 60, 86400) || duration != expected.duration_seconds ||
        !text(object, "state", phase, 16) || !number(object, "revision", result.revision, 1) ||
        !number(object, "serverStartUnixMilliseconds", result.server_start) || !number(object, "deadlineUnixMilliseconds", result.deadline) ||
        !number(object, "serverTime", result.server_time, 1, kSafeInteger - 86400000) ||
        (!request_id.empty() && (!text(object, "requestId", reply_id, 96) || reply_id != request_id))) return std::nullopt;
    pins.duration_seconds = static_cast<std::uint32_t>(duration); pins.publication_policy = expected.publication_policy;
    if (phase == "Draft") result.phase = RoomPhase::draft;
    else if (phase == "Joining") result.phase = RoomPhase::joining;
    else if (phase == "Prepared") result.phase = RoomPhase::prepared;
    else if (phase == "Running") result.phase = RoomPhase::running;
    else if (phase == "Closed") result.phase = RoomPhase::closed;
    else return std::nullopt;
    const auto* members = field(object, "members");
    if (!members || members->kind != RoomJson::Kind::array) return std::nullopt;
    for (const auto& member : members->items) {
        RoomMember parsed; bool approved = false; const auto* context = field(member, "context");
        if (!fields(member, "|context||displayName||prepared||approved||memberVersion|", 5) || !context ||
            !parse_context(*context, parsed.context) || parsed.context.package_id != pins.package_id ||
            parsed.context.package_digest != pins.package_digest || !text(member, "displayName", parsed.display_name, 64) ||
            !boolean(member, "prepared", parsed.prepared) || !boolean(member, "approved", approved) || !approved ||
            !number(member, "memberVersion", parsed.member_version, 1)) return std::nullopt;
        for (const auto& old : result.members) {
            if (old.context.candidate_id == parsed.context.candidate_id || old.context.client_id == parsed.context.client_id ||
                old.context.session_id == parsed.context.session_id) return std::nullopt;
        }
        result.members.push_back(std::move(parsed));
    }
    if ((result.server_start == 0) != (result.deadline == 0) ||
        (result.server_start != 0 && (result.deadline <= result.server_start || result.deadline - result.server_start != duration * 1000))) return std::nullopt;
    if (result.phase != RoomPhase::running && result.phase != RoomPhase::closed && result.server_start != 0) return std::nullopt;
    if (result.phase == RoomPhase::draft && !result.members.empty()) return std::nullopt;
    if (result.phase == RoomPhase::prepared || result.phase == RoomPhase::running) {
        if (result.members.empty() || pins.runtime_identity.empty() ||
            !std::all_of(result.members.begin(), result.members.end(), [](const auto& member) { return member.prepared; })) return std::nullopt;
    }
    if (result.phase == RoomPhase::running && (result.server_start == 0 || result.server_time < result.server_start || result.server_time >= result.deadline)) return std::nullopt;
    return result;
}

std::optional<StandingsSnapshot> parse_standings(std::string_view json, const ImmutableRoomState& expected) {
    RoomJson object; StandingsSnapshot result; std::string freshness;
    if (!valid_pins(expected) || !RoomJsonReader(json).parse(object) ||
        !fields(object, "|roomId||roundId||revision||generatedTime||freshness||rows|", 6) ||
        !text(object, "roomId", result.room_id, 32) || result.room_id != expected.room_id ||
        !text(object, "roundId", result.round_id, 32) || result.round_id != expected.round_id ||
        !number(object, "revision", result.revision, 1) || !number(object, "generatedTime", result.generated_time, 1) ||
        !text(object, "freshness", freshness, 16)) return std::nullopt;
    if (freshness == "current") result.freshness = StandingsFreshness::current;
    else if (freshness == "pending") result.freshness = StandingsFreshness::pending;
    else if (freshness == "unavailable") result.freshness = StandingsFreshness::unavailable;
    else return std::nullopt;
    const auto* rows = field(object, "rows");
    if (!rows || rows->kind != RoomJson::Kind::array) return std::nullopt;
    for (const auto& row : rows->items) {
        StandingsRow parsed; std::uint64_t rank = 0, solved = 0;
        if (!fields(row, "|rank||displayName||solved||penaltyMinutes|", 4) ||
            !number(row, "rank", rank, 1, 64) || rank > result.rows.size() + 1 ||
            !text(row, "displayName", parsed.display_name, 64) || !number(row, "solved", solved, 0, 64) ||
            !number(row, "penaltyMinutes", parsed.penalty_minutes)) return std::nullopt;
        parsed.rank = static_cast<std::uint32_t>(rank); parsed.solved = static_cast<std::uint32_t>(solved);
        if (!result.rows.empty()) {
            const auto& previous = result.rows.back();
            const bool tied = previous.solved == parsed.solved && previous.penalty_minutes == parsed.penalty_minutes;
            if (parsed.solved > previous.solved || (parsed.solved == previous.solved && parsed.penalty_minutes < previous.penalty_minutes) ||
                parsed.rank != (tied ? previous.rank : result.rows.size() + 1)) return std::nullopt;
        }
        result.rows.push_back(std::move(parsed));
    }
    return result;
}

bool member_context_allowed(const ImmutableRoomState& pins, const exam::StateRequest& context) {
    return valid_pins(pins) && identifier(context.package_id) && context.package_id == pins.package_id &&
        context.package_digest == pins.package_digest && !exam::encode_state_request(context).empty();
}
bool same_members(const RoomState& expected, const RoomState& reply) {
    return expected.members.size() == reply.members.size() && std::all_of(expected.members.begin(), expected.members.end(), [&](const auto& member) {
        return std::any_of(reply.members.begin(), reply.members.end(), [&](const auto& actual) {
            return exam::programming::context_json(member.context) == exam::programming::context_json(actual.context) &&
                member.display_name == actual.display_name && member.prepared == actual.prepared && member.member_version == actual.member_version;
        });
    });
}
std::string member_body(const RoomState& expected, const exam::StateRequest& context, std::string_view action,
                        std::uint64_t member_version, std::string_view name = {}) {
    if (!member_context_allowed(expected.immutable, context) || expected.revision == 0 || expected.revision > kSafeInteger) return {};
    if (action == "approve") {
        if (name.empty() || name.size() > 64 || wide(name).empty() ||
            std::any_of(name.begin(), name.end(), [](unsigned char b) { return b == 127 || (b < 32 && b != '\t' && b != '\n' && b != '\r'); })) return {};
    } else if (member_version == 0 || member_version >= kSafeInteger ||
        std::none_of(expected.members.begin(), expected.members.end(), [&](const auto& member) {
            return exam::programming::context_json(member.context) == exam::programming::context_json(context) &&
                member.member_version == member_version;
        })) return {};
    auto body = room_body(expected.immutable); body.pop_back();
    body += ",\"action\":" + quote(action) + ",\"expectedRevision\":" + std::to_string(expected.revision) +
        ",\"context\":" + exam::programming::context_json(context);
    if (action == "approve") body += ",\"displayName\":" + quote(name);
    else body += ",\"memberVersion\":" + std::to_string(member_version);
    if (action == "prepare") body += ",\"packageDigestHex\":" + quote(hex(expected.immutable.package_digest));
    return body + '}';
}

std::optional<RoomState> member_reply(std::string_view json, const RoomState& expected,
    const exam::StateRequest& context, std::string_view action, std::string_view name = {}) {
    auto result = parse_room(json, expected.immutable);
    if (!result || expected.revision >= kSafeInteger || result->revision != expected.revision + 1) return std::nullopt;
    const auto found = std::find_if(result->members.begin(), result->members.end(), [&](const auto& member) {
        return exam::programming::context_json(member.context) == exam::programming::context_json(context);
    });
    if (action == "remove") return found == result->members.end() ? result : std::nullopt;
    if (found == result->members.end()) return std::nullopt;
    if (action == "approve") return found->display_name == name && !found->prepared && found->member_version == 1 ? result : std::nullopt;
    const auto old = std::find_if(expected.members.begin(), expected.members.end(), [&](const auto& member) {
        return exam::programming::context_json(member.context) == exam::programming::context_json(context);
    });
    return old != expected.members.end() && found->prepared && found->member_version == old->member_version + 1 &&
        found->display_name == old->display_name ? result : std::nullopt;
}

bool registration_material(std::string_view json, std::string_view package_id, bool grading) {
    RoomJson object;
    if (!RoomJsonReader(json, grading ? kMaximumGradingRequestBytes : kMaximumHttpRequestBytes).parse(object)) return false;
    const RoomJson* problems = &object;
    std::string value; std::uint64_t integer = 0;
    if (!grading) {
        if (!fields(object, "|id||title||subject||candidate||durationSeconds||contentMode||programmingVersion||sessionMode||languages||questions||allowedOrigins||documents|", object.fields.size()) ||
            !text(object, "id", value, 96) || value != package_id || !text(object, "title", value, 240) ||
            !number(object, "durationSeconds", integer, 60, 86400) || !number(object, "programmingVersion", integer, 1, 1) ||
            !text(object, "contentMode", value, 32) || value != "computer_science" ||
            !text(object, "sessionMode", value, 16) || (value != "exam" && value != "contest" && value != "practice")) return false;
        for (const auto key : {"subject", "candidate"}) {
            if (field(object, key) && !text(object, key, value, std::string_view(key) == "subject" ? 120 : 160, true)) return false;
        }
        for (const auto key : {"allowedOrigins", "documents"}) {
            const auto* v = field(object, key);
            if (v && (v->kind != RoomJson::Kind::array || !v->items.empty())) return false;
        }
        const auto* languages = field(object, "languages");
        if (!languages || languages->kind != RoomJson::Kind::array || languages->items.size() != 2 ||
            languages->items[0].kind != RoomJson::Kind::text || languages->items[0].text != "cpp20" ||
            languages->items[1].kind != RoomJson::Kind::text || languages->items[1].text != "python3") return false;
        problems = field(object, "questions");
    }
    if (!problems || problems->kind != RoomJson::Kind::array || problems->items.empty()) return false;
    std::vector<std::string> ids;
    for (const auto& problem : problems->items) {
        if (!fields(problem, grading ? "|id||revision||cases|" :
            "|id||type||revision||prompt||instruction||points||inputDescription||outputDescription||constraints||explanation||samples||timeLimitMs||memoryLimitMiB|",
            grading ? 3 : problem.fields.size()) || !text(problem, "id", value, 96) || !identifier(value) ||
            std::find(ids.begin(), ids.end(), value) != ids.end() || !number(problem, "revision", integer, 1, 4294967295ULL)) return false;
        ids.push_back(value);
        if (!grading) {
            if (!text(problem, "type", value, 16) || value != "programming" || !text(problem, "prompt", value, 8000) ||
                !number(problem, "timeLimitMs", integer, 100, 10000) || !number(problem, "memoryLimitMiB", integer, 16, 512) ||
                (field(problem, "points") && !number(problem, "points", integer, 0, 1000))) return false;
            for (const auto key : {"instruction", "inputDescription", "outputDescription", "constraints", "explanation"}) {
                if (field(problem, key) && !text(problem, key, value, 4000, true)) return false;
            }
        }
        const auto* cases = field(problem, grading ? "cases" : "samples");
        if (!cases || cases->kind != RoomJson::Kind::array || cases->items.empty() || (!grading && cases->items.size() > 8)) return false;
        std::vector<std::string> sample_ids;
        for (const auto& item : cases->items) {
            if (!fields(item, grading ? "|input||output|" : "|id||input||output|", grading ? 2 : 3) ||
                !text(item, "input", value, grading ? 65536 : 4096, true) || !text(item, "output", value, grading ? 65536 : 4096, true)) return false;
            if (!grading) {
                if (!text(item, "id", value, 96) || !identifier(value) || std::find(sample_ids.begin(), sample_ids.end(), value) != sample_ids.end()) return false;
                sample_ids.push_back(value);
            }
        }
    }
    return true;
}

bool registration_reply(std::string_view json, std::string_view package_id,
                         const security::Sha256Digest& digest, bool grading) {
    RoomJson object; std::string status, returned_digest, returned_package;
    return RoomJsonReader(json).parse(object) && fields(object,
        grading ? "|status||gradingDigestHex|" : "|status||packageId||packageDigestHex|", grading ? 2 : 3) &&
        text(object, "status", status, 16) && status == "registered" &&
        text(object, grading ? "gradingDigestHex" : "packageDigestHex", returned_digest, 64) && returned_digest == hex(digest) &&
        (grading || (text(object, "packageId", returned_package, 96) && returned_package == package_id));
}

} // namespace

ProgrammingBrokerConfig programming_broker_environment() {
    return {environment(L"NSTU_CS_BROKER_URL", 2049), environment(L"NSTU_CS_BROKER_TOKEN", 257)};
}

bool validate_programming_broker_config(const ProgrammingBrokerConfig& config) {
    return endpoint(config.url).has_value() && config.token.size() >= 32 && config.token.size() <= 256 &&
        std::all_of(config.token.begin(), config.token.end(), [](unsigned char byte) { return byte >= 33u && byte < 127u; });
}

struct ProgrammingBroker::Impl {
    struct Work {
        std::string route;
        std::string body;
        std::function<void(std::optional<std::string>)> completion;
        std::optional<exam::StateRequest> recovery_context = std::nullopt;
        bool control = false;
        std::optional<std::chrono::steady_clock::time_point> expires = std::nullopt;
    };
    ProgrammingBrokerConfig config;
    Transport transport;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Work> queue;
    std::deque<Work> control_queue;
    std::deque<Work> grading_queue;
    bool grading_busy = false;
    std::size_t queued_bytes = 0;
    std::atomic_bool running = false;
    std::thread workers[4];

    Impl(ProgrammingBrokerConfig value, Transport supplied) : config(std::move(value)), transport(std::move(supplied)) {
        if (!validate_programming_broker_config(config)) return;
        running = true;
        try {
            workers[0] = std::thread([this] { loop(true); });
            for (std::size_t index = 1; index < 3; ++index) workers[index] = std::thread([this] { loop(false); });
            workers[3] = std::thread([this] { loop(false, true); });
        } catch (...) {
            running = false;
            changed.notify_all();
            for (auto& worker : workers) if (worker.joinable()) worker.join();
            throw;
        }
    }
    bool enqueue(Work work) {
        const bool grading = work.route == "/v1/grading";
        if (!route_allowed(work.route) || work.body.empty() ||
            work.body.size() > (grading ? kMaximumGradingRequestBytes : kMaximumHttpRequestBytes)) return false;
        std::scoped_lock lock(mutex);
        if (grading) {
            if (!running || grading_busy) return false;
            grading_queue.push_back(std::move(work));
            grading_busy = true;
            changed.notify_all();
            return true;
        }
        if (!running || queue.size() + control_queue.size() >= kMaximumQueuedRequests ||
            work.body.size() > kMaximumQueuedBytes - queued_bytes) return false;
        const auto bytes = work.body.size();
        (work.control ? control_queue : queue).push_back(std::move(work));
        queued_bytes += bytes;
        changed.notify_all();
        return true;
    }
    void loop(bool control, bool grading = false) {
        for (;;) {
            Work work;
            {
                std::unique_lock lock(mutex);
                auto& selected = grading ? grading_queue : control ? control_queue : queue;
                changed.wait(lock, [&] { return !running || !selected.empty(); });
                if (selected.empty()) return;
                work = std::move(selected.front());
                selected.pop_front();
                if (!grading) queued_bytes -= work.body.size();
            }
            std::optional<std::string> response;
            try {
                bool known = !work.expires || std::chrono::steady_clock::now() < *work.expires;
                if (known && work.recovery_context) {
                    auto body = exam::programming::context_json(*work.recovery_context);
                    body.pop_back();
                    body += ",\"programmingVersion\":1}";
                    const auto validation = transport ? transport("/v1/validate-context", body)
                                                      : http_post(config, "/v1/validate-context", body, work.expires);
                    known = validation && exam::programming::validate_known_context_json(*validation, *work.recovery_context);
                }
                if (known) response = transport ? transport(work.route, work.body) : http_post(config, work.route, work.body, work.expires);
            } catch (...) {}
            if (response && response->size() > exam::kMaximumExamPayloadBytes) response.reset();
            if (grading) {
                std::scoped_lock lock(mutex);
                grading_busy = false;
            }
            try { work.completion(std::move(response)); } catch (...) {}
        }
    }
    std::optional<std::string> post(std::string_view route, std::string body) {
        auto promise = std::make_shared<std::promise<std::optional<std::string>>>();
        auto result = promise->get_future();
        if (!enqueue({std::string(route), std::move(body), [promise](auto response) { promise->set_value(std::move(response)); },
                      std::nullopt, true, std::chrono::steady_clock::now() + std::chrono::seconds(45)})) return std::nullopt;
        if (result.wait_for(std::chrono::seconds(45)) != std::future_status::ready) return std::nullopt;
        return result.get();
    }
    void stop() noexcept {
        std::deque<Work> pending;
        std::deque<Work> pending_control;
        std::deque<Work> pending_grading;
        {
            std::scoped_lock lock(mutex);
            running = false;
            pending.swap(queue);
            pending_control.swap(control_queue);
            pending_grading.swap(grading_queue);
            queued_bytes = 0;
            changed.notify_all();
        }
        for (auto& work : pending) { try { work.completion(std::nullopt); } catch (...) {} }
        for (auto& work : pending_control) { try { work.completion(std::nullopt); } catch (...) {} }
        for (auto& work : pending_grading) { try { work.completion(std::nullopt); } catch (...) {} }
        for (auto& worker : workers) if (worker.joinable()) worker.join();
        std::fill(config.token.begin(), config.token.end(), '\0');
    }
};

ProgrammingBroker::ProgrammingBroker(ProgrammingBrokerConfig config, Transport transport)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(transport))) {}
ProgrammingBroker::~ProgrammingBroker() { stop(); }
bool ProgrammingBroker::configured() const noexcept { return impl_ && impl_->running; }
void ProgrammingBroker::stop() noexcept { if (impl_) impl_->stop(); }

bool ProgrammingBroker::submit(exam::programming::Request request, Completion completion, bool recovery_only) {
    if (!completion || !exam::programming::validate_request(request)) return false;
    using exam::programming::Operation;
    if (recovery_only && request.operation != Operation::draft && request.operation != Operation::submit &&
        request.operation != Operation::finalize && request.operation != Operation::recover) return false;
    auto body = exam::programming::request_json(request);
    const bool control = request.operation == Operation::ready || request.operation == Operation::cancel;
    const auto recovery_context = recovery_only ? std::optional{request.context} : std::nullopt;
    if (recovery_only) {
        body.pop_back();
        body += ",\"recoveryOnly\":true}";
    }
    return impl_->enqueue({"/v1/programming", std::move(body),
        [request = std::move(request), completion = std::move(completion)](std::optional<std::string> json) {
            const auto validated = json ? exam::programming::parse_response_json(*json, &request) : std::nullopt;
            completion(validated ? validated->json : exam::programming::unavailable_response_json(request, "broker_unavailable"));
        }, recovery_context, control});
}

bool ProgrammingBroker::prepare_session(const exam::StateRequest& context, std::string_view mode, std::string_view grading_digest) {
    if ((mode != "exam" && mode != "contest" && mode != "practice") || grading_digest.size() != 64 ||
        !std::all_of(grading_digest.begin(), grading_digest.end(), [](char value) { return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'); })) return false;
    auto body = exam::programming::context_json(context);
    if (body.empty()) return false;
    body.pop_back();
    body += ",\"programmingVersion\":1,\"sessionMode\":\"" + std::string(mode) + "\",\"gradingDigestHex\":\"" + std::string(grading_digest) + "\"}";
    const auto response = impl_->post("/v1/sessions", std::move(body));
    return response && exam::programming::validate_prepared_json(*response, context);
}

bool ProgrammingBroker::ready(const exam::StateRequest& context) {
    exam::programming::Request request;
    request.context = context;
    request.operation = exam::programming::Operation::ready;
    request.request_id = "native-ready";
    const auto response = impl_->post("/v1/programming", exam::programming::request_json(request));
    const auto validated = response ? exam::programming::parse_response_json(*response, &request) : std::nullopt;
    return validated && validated->runner_ready && validated->status == "ready";
}

std::optional<exam::programming::LifecycleReply> ProgrammingBroker::begin_session(
    const exam::StateRequest& context, std::string_view request_id, std::uint32_t duration_seconds) {
    if (duration_seconds < 60 || duration_seconds > 86400 || !request_id_allowed(context, request_id)) return std::nullopt;
    auto body = exam::programming::context_json(context);
    if (body.empty()) return std::nullopt;
    body.pop_back();
    body += ",\"programmingVersion\":1,\"requestId\":\"" + std::string(request_id) +
            "\",\"durationSeconds\":" + std::to_string(duration_seconds) + '}';
    const auto response = impl_->post("/v1/begin", std::move(body));
    return response ? exam::programming::parse_lifecycle_json(*response, context, request_id, true) : std::nullopt;
}

bool ProgrammingBroker::stop_session(const exam::StateRequest& context, std::string_view request_id) {
    if (!request_id_allowed(context, request_id)) return false;
    auto body = exam::programming::context_json(context);
    if (body.empty()) return false;
    body.pop_back();
    body += ",\"programmingVersion\":1,\"requestId\":\"" + std::string(request_id) + "\"}";
    const auto response = impl_->post("/v1/stop", std::move(body));
    return response && exam::programming::parse_lifecycle_json(*response, context, request_id, false).has_value();
}

bool ProgrammingBroker::register_package(std::string_view package_id,
    const security::Sha256Digest& package_digest, std::string_view manifest_json) {
    if (!identifier(package_id) || !hex_id(hex(package_digest), 64) || !registration_material(manifest_json, package_id, false)) return false;
    const auto response = impl_->post("/v1/packages", "{\"programmingVersion\":1,\"packageId\":" + quote(package_id) +
        ",\"packageDigestHex\":" + quote(hex(package_digest)) + ",\"manifest\":" + std::string(manifest_json) + '}');
    return response && registration_reply(*response, package_id, package_digest, false);
}

bool ProgrammingBroker::register_grading(std::string_view package_id,
    const security::Sha256Digest& package_digest, const security::Sha256Digest& grading_digest, std::string_view problems_json) {
    if (!identifier(package_id) || !hex_id(hex(package_digest), 64) || !hex_id(hex(grading_digest), 64) ||
        !registration_material(problems_json, package_id, true)) return false;
    const auto response = impl_->post("/v1/grading", "{\"programmingVersion\":1,\"packageId\":" + quote(package_id) +
        ",\"packageDigestHex\":" + quote(hex(package_digest)) + ",\"gradingDigestHex\":" + quote(hex(grading_digest)) +
        ",\"problems\":" + std::string(problems_json) + '}');
    return response && registration_reply(*response, package_id, grading_digest, true);
}

std::optional<RoomState> ProgrammingBroker::create_room(const ImmutableRoomState& expected) {
    auto body = room_body(expected);
    if (body.empty()) return std::nullopt;
    body.pop_back();
    body += ",\"packageId\":" + quote(expected.package_id) + ",\"packageDigestHex\":" + quote(hex(expected.package_digest)) +
        ",\"gradingDigestHex\":" + quote(hex(expected.grading_digest)) + ",\"publicationPolicy\":" +
        quote(expected.publication_policy == RoomPublicationPolicy::live ? "live" : "teacher_only") + '}';
    const auto response = impl_->post("/v1/contest-rooms/create", std::move(body));
    return response ? parse_room(*response, expected, {}, true) : std::nullopt;
}
std::optional<RoomState> ProgrammingBroker::room_state(const ImmutableRoomState& expected) {
    const auto response = impl_->post("/v1/contest-rooms/state", room_body(expected));
    return response ? parse_room(*response, expected) : std::nullopt;
}
std::optional<RoomState> ProgrammingBroker::approve_member(const RoomState& expected,
    const exam::StateRequest& context, std::string_view display_name) {
    const auto response = impl_->post("/v1/contest-rooms/members", member_body(expected, context, "approve", 0, display_name));
    return response ? member_reply(*response, expected, context, "approve", display_name) : std::nullopt;
}
std::optional<RoomState> ProgrammingBroker::remove_member(const RoomState& expected,
    const exam::StateRequest& context, std::uint64_t member_version) {
    const auto response = impl_->post("/v1/contest-rooms/members", member_body(expected, context, "remove", member_version));
    return response ? member_reply(*response, expected, context, "remove") : std::nullopt;
}
std::optional<RoomState> ProgrammingBroker::prepare_member(const RoomState& expected,
    const exam::StateRequest& context, std::uint64_t member_version) {
    const auto response = impl_->post("/v1/contest-rooms/members", member_body(expected, context, "prepare", member_version));
    return response ? member_reply(*response, expected, context, "prepare") : std::nullopt;
}
std::optional<RoomState> ProgrammingBroker::begin_room(const RoomState& expected, std::string_view request_id) {
    if (!identifier(request_id) || expected.revision == 0 || expected.revision >= kSafeInteger ||
        expected.phase == RoomPhase::running || expected.phase == RoomPhase::closed ||
        expected.server_start != 0 || expected.deadline != 0) return std::nullopt;
    auto body = room_body(expected.immutable); if (body.empty()) return std::nullopt;
    body.pop_back(); body += ",\"expectedRevision\":" + std::to_string(expected.revision) + ",\"requestId\":" + quote(request_id) + '}';
    const auto response = impl_->post("/v1/contest-rooms/begin", std::move(body));
    auto parsed = response ? parse_room(*response, expected.immutable, request_id) : std::nullopt;
    return parsed && parsed->phase == RoomPhase::running && parsed->revision == expected.revision + 1 &&
        parsed->server_start == parsed->server_time && parsed->server_time >= expected.server_time &&
        same_members(expected, *parsed) ? parsed : std::nullopt;
}
std::optional<RoomState> ProgrammingBroker::stop_room(const RoomState& expected, std::string_view request_id) {
    if (!identifier(request_id) || expected.revision == 0 || expected.revision >= kSafeInteger) return std::nullopt;
    auto body = room_body(expected.immutable); if (body.empty()) return std::nullopt;
    body.pop_back(); body += ",\"expectedRevision\":" + std::to_string(expected.revision) + ",\"requestId\":" + quote(request_id) + '}';
    const auto response = impl_->post("/v1/contest-rooms/stop", std::move(body));
    auto parsed = response ? parse_room(*response, expected.immutable, request_id) : std::nullopt;
    return parsed && parsed->phase == RoomPhase::closed && parsed->revision == expected.revision + 1 &&
        parsed->server_start == expected.server_start && parsed->deadline == expected.deadline &&
        parsed->server_time >= expected.server_time && same_members(expected, *parsed) ? parsed : std::nullopt;
}
std::optional<StandingsSnapshot> ProgrammingBroker::room_standings(const ImmutableRoomState& expected) {
    const auto response = impl_->post("/v1/contest-rooms/standings", room_body(expected));
    return response ? parse_standings(*response, expected) : std::nullopt;
}

} // namespace nstu::server
