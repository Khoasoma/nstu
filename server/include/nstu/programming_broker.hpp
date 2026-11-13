#pragma once

#include "nstu/programming.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace nstu::server {

struct ProgrammingBrokerConfig {
    std::string url;
    std::string token;
};

[[nodiscard]] ProgrammingBrokerConfig programming_broker_environment();
[[nodiscard]] bool validate_programming_broker_config(const ProgrammingBrokerConfig& config);

class ProgrammingBroker {
public:
    using Transport = std::function<std::optional<std::string>(std::string_view,
                                                              std::string_view)>;
    using Completion = std::function<void(std::string)>;
    explicit ProgrammingBroker(ProgrammingBrokerConfig config, Transport transport = {});
    ~ProgrammingBroker();
    ProgrammingBroker(const ProgrammingBroker&) = delete;
    ProgrammingBroker& operator=(const ProgrammingBroker&) = delete;
    [[nodiscard]] bool configured() const noexcept;
    [[nodiscard]] bool submit(exam::programming::Request request, Completion completion,
                              bool recovery_only = false);
    [[nodiscard]] bool prepare_session(const exam::StateRequest& context,
                                      std::string_view mode, std::string_view grading_digest);
    [[nodiscard]] bool ready(const exam::StateRequest& context);
    [[nodiscard]] std::optional<exam::programming::LifecycleReply> begin_session(
        const exam::StateRequest& context, std::string_view request_id,
        std::uint32_t duration_seconds);
    [[nodiscard]] bool stop_session(const exam::StateRequest& context,
                                    std::string_view request_id);
    void stop() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nstu::server
