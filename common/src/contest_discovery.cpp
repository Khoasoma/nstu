#include "nstu/contest_discovery.hpp"

#include "nstu/multicast.hpp"
#include "nstu/rate_limiter.hpp"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>

namespace nstu::contest_discovery {
namespace {
constexpr std::array<std::byte, 4> kMagic{
    std::byte{'N'}, std::byte{'S'}, std::byte{'C'}, std::byte{'R'}};
constexpr std::uint16_t kProbeKind = 1;
constexpr std::uint16_t kReplyKind = 2;

void set_error(std::string* error, const char* message) {
    if (error != nullptr) *error = message;
}

bool nonzero(const security::Nonce& nonce) noexcept {
    return std::any_of(nonce.begin(), nonce.end(),
                       [](std::byte byte) { return byte != std::byte{0}; });
}

bool valid_id(const std::string& id) noexcept {
    return id.size() == 32 && id.find_first_not_of("0123456789abcdef") ==
                                 std::string::npos &&
           id.find_first_not_of('0') != std::string::npos;
}

bool printable(std::uint32_t code) noexcept {
    // Reject control/format characters, including invisible bidi overrides.
    return code >= 0x20 && !(code >= 0x7f && code <= 0x9f) && code != 0xad &&
           !(code >= 0x600 && code <= 0x605) && code != 0x61c &&
           code != 0x6dd && code != 0x70f && !(code >= 0x890 && code <= 0x891) &&
           code != 0x8e2 && code != 0x180e &&
           !(code >= 0x200b && code <= 0x200f) &&
           !(code >= 0x2028 && code <= 0x202e) &&
           !(code >= 0x2060 && code <= 0x206f) && code != 0xfeff &&
           !(code >= 0xfff9 && code <= 0xfffb) && code != 0x110bd &&
           code != 0x110cd && !(code >= 0x13430 && code <= 0x1343f) &&
           !(code >= 0x1bca0 && code <= 0x1bca3) &&
           !(code >= 0x1d173 && code <= 0x1d17a) &&
           !(code >= 0xe0000 && code <= 0xe007f) &&
           !(code >= 0xfdd0 && code <= 0xfdef) && (code & 0xfffe) != 0xfffe;
}

bool valid_label(const std::string& label) noexcept {
    if (label.empty() || label.size() > kMaximumLabelBytes) return false;
    std::size_t offset = 0;
    while (offset < label.size()) {
        const auto first = static_cast<unsigned char>(label[offset++]);
        std::uint32_t code = first;
        unsigned int trailing = 0;
        std::uint32_t minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            code = first & 0x1f; trailing = 1; minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            code = first & 0x0f; trailing = 2; minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            code = first & 0x07; trailing = 3; minimum = 0x10000;
        } else if (first >= 0x80) {
            return false;
        }
        if (label.size() - offset < trailing) return false;
        for (unsigned int i = 0; i < trailing; ++i) {
            const auto next = static_cast<unsigned char>(label[offset++]);
            if ((next & 0xc0) != 0x80) return false;
            code = (code << 6) | (next & 0x3f);
        }
        if (code < minimum || code > 0x10ffff ||
            (code >= 0xd800 && code <= 0xdfff) || !printable(code)) return false;
    }
    return true;
}

bool valid_room(const RoomAdvertisement& room) noexcept {
    const auto& teacher = room.teacher_id;
    const auto ascii_alnum = [](unsigned char c) noexcept {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    };
    if (teacher.empty() || teacher.size() > kMaximumTeacherIdBytes ||
        (!ascii_alnum(static_cast<unsigned char>(teacher.front())) && teacher.front() != '_') ||
        !std::all_of(teacher.begin(), teacher.end(), [&](unsigned char c) {
            return ascii_alnum(c) || c == '_' || c == '.' || c == '-';
        })) return false;
    return valid_id(room.room_id) && valid_id(room.round_id) &&
           valid_label(room.title) && valid_label(room.teacher_label) &&
           room.control_port != 0 && room.protocol_version == kRoomProtocolVersion &&
           (room.join_state == JoinState::open ||
            room.join_state == JoinState::join_closed);
}

// Decimal literal IPv4 only; accepting an address never performs a DNS lookup.
bool valid_address(const std::string& address) noexcept {
    std::array<unsigned int, 4> parts{};
    std::size_t offset = 0;
    for (std::size_t part = 0; part < parts.size(); ++part) {
        const auto start = offset;
        while (offset < address.size() && address[offset] >= '0' &&
               address[offset] <= '9') {
            if (offset - start >= 3) return false;
            parts[part] = parts[part] * 10 + static_cast<unsigned int>(address[offset++] - '0');
        }
        if (offset == start || parts[part] > 255 ||
            (offset - start > 1 && address[start] == '0')) return false;
        if (part < 3 && (offset == address.size() || address[offset++] != '.')) return false;
    }
    return offset == address.size() && parts[0] != 0 && parts[0] < 224;
}

void append_u16(std::vector<std::byte>& wire, std::uint16_t value) {
    wire.push_back(static_cast<std::byte>(value & 0xffu));
    wire.push_back(static_cast<std::byte>(value >> 8u));
}

std::uint16_t read_u16(std::span<const std::byte> wire, std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(std::to_integer<unsigned int>(wire[offset]) |
           (std::to_integer<unsigned int>(wire[offset + 1]) << 8u));
}

void append_text(std::vector<std::byte>& wire, const std::string& text) {
    const auto bytes = std::as_bytes(std::span(text));
    wire.insert(wire.end(), bytes.begin(), bytes.end());
}

bool header_matches(std::span<const std::byte> wire, std::uint16_t kind) noexcept {
    return wire.size() >= kProbeBytes && wire.size() <= kMaximumDatagramBytes &&
           std::equal(kMagic.begin(), kMagic.end(), wire.begin()) &&
           read_u16(wire, 4) == kDiscoveryVersion && read_u16(wire, 6) == kind;
}

#if defined(_WIN32)
class Socket {
public:
    explicit Socket(SOCKET value = INVALID_SOCKET) noexcept : value_(value) {}
    ~Socket() { reset(); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    [[nodiscard]] SOCKET get() const noexcept { return value_; }
    [[nodiscard]] bool valid() const noexcept { return value_ != INVALID_SOCKET; }
    SOCKET release() noexcept { return std::exchange(value_, INVALID_SOCKET); }
    void reset() noexcept {
        if (valid()) closesocket(std::exchange(value_, INVALID_SOCKET));
    }
private:
    SOCKET value_;
};

void socket_error(std::string* error, const char* operation) {
    if (error != nullptr) *error = std::string(operation) + ": Winsock error " +
                                  std::to_string(WSAGetLastError());
}

bool setup_socket(SOCKET socket, bool responder, std::string* error) {
    const BOOL enabled = TRUE;
    const auto option = responder ? SO_EXCLUSIVEADDRUSE : SO_BROADCAST;
    if (setsockopt(socket, SOL_SOCKET, option,
                   reinterpret_cast<const char*>(&enabled), sizeof(enabled)) == SOCKET_ERROR) {
        socket_error(error, "contest discovery socket setup");
        return false;
    }
    // UDP ICMP errors must not abort the sweep when one manual target is absent.
    constexpr DWORD udp_connection_reset = _WSAIOW(IOC_VENDOR, 12);
    BOOL reset = FALSE;
    DWORD returned = 0;
    if (WSAIoctl(socket, udp_connection_reset, &reset, sizeof(reset), nullptr, 0,
                 &returned, nullptr, nullptr) == SOCKET_ERROR) {
        socket_error(error, "contest discovery UDP reset setup");
        return false;
    }
    u_long nonblocking = 1;
    if (ioctlsocket(socket, FIONBIO, &nonblocking) == SOCKET_ERROR) {
        socket_error(error, "contest discovery nonblocking setup");
        return false;
    }
    return true;
}

sockaddr_in endpoint(std::uint32_t address, std::uint16_t port) noexcept {
    sockaddr_in value{};
    value.sin_family = AF_INET;
    value.sin_port = htons(port);
    value.sin_addr.s_addr = address;
    return value;
}

std::string address_text(const sockaddr_in& source) {
    std::array<char, INET_ADDRSTRLEN> text{};
    if (InetNtopA(AF_INET, &source.sin_addr, text.data(),
                  static_cast<DWORD>(text.size())) == nullptr) return {};
    return text.data();
}

void add_target(std::vector<sockaddr_in>& targets, sockaddr_in target) {
    if (targets.size() < kMaximumCandidates &&
        std::none_of(targets.begin(), targets.end(), [&](const auto& existing) {
            return existing.sin_addr.s_addr == target.sin_addr.s_addr &&
                   existing.sin_port == target.sin_port;
        })) targets.push_back(target);
}

std::vector<sockaddr_in> targets_for(const DiscoveryOptions& options, std::string* error) {
    std::vector<sockaddr_in> targets;
    if (!options.manual_targets.empty()) {
        if (options.manual_targets.size() > kMaximumCandidates) {
            set_error(error, "too many manual contest discovery targets");
            return {};
        }
        for (const auto& target : options.manual_targets) {
            IN_ADDR address{};
            if (!valid_address(target.address) || target.port == 0 ||
                InetPtonA(AF_INET, target.address.c_str(), &address) != 1) {
                set_error(error, "manual contest target requires literal IPv4 and nonzero port");
                return {};
            }
            add_target(targets, endpoint(address.s_addr, target.port));
        }
        return targets;
    }

    ULONG bytes = 16 * 1024;
    ULONG result = ERROR_BUFFER_OVERFLOW;
    std::vector<std::max_align_t> buffer;
    PIP_ADAPTER_ADDRESSES adapters = nullptr;
    for (int attempt = 0; attempt < 3 && result == ERROR_BUFFER_OVERFLOW; ++attempt) {
        if (bytes > 1024 * 1024) break;
        buffer.resize((bytes + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
        adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
        result = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST |
            GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, adapters, &bytes);
    }
    if (result != NO_ERROR) {
        set_error(error, "active IPv4 LAN adapter enumeration failed; use a manual target");
        return {};
    }
    for (const auto* adapter = adapters; adapter != nullptr; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp ||
            (adapter->IfType != IF_TYPE_ETHERNET_CSMACD && adapter->IfType != IF_TYPE_IEEE80211)) continue;
        for (const auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr;
             unicast = unicast->Next) {
            const auto prefix = unicast->OnLinkPrefixLength;
            if (unicast->Address.lpSockaddr == nullptr ||
                unicast->Address.lpSockaddr->sa_family != AF_INET || prefix == 0 || prefix > 30) continue;
            const auto* address = reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
            const auto mask = 0xffffffffu << (32u - prefix);
            const auto broadcast = htonl((ntohl(address->sin_addr.s_addr) & mask) | ~mask);
            add_target(targets, endpoint(broadcast, options.discovery_port));
        }
    }
    return targets;
}

int wait_readable(SOCKET socket, std::chrono::steady_clock::time_point deadline) noexcept {
    const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
        deadline - std::chrono::steady_clock::now()).count();
    if (remaining <= 0) return 0;
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(socket, &readable);
    timeval wait{static_cast<long>(remaining / 1000000),
                 static_cast<long>(remaining % 1000000)};
    return select(0, &readable, nullptr, nullptr, &wait);
}
#endif
} // namespace

