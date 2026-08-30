#include "nstu/client_registry.hpp"

#include <algorithm>

namespace nstu::server {

void ClientRegistry::upsert(ClientRecord record) {
    std::scoped_lock lock(mutex_);
    clients_.insert_or_assign(record.id, std::move(record));
}

bool ClientRegistry::set_status(std::uint64_t id, ClientStatus status) {
    std::scoped_lock lock(mutex_);
    const auto found = clients_.find(id);
    if (found == clients_.end()) {
        return false;
    }
    found->second.status = status;
    return true;
}

bool ClientRegistry::update_health(std::uint64_t id, std::uint32_t latency_ms,
                                   std::uint32_t loss_per_mille,
                                   net::VideoDeliveryMode delivery) {
    std::scoped_lock lock(mutex_);
    const auto found = clients_.find(id);
    if (found == clients_.end()) {
        return false;
    }
    found->second.latency_ms = latency_ms;
    found->second.packet_loss_per_mille = loss_per_mille;
    found->second.delivery = delivery;
    found->second.last_seen = std::chrono::steady_clock::now();
    found->second.status = loss_per_mille > 50 ? ClientStatus::degraded
                                               : ClientStatus::online;
    return true;
}

std::size_t ClientRegistry::expire(
    std::chrono::steady_clock::time_point now,
    std::chrono::milliseconds timeout) {
    std::scoped_lock lock(mutex_);
    std::size_t expired = 0;
    for (auto& [id, client] : clients_) {
        (void)id;
        if (client.status != ClientStatus::offline &&
            now - client.last_seen > timeout) {
            client.status = ClientStatus::offline;
            ++expired;
        }
    }
    return expired;
}

std::vector<ClientRecord> ClientRegistry::snapshot() const {
    std::scoped_lock lock(mutex_);
    std::vector<ClientRecord> result;
    result.reserve(clients_.size());
    for (const auto& [id, client] : clients_) {
        (void)id;
        result.push_back(client);
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.hostname < right.hostname;
    });
    return result;
}

std::size_t ClientRegistry::size() const {
    std::scoped_lock lock(mutex_);
    return clients_.size();
}

const char* to_string(ClientStatus status) noexcept {
    switch (status) {
    case ClientStatus::connecting:
        return "Connecting";
    case ClientStatus::online:
        return "Online";
    case ClientStatus::degraded:
        return "Degraded";
    case ClientStatus::locked:
        return "Locked";
    case ClientStatus::offline:
        return "Offline";
    }
    return "Unknown";
}

} // namespace nstu::server
