#pragma once

#include "nstu/color_converter.hpp"
#include "nstu/desktop_duplication.hpp"
#include "nstu/hardware_encoder.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nstu::video {

enum class RecoveryState : std::uint8_t {
    stopped,
    recovering,
    running,
    failed,
};

struct RecoveryPolicy {
    std::chrono::milliseconds initial_backoff{100};
    std::chrono::milliseconds maximum_backoff{5000};
    std::uint32_t maximum_consecutive_failures = 20;
};

class RecoveryController {
public:
    explicit RecoveryController(RecoveryPolicy policy = {});

    void start(std::chrono::steady_clock::time_point now) noexcept;
    void stop() noexcept;
    [[nodiscard]] bool attempt_due(
        std::chrono::steady_clock::time_point now) const noexcept;
    void record_success() noexcept;
    void record_failure(std::chrono::steady_clock::time_point now) noexcept;

    [[nodiscard]] RecoveryState state() const noexcept;
    [[nodiscard]] std::uint32_t consecutive_failures() const noexcept;
    [[nodiscard]] std::uint64_t total_recoveries() const noexcept;
    [[nodiscard]] std::chrono::steady_clock::time_point next_attempt() const
        noexcept;

private:
    RecoveryPolicy policy_;
    RecoveryState state_ = RecoveryState::stopped;
    std::uint32_t consecutive_failures_ = 0;
    std::uint64_t total_recoveries_ = 0;
    std::chrono::steady_clock::time_point next_attempt_{};
};

struct RecoveringVideoPipelineConfig {
    std::uint32_t adapter_index = 0;
    std::uint32_t output_index = 0;
    std::uint32_t frames_per_second = 10;
    std::uint32_t bitrate = 4'000'000;
    RecoveryPolicy recovery{};
};

enum class PipelinePollResult : std::uint8_t {
    frame_processed,
    capture_timeout,
    recovering,
    fatal,
    stopped,
};

struct PipelineFrame {
    CapturedFrameInfo capture;
    std::int64_t timestamp_100ns = 0;
    std::vector<std::byte> access_unit;
};

class RecoveringVideoPipeline {
public:
    RecoveringVideoPipeline();
    ~RecoveringVideoPipeline();
    RecoveringVideoPipeline(const RecoveringVideoPipeline&) = delete;
    RecoveringVideoPipeline& operator=(const RecoveringVideoPipeline&) = delete;

    [[nodiscard]] bool start(
        RecoveringVideoPipelineConfig config,
        std::chrono::steady_clock::time_point now,
        std::string* error = nullptr);
    [[nodiscard]] PipelinePollResult poll(
        std::uint32_t capture_timeout_ms,
        std::chrono::steady_clock::time_point now, PipelineFrame& frame,
        std::string* error = nullptr);
    [[nodiscard]] bool request_keyframe(std::string* error = nullptr);
    void stop() noexcept;

    [[nodiscard]] RecoveryState state() const noexcept;
    [[nodiscard]] std::uint32_t width() const noexcept;
    [[nodiscard]] std::uint32_t height() const noexcept;
    [[nodiscard]] const RecoveryController& recovery() const noexcept;

private:
    [[nodiscard]] bool initialize_capture(std::string* error);
    [[nodiscard]] bool initialize_frame_pipeline(ID3D11Texture2D* texture,
                                                 std::string* error);
    void enter_recovery(std::chrono::steady_clock::time_point now) noexcept;
    void reset_components() noexcept;

    RecoveringVideoPipelineConfig config_;
    MediaFoundationRuntime media_foundation_;
    DesktopDuplicator duplicator_;
    D3D11ColorConverter converter_;
    HardwareH264Encoder encoder_;
    RecoveryController recovery_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    LARGE_INTEGER qpc_frequency_{};
    bool started_ = false;
};

} // namespace nstu::video
