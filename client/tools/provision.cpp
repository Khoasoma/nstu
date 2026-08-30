#include "nstu/client_config.hpp"
#include "nstu/enrollment.hpp"
#include "nstu/deployment.hpp"
#include "nstu/multicast.hpp"
#include "nstu/network.hpp"
#include "nstu/protocol.hpp"

#include <windows.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace {

std::optional<std::string> narrow_ascii(const wchar_t* text) {
    std::string result;
    while (*text != L'\0') {
        if (*text > 0x7f) {
            return std::nullopt;
        }
        result.push_back(static_cast<char>(*text++));
    }
    return result;
}

int hex_value(wchar_t value) {
    if (value >= L'0' && value <= L'9') {
        return value - L'0';
    }
    if (value >= L'a' && value <= L'f') {
        return value - L'a' + 10;
    }
    if (value >= L'A' && value <= L'F') {
        return value - L'A' + 10;
    }
    return -1;
}

std::optional<nstu::security::ClientId> parse_client_id(const wchar_t* text) {
    if (wcslen(text) != nstu::security::kClientIdBytes * 2) {
        return std::nullopt;
    }
    nstu::security::ClientId id{};
    for (std::size_t index = 0; index < id.size(); ++index) {
        const int high = hex_value(text[index * 2]);
        const int low = hex_value(text[index * 2 + 1]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        id[index] = static_cast<std::byte>((high << 4) | low);
    }
    return id;
}

std::vector<std::byte> load_file(const wchar_t* path) {
    const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return {};
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 32 ||
        size.QuadPart > 4096) {
        CloseHandle(file);
        return {};
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    const bool succeeded = ReadFile(file, bytes.data(),
                                    static_cast<DWORD>(bytes.size()), &read,
                                    nullptr) && read == bytes.size();
    CloseHandle(file);
    if (!succeeded) {
        nstu::security::secure_zero(bytes);
        return {};
    }
    return bytes;
}

bool receive_exact(nstu::net::TcpSocket& socket, std::span<std::byte> output,
                   std::string* error) {
    std::size_t offset = 0;
    while (offset < output.size()) {
        const int received = socket.receive(output.subspan(offset), error);
        if (received <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(received);
    }
    return true;
}

std::optional<nstu::protocol::TcpFrame> receive_frame(
    nstu::net::TcpSocket& socket, std::string* error) {
    std::array<std::byte, 4> prefix{};
    if (!receive_exact(socket, prefix, error)) {
        return std::nullopt;
    }
    const std::uint32_t body_bytes =
        std::to_integer<std::uint32_t>(prefix[0]) |
        (std::to_integer<std::uint32_t>(prefix[1]) << 8u) |
        (std::to_integer<std::uint32_t>(prefix[2]) << 16u) |
        (std::to_integer<std::uint32_t>(prefix[3]) << 24u);
    if (body_bytes < nstu::protocol::kCommandHeaderBytes ||
        body_bytes > nstu::protocol::kCommandHeaderBytes +
                         nstu::protocol::kMaxCommandPayload) {
        return std::nullopt;
    }
    std::vector<std::byte> body(body_bytes);
    if (!receive_exact(socket, body, error)) {
        return std::nullopt;
    }
    std::vector<std::byte> wire(prefix.begin(), prefix.end());
    wire.insert(wire.end(), body.begin(), body.end());
    nstu::protocol::TcpFrameParser parser;
    std::vector<nstu::protocol::TcpFrame> frames;
    return parser.feed(wire, frames) && frames.size() == 1
        ? std::optional{std::move(frames.front())}
        : std::nullopt;
}

std::filesystem::path default_config_path() {
    const auto root = nstu::deployment::data_root(nullptr);
    return root.empty() ? std::filesystem::path{}
                        : root / L"client-config.bin";
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 6) {
        fwprintf(stderr,
                 L"Usage: nstu-provision.exe <server-ip> <port> "
                 L"<32-hex-client-id> <key-id> <enrollment-secret-file>\n");
        return 2;
    }
    const auto address = narrow_ascii(argv[1]);
    wchar_t* port_end = nullptr;
    const unsigned long parsed_port = wcstoul(argv[2], &port_end, 10);
    const auto client_id = parse_client_id(argv[3]);
    wchar_t* key_end = nullptr;
    const unsigned long parsed_key = wcstoul(argv[4], &key_end, 10);
    auto enrollment_secret = load_file(argv[5]);
    if (!address || address->empty() || *port_end != L'\0' ||
        parsed_port == 0 || parsed_port > 65535 || !client_id ||
        *key_end != L'\0' || parsed_key == 0 ||
        parsed_key > std::numeric_limits<std::uint32_t>::max() ||
        enrollment_secret.size() < nstu::security::kMinimumProtocolKeyBytes) {
        fwprintf(stderr, L"Invalid provisioning arguments.\n");
        nstu::security::secure_zero(enrollment_secret);
        return 2;
    }
    const auto now = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    const auto request = nstu::security::create_enrollment_request(
        *client_id, static_cast<std::uint32_t>(parsed_key), now,
        enrollment_secret);
    if (!request) {
        fwprintf(stderr, L"Enrollment request generation failed.\n");
        nstu::security::secure_zero(enrollment_secret);
        return 1;
    }
    nstu::net::WinsockRuntime winsock;
    nstu::net::TcpSocket socket;
    std::string error;
    if (!winsock.ready() ||
        !socket.connect(*address, static_cast<std::uint16_t>(parsed_port),
                        &error) ||
        !socket.set_io_timeouts(5000, 5000, &error)) {
        fwprintf(stderr, L"Server connection failed.\n");
        nstu::security::secure_zero(enrollment_secret);
        return 1;
    }
    const auto payload = nstu::security::encode_enrollment_request(*request);
    const nstu::protocol::CommandEnvelope envelope{
        .version = nstu::protocol::kCommandVersion,
        .type = nstu::protocol::CommandType::enrollment_request,
        .payload_bytes = static_cast<std::uint32_t>(payload.size()),
        .request_id = now == 0 ? 1 : now,
    };
    const auto wire = nstu::protocol::encode_tcp_frame(envelope, payload);
    if (socket.send_all(wire, &error) != static_cast<int>(wire.size())) {
        fwprintf(stderr, L"Enrollment request send failed.\n");
        nstu::security::secure_zero(enrollment_secret);
        return 1;
    }
    const auto response = receive_frame(socket, &error);
    if (!response || response->envelope.type !=
                         nstu::protocol::CommandType::enrollment_accept ||
        response->envelope.request_id != envelope.request_id ||
        response->payload.size() != sizeof(std::uint32_t) ||
        (std::to_integer<std::uint32_t>(response->payload[0]) |
         (std::to_integer<std::uint32_t>(response->payload[1]) << 8u) |
         (std::to_integer<std::uint32_t>(response->payload[2]) << 16u) |
         (std::to_integer<std::uint32_t>(response->payload[3]) << 24u)) !=
            static_cast<std::uint32_t>(parsed_key)) {
        fwprintf(stderr, L"Enrollment was rejected by the server.\n");
        nstu::security::secure_zero(enrollment_secret);
        return 1;
    }
    auto key = nstu::security::derive_enrolled_key(enrollment_secret, *request);
    nstu::security::secure_zero(enrollment_secret);
    if (!key) {
        fwprintf(stderr, L"Client key derivation failed.\n");
        return 1;
    }
    nstu::client::ClientRuntimeConfig config;
    config.server_address = *address;
    config.server_port = static_cast<std::uint16_t>(parsed_port);
    config.client_id = *client_id;
    config.key_id = static_cast<std::uint32_t>(parsed_key);
    config.pre_shared_key.assign(key->begin(), key->end());
    nstu::security::secure_zero(*key);
    const auto config_path = default_config_path();
    const bool data_root_ready =
        !config_path.empty() && nstu::deployment::ensure_data_root(
                                    config_path.parent_path(), nullptr);
    constexpr char entropy_text[] = "NSTU-CLIENT-CONFIG-V1";
    const auto entropy = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(entropy_text),
        sizeof(entropy_text) - 1);
    const bool saved = data_root_ready &&
        nstu::client::save_client_runtime_config(
            config, config_path.wstring(), entropy, &error);
    nstu::client::clear_client_runtime_config(config);
    if (!saved) {
        fwprintf(stderr, L"Protected client configuration save failed.\n");
        return 1;
    }
    wprintf(L"NSTU client enrollment completed. Restart the NSTU service or "
            L"restart Windows.\n");
    return 0;
}
