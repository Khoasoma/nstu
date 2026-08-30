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
                                   std::uint64_t finalized_sample_size,
                                   net::VideoDeliveryMode delivery) {
    std::scoped_lock lock(mutex_);
    const auto found = clients_.find(id);
    if (found == clients_.end()) {
        return false;
    }
    found->second.latency_ms = latency_ms;
    found->second.packet_loss_per_mille = loss_per_mille;
    found->second.packet_loss_sample_size = finalized_sample_size;
    found->second.delivery = delivery;
    found->second.last_seen = std::chrono::steady_clock::now();
    constexpr std::uint64_t minimum_sample_size = 200;
    constexpr std::uint32_t degrade_threshold_per_mille = 50;
    constexpr std::uint32_t recover_threshold_per_mille = 20;
    constexpr std::uint8_t bad_windows_required = 3;
    constexpr std::uint8_t good_windows_required = 5;
    if (finalized_sample_size >= minimum_sample_size) {
        if (loss_per_mille >= degrade_threshold_per_mille) {
            found->second.bad_loss_windows = static_cast<std::uint8_t>(
                std::min<unsigned int>(found->second.bad_loss_windows + 1,
                                       bad_windows_required));
            found->second.good_loss_windows = 0;
        } else if (loss_per_mille <= recover_threshold_per_mille) {
            found->second.good_loss_windows = static_cast<std::uint8_t>(
                std::min<unsigned int>(found->second.good_loss_windows + 1,
                                       good_windows_required));
            found->second.bad_loss_windows = 0;
        } else {
            found->second.bad_loss_windows = 0;
            found->second.good_loss_windows = 0;
        }
    }
    if (found->second.status != ClientStatus::locked &&
        found->second.status != ClientStatus::offline) {
        if (found->second.bad_loss_windows >= bad_windows_required) {
            found->second.status = ClientStatus::degraded;
        } else if (found->second.good_loss_windows >= good_windows_required) {
            found->second.status = ClientStatus::online;
        }
    }
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
