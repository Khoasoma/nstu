#include "nstu/client_registry.hpp"

#include <cassert>
#include <chrono>

int main() {
    using namespace std::chrono_literals;
    nstu::server::ClientRegistry registry;
    const auto now = std::chrono::steady_clock::now();
    registry.upsert({
        .id = 1,
        .hostname = "LAB-PC-01",
        .address = "192.168.1.101",
        .status = nstu::server::ClientStatus::online,
        .last_seen = now,
    });
    assert(registry.size() == 1);
    assert(registry.update_health(
        1, 4, 80, 1000, nstu::net::VideoDeliveryMode::unicast));
    assert(registry.update_health(
        1, 4, 80, 1000, nstu::net::VideoDeliveryMode::unicast));
    assert(registry.update_health(
        1, 4, 80, 1000, nstu::net::VideoDeliveryMode::unicast));
    auto clients = registry.snapshot();
    assert(clients.size() == 1);
    assert(clients[0].status == nstu::server::ClientStatus::degraded);
    assert(clients[0].delivery == nstu::net::VideoDeliveryMode::unicast);

    for (int sample = 0; sample < 4; ++sample) {
        assert(registry.update_health(
            1, 4, 10, 1000, nstu::net::VideoDeliveryMode::multicast));
    }
    clients = registry.snapshot();
    assert(clients[0].status == nstu::server::ClientStatus::degraded);
    assert(registry.update_health(
        1, 4, 10, 1000, nstu::net::VideoDeliveryMode::multicast));
    clients = registry.snapshot();
    assert(clients[0].status == nstu::server::ClientStatus::online);

    assert(registry.update_health(
        1, 4, 100, 20, nstu::net::VideoDeliveryMode::multicast));
    assert(registry.update_health(
        1, 4, 100, 20, nstu::net::VideoDeliveryMode::multicast));
    assert(registry.update_health(
        1, 4, 100, 20, nstu::net::VideoDeliveryMode::multicast));
    clients = registry.snapshot();
    assert(clients[0].status == nstu::server::ClientStatus::online);
    assert(registry.expire(now + 10s, 5s) == 1);
    clients = registry.snapshot();
    assert(clients[0].status == nstu::server::ClientStatus::offline);
    return 0;
}