std::vector<std::byte> encode_probe(const Probe& probe) {
    if (!nonzero(probe.nonce)) return {};
    std::vector<std::byte> wire;
    wire.reserve(kProbeBytes);
    wire.insert(wire.end(), kMagic.begin(), kMagic.end());
    append_u16(wire, kDiscoveryVersion);
    append_u16(wire, kProbeKind);
    wire.insert(wire.end(), probe.nonce.begin(), probe.nonce.end());
    return wire;
}

std::optional<Probe> decode_probe(std::span<const std::byte> wire) {
    if (wire.size() != kProbeBytes || !header_matches(wire, kProbeKind)) return std::nullopt;
    Probe probe;
    std::copy_n(wire.begin() + 8, probe.nonce.size(), probe.nonce.begin());
    return nonzero(probe.nonce) ? std::optional{probe} : std::nullopt;
}

std::vector<std::byte> encode_reply(const Probe& probe, const RoomAdvertisement& room) {
    if (!nonzero(probe.nonce) || !valid_room(room)) return {};
    auto wire = encode_probe(probe);
    wire.reserve(kReplyHeaderBytes + room.title.size() + room.teacher_label.size() + room.teacher_id.size());
    wire[6] = static_cast<std::byte>(kReplyKind);
    append_u16(wire, room.protocol_version);
    append_u16(wire, room.control_port);
    append_u16(wire, static_cast<std::uint16_t>(room.join_state));
    append_u16(wire, static_cast<std::uint16_t>(room.title.size()));
    append_u16(wire, static_cast<std::uint16_t>(room.teacher_label.size()));
    append_text(wire, room.room_id);
    append_text(wire, room.round_id);
    append_u16(wire, static_cast<std::uint16_t>(room.teacher_id.size()));
    append_text(wire, room.title);
    append_text(wire, room.teacher_label);
    append_text(wire, room.teacher_id);
    return wire;
}

