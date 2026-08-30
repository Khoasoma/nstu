#include "nstu/multicast.hpp"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <limits>

namespace nstu::net {
namespace {

void set_error(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
}

#if defined(_WIN32)
bool parse_ipv4(const std::string& text, IN_ADDR& address) {
    return InetPtonA(AF_INET, text.c_str(), &address) == 1;
}
#endif

} // namespace

WinsockRuntime::WinsockRuntime() noexcept {
#if defined(_WIN32)
    WSADATA data{};
    ready_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
#endif
}

WinsockRuntime::~WinsockRuntime() {
#if defined(_WIN32)
    if (ready_) {
        WSACleanup();
    }
#endif
}

bool WinsockRuntime::ready() const noexcept {
#if defined(_WIN32)
    return ready_;
#else
    return false;
#endif
}

UdpMulticastSocket::UdpMulticastSocket() noexcept = default;

UdpMulticastSocket::~UdpMulticastSocket() {
    close();
}

bool UdpMulticastSocket::open_sender(const MulticastConfig& config,
                                     std::string* error) {
#if defined(_WIN32)
    close();
    IN_ADDR group{};
    if (!parse_ipv4(config.group, group)) {
        set_error(error, "invalid multicast group address");
        return false;
    }
    if (!IN_MULTICAST(ntohl(group.S_un.S_addr))) {
        set_error(error, "address is not an IPv4 multicast group");
        return false;
    }
    if (config.ttl == 0) {
        set_error(error, "multicast TTL must be non-zero");
        return false;
    }

    const SOCKET socket_handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_handle == INVALID_SOCKET) {
        set_error(error, "socket creation failed");
        return false;
    }
    const int ttl = config.ttl;
    const BOOL loopback = FALSE;
    if (setsockopt(socket_handle, IPPROTO_IP, IP_MULTICAST_TTL,
                   reinterpret_cast<const char*>(&ttl), sizeof(ttl)) ==
            SOCKET_ERROR ||
        setsockopt(socket_handle, IPPROTO_IP, IP_MULTICAST_LOOP,
                   reinterpret_cast<const char*>(&loopback), sizeof(loopback)) ==
            SOCKET_ERROR) {
        closesocket(socket_handle);
        set_error(error, "multicast socket option failed");
        return false;
    }
    if (config.interface_address != "0.0.0.0") {
        IN_ADDR interface_address{};
        if (!parse_ipv4(config.interface_address, interface_address) ||
            setsockopt(socket_handle, IPPROTO_IP, IP_MULTICAST_IF,
                       reinterpret_cast<const char*>(&interface_address),
                       sizeof(interface_address)) == SOCKET_ERROR) {
            closesocket(socket_handle);
            set_error(error, "multicast interface selection failed");
            return false;
        }
    }

    handle_ = static_cast<std::uintptr_t>(socket_handle);
    destination_address_ = group.S_un.S_addr;
    destination_port_ = config.port;
    return true;
#else
    (void)config;
    set_error(error, "multicast sockets require Windows");
    return false;
#endif
}

bool UdpMulticastSocket::open_receiver(const MulticastConfig& config,
                                       std::string* error) {
#if defined(_WIN32)
    close();
    IN_ADDR group{};
    IN_ADDR interface_address{};
    if (!parse_ipv4(config.group, group) ||
        !parse_ipv4(config.interface_address, interface_address)) {
        set_error(error, "invalid multicast or interface address");
        return false;
    }
    if (!IN_MULTICAST(ntohl(group.S_un.S_addr))) {
        set_error(error, "address is not an IPv4 multicast group");
        return false;
    }

    const SOCKET socket_handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_handle == INVALID_SOCKET) {
        set_error(error, "socket creation failed");
        return false;
    }
    const BOOL reuse = TRUE;
    if (setsockopt(socket_handle, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&reuse), sizeof(reuse)) ==
        SOCKET_ERROR) {
        closesocket(socket_handle);
        set_error(error, "SO_REUSEADDR failed");
        return false;
    }

    sockaddr_in bind_address{};
    bind_address.sin_family = AF_INET;
    bind_address.sin_port = htons(config.port);
    bind_address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(socket_handle, reinterpret_cast<const sockaddr*>(&bind_address),
             sizeof(bind_address)) == SOCKET_ERROR) {
        closesocket(socket_handle);
        set_error(error, "multicast bind failed");
        return false;
    }

    ip_mreq membership{};
    membership.imr_multiaddr = group;
    membership.imr_interface = interface_address;
    if (setsockopt(socket_handle, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   reinterpret_cast<const char*>(&membership),
                   sizeof(membership)) == SOCKET_ERROR) {
        closesocket(socket_handle);
        set_error(error, "multicast join failed");
        return false;
    }

    handle_ = static_cast<std::uintptr_t>(socket_handle);
    return true;
#else
    (void)config;
    set_error(error, "multicast sockets require Windows");
    return false;
#endif
}

int UdpMulticastSocket::send(std::span<const std::byte> payload,
                             std::string* error) const {
#if defined(_WIN32)
    if (!is_open() || destination_address_ == 0 || payload.size() >
        static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        set_error(error, "socket is not an open sender");
        return -1;
    }
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(destination_port_);
    destination.sin_addr.s_addr = destination_address_;
    const int result = sendto(
        static_cast<SOCKET>(handle_),
        reinterpret_cast<const char*>(payload.data()),
        static_cast<int>(payload.size()), 0,
        reinterpret_cast<const sockaddr*>(&destination), sizeof(destination));
    if (result == SOCKET_ERROR) {
        set_error(error, "multicast send failed");
        return -1;
    }
    return result;
#else
    (void)payload;
    set_error(error, "multicast sockets require Windows");
    return -1;
#endif
}

int UdpMulticastSocket::receive(std::span<std::byte> payload,
                                std::string* error) const {
#if defined(_WIN32)
    if (!is_open() || payload.size() >
        static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        set_error(error, "socket is not open");
        return -1;
    }
    const int result = recvfrom(
        static_cast<SOCKET>(handle_),
        reinterpret_cast<char*>(payload.data()),
        static_cast<int>(payload.size()), 0, nullptr, nullptr);
    if (result == SOCKET_ERROR) {
        set_error(error, "multicast receive failed");
        return -1;
    }
    return result;
#else
    (void)payload;
    set_error(error, "multicast sockets require Windows");
    return -1;
#endif
}

void UdpMulticastSocket::close() noexcept {
#if defined(_WIN32)
    if (is_open()) {
        closesocket(static_cast<SOCKET>(handle_));
    }
#endif
    handle_ = static_cast<std::uintptr_t>(INVALID_SOCKET);
    destination_address_ = 0;
    destination_port_ = 0;
}

bool UdpMulticastSocket::is_open() const noexcept {
#if defined(_WIN32)
    return handle_ != static_cast<std::uintptr_t>(INVALID_SOCKET);
#else
    return false;
#endif
}

} // namespace nstu::net
