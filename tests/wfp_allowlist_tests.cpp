#include "nstu/exam_lockdown_controller.hpp"
#include "nstu/wfp_allowlist.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

namespace {

nstu::security::ClientId make_test_client_id(std::byte fill_byte) {
    nstu::security::ClientId id{};
    id.fill(fill_byte);
    return id;
}

nstu::exam::SessionId make_test_session_id(std::byte fill_byte) {
    nstu::exam::SessionId id{};
    id.fill(fill_byte);
    return id;
}

nstu::net::Ipv6Address make_ipv6(std::initializer_list<std::uint8_t> bytes) {
    nstu::net::Ipv6Address addr{};
    std::size_t i = 0;
    for (const auto b : bytes) {
        if (i < 16) {
            addr.bytes[i++] = b;
        }
    }
    return addr;
}

} // namespace

int main() {
    using namespace nstu::net;
    using namespace nstu::client;

    // 1. Ipv6Address struct tests: comparison, ordering, sorting, uniqueness
    {
        const auto ip_zero = make_ipv6({0});
        const auto ip_loopback = make_ipv6({0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1});
        const auto ip_google_1 = make_ipv6({0x20,0x01,0x48,0x60,0x48,0x60,0,0,0,0,0,0,0,0,0x88,0x88});
        const auto ip_google_2 = make_ipv6({0x20,0x01,0x48,0x60,0x48,0x60,0,0,0,0,0,0,0,0,0x88,0x44});

        assert(ip_zero == ip_zero);
        assert(ip_zero != ip_loopback);
        assert(ip_loopback < ip_google_1);
        assert(ip_google_2 < ip_google_1);

        std::vector<Ipv6Address> list{ip_google_1, ip_zero, ip_google_2, ip_loopback, ip_zero};
        std::sort(list.begin(), list.end());
        list.erase(std::unique(list.begin(), list.end()), list.end());

        assert(list.size() == 4);
        assert(list[0] == ip_zero);
        assert(list[1] == ip_loopback);
        assert(list[2] == ip_google_2);
        assert(list[3] == ip_google_1);
    }

    // 2. WfpAllowlistConfig default values
    {
        WfpAllowlistConfig cfg{};
        assert(cfg.default_deny == true);
        assert(cfg.permit_loopback == true);
        assert(cfg.permit_dns_dhcp == true);
        assert(cfg.block_http == true);
        assert(cfg.block_https == true);
        assert(cfg.allowed_ipv4.empty());
        assert(cfg.allowed_ipv6.empty());
        assert(cfg.server_ipv4 == 0);
        assert(cfg.server_port == 0);
        assert((cfg.server_ipv6 == Ipv6Address{}));
    }

    // 3. is_disallowed_ipv4 tests
    {
        // Unspecified 0.0.0.0
        assert(is_disallowed_ipv4(0x00000000u));

        // Loopback 127.0.0.0/8
        assert(is_disallowed_ipv4(0x7f000001u)); // 127.0.0.1
        assert(is_disallowed_ipv4(0x7fffffffu)); // 127.255.255.255

        // Link-local 169.254.0.0/16
        assert(is_disallowed_ipv4(0xa9fe0001u)); // 169.254.0.1
        assert(is_disallowed_ipv4(0xa9feffffu)); // 169.254.255.255

        // Multicast / reserved / broadcast (>= 224.0.0.0)
        assert(is_disallowed_ipv4(0xe0000001u)); // 224.0.0.1
        assert(is_disallowed_ipv4(0xeffffffau)); // 239.255.255.250
        assert(is_disallowed_ipv4(0xf0000001u)); // 240.0.0.1
        assert(is_disallowed_ipv4(0xffffffffu)); // 255.255.255.255

        // Valid allowed public and private IPv4 addresses
        assert(!is_disallowed_ipv4(0x08080808u)); // 8.8.8.8
        assert(!is_disallowed_ipv4(0x01010101u)); // 1.1.1.1
        assert(!is_disallowed_ipv4(0x5db8d822u)); // 93.184.216.34 (example.com)
        assert(!is_disallowed_ipv4(0xc0a80101u)); // 192.168.1.1
        assert(!is_disallowed_ipv4(0x0a000001u)); // 10.0.0.1
    }

    // 4. is_disallowed_ipv6 tests
    {
        // Unspecified ::
        assert(is_disallowed_ipv6(make_ipv6({0})));

        // Loopback ::1
        assert(is_disallowed_ipv6(make_ipv6({0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1})));

        // Link-local fe80::/10
        assert(is_disallowed_ipv6(make_ipv6({0xfe,0x80,0,0,0,0,0,0,0,0,0,0,0,0,0,1})));
        assert(is_disallowed_ipv6(make_ipv6({0xfe,0xbf,0,0,0,0,0,0,0,0,0,0,0,0,0,1})));

        // Multicast ff00::/8
        assert(is_disallowed_ipv6(make_ipv6({0xff,0x02,0,0,0,0,0,0,0,0,0,0,0,0,0,1})));
        assert(is_disallowed_ipv6(make_ipv6({0xff,0x05,0,0,0,0,0,0,0,0,0,0,0,0,0,2})));

        // Site-local fec0::/10
        assert(is_disallowed_ipv6(make_ipv6({0xfe,0xc0,0,0,0,0,0,0,0,0,0,0,0,0,0,1})));
        assert(is_disallowed_ipv6(make_ipv6({0xfe,0xdf,0,0,0,0,0,0,0,0,0,0,0,0,0,1})));

        // Discard prefix 100::/64
        assert(is_disallowed_ipv6(make_ipv6({0x01,0x00,0,0,0,0,0,0,0,0,0,0,0,0,0,1})));

        // Documentation 2001:db8::/32
        assert(is_disallowed_ipv6(make_ipv6({0x20,0x01,0x0d,0xb8,0,0,0,0,0,0,0,0,0,0,0,1})));

        // IPv4-mapped IPv6 ::ffff:x.x.x.x
        // Mapped loopback 127.0.0.1 -> disallowed
        assert(is_disallowed_ipv6(make_ipv6({0,0,0,0,0,0,0,0,0,0,0xff,0xff,127,0,0,1})));
        // Mapped link-local 169.254.1.1 -> disallowed
        assert(is_disallowed_ipv6(make_ipv6({0,0,0,0,0,0,0,0,0,0,0xff,0xff,169,254,1,1})));
        // Mapped multicast 224.0.0.1 -> disallowed
        assert(is_disallowed_ipv6(make_ipv6({0,0,0,0,0,0,0,0,0,0,0xff,0xff,224,0,0,1})));
        // Mapped public 8.8.8.8 -> allowed
        assert(!is_disallowed_ipv6(make_ipv6({0,0,0,0,0,0,0,0,0,0,0xff,0xff,8,8,8,8})));
        // Mapped public 93.184.216.34 -> allowed
        assert(!is_disallowed_ipv6(make_ipv6({0,0,0,0,0,0,0,0,0,0,0xff,0xff,93,184,216,34})));

        // Valid global unicast IPv6 addresses -> allowed
        // 2606:2800:220:1:248:1893:25c8:1946 (example.com)
        assert(!is_disallowed_ipv6(make_ipv6({0x26,0x06,0x28,0x00,0x02,0x20,0x00,0x01,
                                              0x02,0x48,0x18,0x93,0x25,0xc8,0x19,0x46})));
        // 2001:4860:4860::8888 (Google DNS)
        assert(!is_disallowed_ipv6(make_ipv6({0x20,0x01,0x48,0x60,0x48,0x60,0,0,
                                              0,0,0,0,0,0,0x88,0x88})));
    }

    // 5. WfpWebsiteAllowlist unit & lifecycle tests
    {
        WfpWebsiteAllowlist allowlist;
        assert(!allowlist.active());

        // Legacy mode with empty allowed addresses returns false with validation error
        WfpAllowlistConfig legacy_empty{};
        legacy_empty.default_deny = false;
        std::string err;
        assert(!allowlist.apply(legacy_empty, &err));
        assert(!err.empty());
        assert(!allowlist.active());

        // Move semantics: move-construct and move-assign
        WfpWebsiteAllowlist other(std::move(allowlist));
        assert(!other.active());
        WfpWebsiteAllowlist third;
        third = std::move(other);
        assert(!third.active());

        // Non-admin graceful error handling: Calling clear() or apply() does not crash
        (void)third.clear(nullptr);
    }

    // 6. ExamLockdownController dual-stack arming integration
    {
        WfpAllowlistConfig received_config{};
        bool apply_called = false;
        ExamLockdownController controller(
            [&](const WfpAllowlistConfig& cfg, std::string* /*err*/) {
                received_config = cfg;
                apply_called = true;
                return true;
            },
            [&](std::string* /*err*/) { return true; });

        const auto client_id = make_test_client_id(std::byte{0x11});
        const auto session_id = make_test_session_id(std::byte{0x22});
        const auto ipv6_origin = make_ipv6({0x26,0x06,0x28,0x00,0x02,0x20,0x00,0x01,
                                            0x02,0x48,0x18,0x93,0x25,0xc8,0x19,0x46});
        const auto ipv6_server = make_ipv6({0x20,0x01,0x48,0x60,0x48,0x60,0,0,
                                            0,0,0,0,0,0,0x88,0x88});

        // 6a. Refuses to arm when all IP sets and server IPs are empty/zero
        {
            const auto token = controller.begin_attempt();
            ExamLockdownArmParams empty_params{
                .session_id = session_id,
                .client_id = client_id,
                .package_verified = true,
                .duration_seconds = 3600,
                .allowed_ipv4 = {},
                .allowed_ipv6 = {},
                .server_ipv4 = 0,
                .server_ipv6 = {},
            };
            std::string err;
            assert(!controller.arm(token, empty_params, client_id, 1000, &err));
            assert(!controller.is_armed());
        }

        // 6b. Arms successfully with IPv6 origins alone
        {
            const auto token = controller.begin_attempt();
            ExamLockdownArmParams v6_only_params{
                .session_id = session_id,
                .client_id = client_id,
                .package_verified = true,
                .duration_seconds = 3600,
                .allowed_ipv4 = {},
                .allowed_ipv6 = {ipv6_origin},
                .server_ipv4 = 0,
                .server_ipv6 = {},
                .server_port = 52140,
                .default_deny = true,
                .permit_loopback = true,
                .permit_dns_dhcp = true,
            };
            std::string err;
            assert(controller.arm(token, v6_only_params, client_id, 1000, &err));
            assert(controller.is_armed());
            assert(controller.tick(1000) == LockdownAction::apply);
            assert(apply_called);
            assert(received_config.allowed_ipv4.empty());
            assert(received_config.allowed_ipv6.size() == 1);
            assert(received_config.allowed_ipv6[0] == ipv6_origin);
            assert(received_config.default_deny == true);
            assert(received_config.permit_loopback == true);
            assert(received_config.permit_dns_dhcp == true);
            assert(received_config.server_port == 52140);
            controller.disarm();
        }

        // 6c. Arms successfully with dual-stack origins and server endpoints
        {
            apply_called = false;
            const auto token = controller.begin_attempt();
            ExamLockdownArmParams dual_params{
                .session_id = session_id,
                .client_id = client_id,
                .package_verified = true,
                .duration_seconds = 7200,
                .allowed_ipv4 = {0x08080808u},
                .allowed_ipv6 = {ipv6_origin},
                .server_ipv4 = 0xc0a80164u, // 192.168.1.100
                .server_ipv6 = ipv6_server,
                .server_port = 52140,
                .default_deny = true,
                .permit_loopback = true,
                .permit_dns_dhcp = true,
            };
            std::string err;
            assert(controller.arm(token, dual_params, client_id, 2000, &err));
            assert(controller.is_armed());
            assert(controller.tick(2000) == LockdownAction::apply);
            assert(apply_called);
            assert(received_config.allowed_ipv4.size() == 1);
            assert(received_config.allowed_ipv4[0] == 0x08080808u);
            assert(received_config.allowed_ipv6.size() == 1);
            assert(received_config.allowed_ipv6[0] == ipv6_origin);
            assert(received_config.server_ipv4 == 0xc0a80164u);
            assert(received_config.server_ipv6 == ipv6_server);
            assert(received_config.server_port == 52140);
            assert(received_config.default_deny == true);
            controller.disarm();
        }

        // 6d. Arms successfully for offline exam (server endpoint only, empty origins)
        {
            apply_called = false;
            const auto token = controller.begin_attempt();
            ExamLockdownArmParams offline_params{
                .session_id = session_id,
                .client_id = client_id,
                .package_verified = true,
                .duration_seconds = 1800,
                .allowed_ipv4 = {},
                .allowed_ipv6 = {},
                .server_ipv4 = 0x0a000001u, // 10.0.0.1
                .server_ipv6 = {},
                .server_port = 52140,
                .default_deny = true,
                .permit_loopback = true,
                .permit_dns_dhcp = true,
            };
            std::string err;
            assert(controller.arm(token, offline_params, client_id, 3000, &err));
            assert(controller.is_armed());
            assert(controller.tick(3000) == LockdownAction::apply);
            assert(apply_called);
            assert(received_config.allowed_ipv4.empty());
            assert(received_config.allowed_ipv6.empty());
            assert(received_config.server_ipv4 == 0x0a000001u);
            assert(received_config.default_deny == true);
            controller.disarm();
        }
    }

    return 0;
}
