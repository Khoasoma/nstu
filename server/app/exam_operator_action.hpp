#pragma once

#include <chrono>
#include <functional>
#include <future>
#include <optional>
#include <string>
#include <utility>

namespace nstu::ui {
class ExamOperatorAction {
public:
    [[nodiscard]] bool busy() const noexcept { return result_.valid(); }
    [[nodiscard]] bool start(std::function<std::string()> action) {
        if (busy()) return false;
        try {
            result_ = std::async(std::launch::async, [action = std::move(action)] {
                try { return action(); }
                catch (...) { return std::string{"Exam operation failed; retry or ask for assistance."}; }
            });
            return true;
        } catch (...) { return false; }
    }
    [[nodiscard]] std::optional<std::string> poll() {
        if (!busy() || result_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return std::nullopt;
        return result_.get();
    }
    void wait() { if (busy()) result_.wait(); }
private:
    std::future<std::string> result_;
};
} // namespace nstu::ui