std::optional<RoomAdvertisement> decode_reply(std::span<const std::byte> wire, const Probe& probe) {
    if (wire.size() < kReplyHeaderBytes || !header_matches(wire, kReplyKind) ||
        !nonzero(probe.nonce) || !std::equal(probe.nonce.begin(), probe.nonce.end(), wire.begin() + 8)) {
        return std::nullopt;
    }
    const auto title_bytes = read_u16(wire, 46);
    const auto label_bytes = read_u16(wire, 48);
    const auto teacher_bytes = read_u16(wire, 114);
    if (title_bytes > kMaximumLabelBytes || label_bytes > kMaximumLabelBytes ||
        teacher_bytes == 0 || teacher_bytes > kMaximumTeacherIdBytes ||
        wire.size() != kReplyHeaderBytes + title_bytes + label_bytes + teacher_bytes) return std::nullopt;
    RoomAdvertisement room;
    room.protocol_version = read_u16(wire, 40);
    room.control_port = read_u16(wire, 42);
    room.join_state = static_cast<JoinState>(read_u16(wire, 44));
    const auto* text = reinterpret_cast<const char*>(wire.data());
    room.room_id.assign(text + 50, 32);
    room.round_id.assign(text + 82, 32);
    room.title.assign(text + kReplyHeaderBytes, title_bytes);
    room.teacher_label.assign(text + kReplyHeaderBytes + title_bytes, label_bytes);
    room.teacher_id.assign(text + kReplyHeaderBytes + title_bytes + label_bytes, teacher_bytes);
    return valid_room(room) ? std::optional{std::move(room)} : std::nullopt;
}

