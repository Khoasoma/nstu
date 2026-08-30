#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace nstu::net {

struct MulticastConfig {
    std::string group = "239.192.0.1";
    std::uint16_t port = 47000;
    std::string interface_address = "0.0.0.0";
    std::uint8_t ttl = 1;
};

class WinsockRuntime {
public:
    WinsockRuntime() noexcept;
    ~WinsockRuntime();
    WinsockRuntime(const WinsockRuntime&) = delete;
    WinsockRuntime& operator=(const WinsockRuntime&) = delete;

    [[nodiscard]] bool ready() const noexcept;

private:
    bool ready_ = false;
};

class UdpMulticastSocket {
public:
    UdpMulticastSocket() noexcept;
    ~UdpMulticastSocket();
    UdpMulticastSocket(const UdpMulticastSocket&) = delete;
    UdpMulticastSocket& operator=(const UdpMulticastSocket&) = delete;

    [[nodiscard]] bool open_sender(const MulticastConfig& config,
                                   std::string* error = nullptr);
    [[nodiscard]] bool open_receiver(const MulticastConfig& config,
                                     std::string* error = nullptr);
    [[nodiscard]] int send(std::span<const std::byte> payload,
                           std::string* error = nullptr) const;
    [[nodiscard]] int receive(std::span<std::byte> payload,
                               std::string* error = nullptr) const;
    void close() noexcept;
    [[nodiscard]] bool is_open() const noexcept;

private:
    std::uintptr_t handle_ = static_cast<std::uintptr_t>(-1);
    std::uint32_t destination_address_ = 0;
    std::uint16_t destination_port_ = 0;
};

} // namespace nstu::net
