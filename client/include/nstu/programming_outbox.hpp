#pragma once

#include "nstu/programming.hpp"

#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace nstu::client {

class ProgrammingOutbox {
public:
    ~ProgrammingOutbox();
    [[nodiscard]] bool open(const std::filesystem::path& directory, std::string* error = nullptr);
    [[nodiscard]] bool enqueue(const exam::programming::Request& request, std::string* error = nullptr);
    [[nodiscard]] std::vector<exam::programming::Request> pending_for_client(
        const security::ClientId& client_id, std::size_t maximum = 256) const;
    [[nodiscard]] bool acknowledge(const exam::programming::Response& response, std::string* error = nullptr);
    void close() noexcept;

private:
    mutable std::mutex mutex_;
    std::filesystem::path root_;
    std::map<std::string, exam::programming::Request> pending_;
    std::size_t bytes_ = 0;
    void* lock_handle_ = nullptr;
};

} // namespace nstu::client