CandidateSet::CandidateSet(Probe probe, std::chrono::steady_clock::time_point started,
                           std::size_t maximum_candidates)
    : probe_(probe), started_(started), limit_(std::min(maximum_candidates, kMaximumCandidates)) {}

bool CandidateSet::accept(std::span<const std::byte> wire, const std::string& source_address,
                           std::chrono::steady_clock::time_point now) {
    if (now < started_ || now - started_ >= kProbeBudget ||
        candidates_.size() >= limit_ || !valid_address(source_address)) return false;
    auto room = decode_reply(wire, probe_);
    if (!room || std::any_of(candidates_.begin(), candidates_.end(), [&](const auto& candidate) {
        return candidate.room.room_id == room->room_id ||
               (candidate.address == source_address && candidate.room.control_port == room->control_port);
    })) return false;
    candidates_.push_back({source_address, std::move(*room), now});
    return true;
}

const std::vector<RoomCandidate>& CandidateSet::candidates(std::chrono::steady_clock::time_point now) {
    std::erase_if(candidates_, [&](const auto& candidate) {
        return now < candidate.observed_at || now - candidate.observed_at >= kHintLifetime;
    });
    return candidates_;
}

std::vector<RoomCandidate> discover_rooms(const DiscoveryOptions& options, std::string* error) {
    set_error(error, "");
#if defined(_WIN32)
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::clamp(options.timeout, std::chrono::milliseconds{1}, kProbeBudget);
    if (options.discovery_port == 0 || options.maximum_candidates == 0) {
        set_error(error, "invalid contest discovery configuration");
        return {};
    }
    net::WinsockRuntime winsock;
    if (!winsock.ready()) {
        set_error(error, "contest discovery Winsock initialization failed");
        return {};
    }
    auto targets = targets_for(options, error);
    if (targets.empty()) {
        if (error == nullptr || error->empty()) set_error(error, "no active IPv4 LAN target; use a manual target");
        return {};
    }
    Probe probe;
    if (!security::generate_random(probe.nonce)) {
        set_error(error, "contest discovery nonce generation failed");
        return {};
    }
    const auto wire = encode_probe(probe);
    if (wire.empty()) {
        set_error(error, "contest discovery probe encoding failed");
        return {};
    }
    Socket socket(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (!socket.valid()) {
        socket_error(error, "contest discovery socket creation");
        return {};
    }
    if (!setup_socket(socket.get(), false, error)) return {};
    const auto local = endpoint(htonl(INADDR_ANY), 0);
    if (bind(socket.get(), reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR) {
        socket_error(error, "contest discovery bind");
        return {};
    }
    bool sent = false;
    for (const auto& target : targets) {
        if (std::chrono::steady_clock::now() >= deadline) break;
        const auto count = sendto(socket.get(), reinterpret_cast<const char*>(wire.data()),
            static_cast<int>(wire.size()), 0, reinterpret_cast<const sockaddr*>(&target), sizeof(target));
        sent = sent || count == static_cast<int>(wire.size());
    }
    if (!sent) {
        set_error(error, "contest discovery probe could not be sent within its budget");
        return {};
    }
    CandidateSet candidates(probe, started, options.maximum_candidates);
    std::array<std::byte, kMaximumDatagramBytes> buffer{};
    while (std::chrono::steady_clock::now() < deadline &&
           candidates.candidates(std::chrono::steady_clock::now()).size() <
               std::min(options.maximum_candidates, kMaximumCandidates)) {
        const auto ready = wait_readable(socket.get(), deadline);
        if (ready == 0) break;
        if (ready == SOCKET_ERROR) {
            socket_error(error, "contest discovery wait");
            return {};
        }
        sockaddr_in source{};
        int source_bytes = sizeof(source);
        const auto received = recvfrom(socket.get(), reinterpret_cast<char*>(buffer.data()),
            static_cast<int>(buffer.size()), 0, reinterpret_cast<sockaddr*>(&source), &source_bytes);
        if (received <= 0 || source.sin_family != AF_INET) continue;
        const auto source_port = ntohs(source.sin_port);
        if (std::none_of(targets.begin(), targets.end(), [&](const auto& target) {
            return source_port == ntohs(target.sin_port);
        })) continue;
        (void)candidates.accept(std::span(buffer).first(static_cast<std::size_t>(received)),
                                 address_text(source), std::chrono::steady_clock::now());
    }
    auto result = candidates.candidates(std::chrono::steady_clock::now());
    if (result.empty()) set_error(error, "no contest room answered the discovery probe");
    return result;
#else
    (void)options;
    set_error(error, "contest discovery requires Windows");
    return {};
#endif
}

class RoomDiscoveryResponder::Impl {
public:
    ~Impl() { stop(); }

    bool start(std::uint16_t port, std::string* error) {
        set_error(error, "");
#if defined(_WIN32)
        if (worker_.joinable() || !winsock_.ready()) {
            set_error(error, "contest discovery is already started or Winsock is unavailable");
            return false;
        }
        Socket socket(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
        if (!socket.valid()) {
            socket_error(error, "contest discovery responder socket creation");
            return false;
        }
        if (!setup_socket(socket.get(), true, error)) return false;
        auto local = endpoint(htonl(INADDR_ANY), port);
        if (bind(socket.get(), reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR) {
            socket_error(error, "contest discovery responder bind");
            return false;
        }
        int bytes = sizeof(local);
        if (getsockname(socket.get(), reinterpret_cast<sockaddr*>(&local), &bytes) == SOCKET_ERROR) {
            socket_error(error, "contest discovery responder local port");
            return false;
        }
        socket_ = socket.release();
        local_port_ = ntohs(local.sin_port);
        running_ = true;
        beacons_sent_ = 0;
        try {
            worker_ = std::jthread([this](std::stop_token token) { run(token); });
        } catch (const std::system_error&) {
            closesocket(std::exchange(socket_, INVALID_SOCKET));
            local_port_ = 0;
            running_ = false;
            set_error(error, "contest discovery responder thread creation failed");
            return false;
        }
        return true;
#else
        (void)port;
        set_error(error, "contest discovery requires Windows");
        return false;
#endif
    }

    void stop() noexcept {
#if defined(_WIN32)
        if (worker_.joinable()) {
            worker_.request_stop();
            worker_.join();
        }
        if (socket_ != INVALID_SOCKET) closesocket(std::exchange(socket_, INVALID_SOCKET));
#endif
        running_ = false;
        local_port_ = 0;
        std::scoped_lock lock(mutex_);
        room_.reset();
    }

    bool set_beacon(std::optional<RoomAdvertisement> room, std::string* error) {
        const auto valid = !room || valid_room(*room);
        if (!valid) room.reset();
        {
            std::scoped_lock lock(mutex_);
            room_ = std::move(room);
        }
        set_error(error, valid ? "" : "invalid contest discovery metadata; beacon disabled");
        return valid;
    }

    [[nodiscard]] bool running() const noexcept { return running_.load(); }
    [[nodiscard]] std::uint16_t local_port() const noexcept { return local_port_.load(); }
    [[nodiscard]] std::uint64_t beacons_sent() const noexcept { return beacons_sent_.load(); }

private:
#if defined(_WIN32)
    void run(std::stop_token token) {
        security::HandshakeRateLimiter limiter({.maximum_sources = 128,
            .maximum_failures = 8, .window = std::chrono::seconds{1}, .block = std::chrono::seconds{1}});
        auto global_window = std::chrono::steady_clock::now();
        unsigned int global_count = 0;
        std::array<std::byte, kMaximumDatagramBytes> buffer{};
        while (!token.stop_requested()) {
            const auto ready = wait_readable(socket_, std::chrono::steady_clock::now() + std::chrono::milliseconds{100});
            if (ready == 0) continue;
            if (ready == SOCKET_ERROR) break;
            sockaddr_in source{};
            int source_bytes = sizeof(source);
            const auto received = recvfrom(socket_, reinterpret_cast<char*>(buffer.data()),
                static_cast<int>(buffer.size()), 0, reinterpret_cast<sockaddr*>(&source), &source_bytes);
            const auto now = std::chrono::steady_clock::now();
            if (now - global_window >= std::chrono::seconds{1}) {
                global_window = now;
                global_count = 0;
            }
            if (received <= 0 || source.sin_family != AF_INET || source.sin_port == 0 || global_count >= 64) continue;
            const auto address = address_text(source);
            if (!valid_address(address) || !limiter.allow(address, now)) continue;
            // Every unauthenticated probe consumes the source budget, even a
            // valid one; a successful beacon must never reset an attacker's cap.
            limiter.record_failure(address, now);
            const auto probe = decode_probe(std::span(buffer).first(static_cast<std::size_t>(received)));
            if (!probe) continue;
            std::optional<RoomAdvertisement> room;
            {
                std::scoped_lock lock(mutex_);
                room = room_;
            }
            if (!room) continue;
            const auto wire = encode_reply(*probe, *room);
            ++global_count;
            if (sendto(socket_, reinterpret_cast<const char*>(wire.data()), static_cast<int>(wire.size()),
                0, reinterpret_cast<const sockaddr*>(&source), source_bytes) == static_cast<int>(wire.size())) {
                beacons_sent_.fetch_add(1);
            }
        }
        running_ = false;
    }
    net::WinsockRuntime winsock_;
    SOCKET socket_ = INVALID_SOCKET;
#endif
    std::mutex mutex_;
    std::optional<RoomAdvertisement> room_;
    std::jthread worker_;
    std::atomic_bool running_{false};
    std::atomic<std::uint16_t> local_port_{0};
    std::atomic<std::uint64_t> beacons_sent_{0};
};

RoomDiscoveryResponder::RoomDiscoveryResponder() : impl_(std::make_unique<Impl>()) {}
RoomDiscoveryResponder::~RoomDiscoveryResponder() = default;
bool RoomDiscoveryResponder::start(std::uint16_t port, std::string* error) { return impl_->start(port, error); }
void RoomDiscoveryResponder::stop() noexcept { impl_->stop(); }
bool RoomDiscoveryResponder::set_beacon(std::optional<RoomAdvertisement> room, std::string* error) {
    return impl_->set_beacon(std::move(room), error);
}
bool RoomDiscoveryResponder::running() const noexcept { return impl_->running(); }
std::uint16_t RoomDiscoveryResponder::local_port() const noexcept { return impl_->local_port(); }
std::uint64_t RoomDiscoveryResponder::beacons_sent() const noexcept { return impl_->beacons_sent(); }

} // namespace nstu::contest_discovery
