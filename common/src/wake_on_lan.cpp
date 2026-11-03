#include "nstu/wake_on_lan.hpp"
#include "nstu/multicast.hpp"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace nstu::net {
namespace {

void set_error(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
}

int hex_val(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

#if defined(_WIN32)
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
void close_socket(SocketHandle s) noexcept {
    if (s != kInvalidSocket) {
        ::closesocket(s);
    }
}
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
void close_socket(SocketHandle s) noexcept {
    if (s >= 0) {
        ::close(s);
    }
}
#endif

struct SocketCloser {
    SocketHandle sock = kInvalidSocket;
    explicit SocketCloser(SocketHandle s) noexcept : sock(s) {}
    ~SocketCloser() {
        close_socket(sock);
    }
    SocketCloser(const SocketCloser&) = delete;
    SocketCloser& operator=(const SocketCloser&) = delete;
};

} // namespace

const char* to_string(WolSupportState state) noexcept {
    switch (state) {
    case WolSupportState::unknown:
        return "unknown";
    case WolSupportState::supported:
        return "supported";
    case WolSupportState::unsupported:
        return "unsupported";
    case WolSupportState::missing_mac:
        return "missing_mac";
    }
    return "unknown";
}

const char* wol_support_label(WolSupportState state, bool vietnamese) noexcept {
    if (vietnamese) {
        switch (state) {
        case WolSupportState::unknown:
            return "Chưa xác định";
        case WolSupportState::supported:
            return "Được hỗ trợ";
        case WolSupportState::unsupported:
            return "Không hỗ trợ";
        case WolSupportState::missing_mac:
            return "Thiếu địa chỉ MAC";
        }
        return "Chưa xác định";
    }
    switch (state) {
    case WolSupportState::unknown:
        return "Unknown";
    case WolSupportState::supported:
        return "Supported";
    case WolSupportState::unsupported:
        return "Unsupported";
    case WolSupportState::missing_mac:
        return "Missing MAC";
    }
    return "Unknown";
}

std::optional<MacAddress> parse_mac_address(std::string_view text) noexcept {
    // Expected format: 17 chars: XX:XX:XX:XX:XX:XX or XX-XX-XX-XX-XX-XX
    if (text.size() != 17) {
        return std::nullopt;
    }

    const char separator = text[2];
    if (separator != ':' && separator != '-') {
        return std::nullopt;
    }

    MacAddress mac{};
    std::size_t cursor = 0;

    for (std::size_t i = 0; i < 6; ++i) {
        const int high = hex_val(text[cursor]);
        const int low = hex_val(text[cursor + 1]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }

        mac.octets[i] = static_cast<std::uint8_t>((high << 4) | low);
        cursor += 2;

        if (i < 5) {
            if (text[cursor] != separator) {
                return std::nullopt;
            }
            ++cursor;
        }
    }

    return mac;
}

std::string format_mac_address(const MacAddress& mac,
                               char separator,
                               bool uppercase) {
    std::ostringstream oss;
    const char* hex_digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";

    for (std::size_t i = 0; i < 6; ++i) {
        const auto byte = mac.octets[i];
        oss << hex_digits[(byte >> 4) & 0x0F];
        oss << hex_digits[byte & 0x0F];
        if (i < 5 && separator != '\0') {
            oss << separator;
        }
    }

    return oss.str();
}

std::array<std::uint8_t, kWolMagicPacketSize> create_magic_packet(
    const MacAddress& mac) noexcept {
    std::array<std::uint8_t, kWolMagicPacketSize> packet{};

    // First 6 bytes: 0xFF
    for (std::size_t i = 0; i < 6; ++i) {
        packet[i] = 0xFF;
    }

    // Next 16 repetitions of MAC address (16 * 6 = 96 bytes)
    std::size_t offset = 6;
    for (std::size_t rep = 0; rep < 16; ++rep) {
        for (std::size_t i = 0; i < 6; ++i) {
            packet[offset++] = mac.octets[i];
        }
    }

    return packet;
}

bool verify_magic_packet(
    std::span<const std::uint8_t> packet,
    const MacAddress& expected_mac) noexcept {
    if (packet.size() != kWolMagicPacketSize) {
        return false;
    }

    for (std::size_t i = 0; i < 6; ++i) {
        if (packet[i] != 0xFF) {
            return false;
        }
    }

    std::size_t offset = 6;
    for (std::size_t rep = 0; rep < 16; ++rep) {
        for (std::size_t i = 0; i < 6; ++i) {
            if (packet[offset++] != expected_mac.octets[i]) {
                return false;
            }
        }
    }

    return true;
}

bool send_wake_on_lan_packet(
    const MacAddress& mac,
    const WolBroadcastOptions& options,
    std::string* error) {
    if (mac.is_zero()) {
        set_error(error, "cannot send magic packet to zero MAC address");
        return false;
    }

    WinsockRuntime winsock;
    if (!winsock.ready()) {
        set_error(error, "winsock initialization failed");
        return false;
    }

    const SocketHandle sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == kInvalidSocket) {
        set_error(error, "failed to create UDP socket for Wake-on-LAN");
        return false;
    }
    SocketCloser closer{sock};

#if defined(_WIN32)
    const BOOL broadcast = TRUE;
    if (::setsockopt(sock, SOL_SOCKET, SO_BROADCAST,
                     reinterpret_cast<const char*>(&broadcast),
                     sizeof(broadcast)) == SOCKET_ERROR) {
        set_error(error, "failed to set SO_BROADCAST socket option");
        return false;
    }
#else
    const int broadcast = 1;
    if (::setsockopt(sock, SOL_SOCKET, SO_BROADCAST,
                     &broadcast, sizeof(broadcast)) < 0) {
        set_error(error, "failed to set SO_BROADCAST socket option");
        return false;
    }
#endif

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(options.port);

#if defined(_WIN32)
    if (InetPtonA(AF_INET, options.broadcast_ip.c_str(), &dest.sin_addr) != 1) {
        set_error(error, "invalid broadcast IP address");
        return false;
    }
#else
    if (inet_pton(AF_INET, options.broadcast_ip.c_str(), &dest.sin_addr) != 1) {
        set_error(error, "invalid broadcast IP address");
        return false;
    }
#endif

    const auto packet = create_magic_packet(mac);
    const auto sent = ::sendto(
        sock,
        reinterpret_cast<const char*>(packet.data()),
        static_cast<int>(packet.size()),
        0,
        reinterpret_cast<const sockaddr*>(&dest),
        sizeof(dest));

    if (sent != static_cast<int>(packet.size())) {
        set_error(error, "failed to send complete magic packet");
        return false;
    }

    return true;
}

} // namespace nstu::net
