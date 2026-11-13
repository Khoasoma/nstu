#include "nstu/programming_broker.hpp"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <deque>
#include <future>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

namespace nstu::server {
namespace {

constexpr std::size_t kMaximumQueuedRequests = 64;
constexpr std::size_t kMaximumQueuedBytes = 2u * 1024u * 1024u;
constexpr std::size_t kMaximumHttpRequestBytes = 128u * 1024u;

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

std::optional<std::string> http_post(const ProgrammingBrokerConfig& config, std::string_view route, std::string_view body) {
    const auto destination = endpoint(config.url);
    if (!destination || body.size() > kMaximumHttpRequestBytes) return std::nullopt;
    HttpHandle session(WinHttpOpen(L"NSTU-CS/1", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.handle || !WinHttpSetTimeouts(session.handle, 5000, 5000, 5000, 30000)) return std::nullopt;
    HttpHandle connection(WinHttpConnect(session.handle, destination->host.c_str(), destination->port, 0));
    const auto path = wide(route);
    HttpHandle request(connection.handle ? WinHttpOpenRequest(connection.handle, L"POST", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, destination->secure ? WINHTTP_FLAG_SECURE : 0) : nullptr);
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!request.handle || !WinHttpSetOption(request.handle, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect))) return std::nullopt;
    const auto headers = L"Content-Type: application/json\r\nAuthorization: Bearer " + wide(config.token) + L"\r\n";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
    std::string request_body(body);
    if (!WinHttpSendRequest(request.handle, headers.c_str(), static_cast<DWORD>(headers.size()), request_body.data(),
                            static_cast<DWORD>(request_body.size()), static_cast<DWORD>(request_body.size()), 0) ||
        std::chrono::steady_clock::now() >= deadline || !WinHttpReceiveResponse(request.handle, nullptr) ||
        std::chrono::steady_clock::now() >= deadline) return std::nullopt;
    DWORD status = 0;
    DWORD status_bytes = sizeof(status);
    if (!WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_bytes, WINHTTP_NO_HEADER_INDEX) || status != 200) return std::nullopt;
    std::string result;
    for (;;) {
        if (std::chrono::steady_clock::now() >= deadline) return std::nullopt;
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.handle, &available) ||
            std::chrono::steady_clock::now() >= deadline ||
            available > exam::kMaximumExamPayloadBytes - result.size()) return std::nullopt;
        if (available == 0) return result;
        const auto offset = result.size();
        result.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.handle, result.data() + offset, available, &read) || read == 0 ||
            std::chrono::steady_clock::now() >= deadline) return std::nullopt;
        result.resize(offset + read);
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
        route == "/v1/stop" || route == "/v1/validate-context";
}

bool request_id_allowed(const exam::StateRequest& context, std::string_view id) {
    exam::programming::Request request;
    request.context = context;
    request.operation = exam::programming::Operation::ready;
    request.request_id = id;
    return exam::programming::validate_request(request);
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
    std::size_t queued_bytes = 0;
    std::atomic_bool running = false;
    std::thread workers[3];

    Impl(ProgrammingBrokerConfig value, Transport supplied) : config(std::move(value)), transport(std::move(supplied)) {
        if (!validate_programming_broker_config(config)) return;
        running = true;
        try {
            workers[0] = std::thread([this] { loop(true); });
            for (std::size_t index = 1; index < std::size(workers); ++index) workers[index] = std::thread([this] { loop(false); });
        } catch (...) {
            running = false;
            changed.notify_all();
            for (auto& worker : workers) if (worker.joinable()) worker.join();
            throw;
        }
    }
    bool enqueue(Work work) {
        if (!route_allowed(work.route) || work.body.empty() || work.body.size() > kMaximumHttpRequestBytes) return false;
        std::scoped_lock lock(mutex);
        if (!running || queue.size() + control_queue.size() >= kMaximumQueuedRequests ||
            work.body.size() > kMaximumQueuedBytes - queued_bytes) return false;
        const auto bytes = work.body.size();
        (work.control ? control_queue : queue).push_back(std::move(work));
        queued_bytes += bytes;
        changed.notify_all();
        return true;
    }
    void loop(bool control) {
        for (;;) {
            Work work;
            {
                std::unique_lock lock(mutex);
                auto& selected = control ? control_queue : queue;
                changed.wait(lock, [&] { return !running || !selected.empty(); });
                if (selected.empty()) return;
                work = std::move(selected.front());
                selected.pop_front();
                queued_bytes -= work.body.size();
            }
            std::optional<std::string> response;
            try {
                bool known = !work.expires || std::chrono::steady_clock::now() < *work.expires;
                if (known && work.recovery_context) {
                    auto body = exam::programming::context_json(*work.recovery_context);
                    body.pop_back();
                    body += ",\"programmingVersion\":1}";
                    const auto validation = transport ? transport("/v1/validate-context", body)
                                                      : http_post(config, "/v1/validate-context", body);
                    known = validation && exam::programming::validate_known_context_json(*validation, *work.recovery_context);
                }
                if (known) response = transport ? transport(work.route, work.body) : http_post(config, work.route, work.body);
            } catch (...) {}
            if (response && response->size() > exam::kMaximumExamPayloadBytes) response.reset();
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
        {
            std::scoped_lock lock(mutex);
            running = false;
            pending.swap(queue);
            pending_control.swap(control_queue);
            queued_bytes = 0;
            changed.notify_all();
        }
        for (auto& work : pending) { try { work.completion(std::nullopt); } catch (...) {} }
        for (auto& work : pending_control) { try { work.completion(std::nullopt); } catch (...) {} }
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

} // namespace nstu::server
