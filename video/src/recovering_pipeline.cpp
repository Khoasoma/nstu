#include "nstu/recovering_pipeline.hpp"

#include <windows.h>

#include <algorithm>
#include <limits>

namespace nstu::video {
namespace {

void set_error(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
}

} // namespace

RecoveryController::RecoveryController(RecoveryPolicy policy)
    : policy_(policy) {
    policy_.initial_backoff = std::max(policy_.initial_backoff,
                                       std::chrono::milliseconds(1));
    policy_.maximum_backoff = std::max(policy_.maximum_backoff,
                                       policy_.initial_backoff);
    policy_.maximum_consecutive_failures = std::max(
        policy_.maximum_consecutive_failures, 1u);
}

void RecoveryController::start(
    std::chrono::steady_clock::time_point now) noexcept {
    state_ = RecoveryState::recovering;
    consecutive_failures_ = 0;
    total_recoveries_ = 0;
    next_attempt_ = now;
}

void RecoveryController::stop() noexcept {
    state_ = RecoveryState::stopped;
    consecutive_failures_ = 0;
    next_attempt_ = {};
}

bool RecoveryController::attempt_due(
    std::chrono::steady_clock::time_point now) const noexcept {
    return state_ == RecoveryState::recovering && now >= next_attempt_;
}

void RecoveryController::record_success() noexcept {
    if (state_ == RecoveryState::recovering && consecutive_failures_ != 0) {
        ++total_recoveries_;
    }
    state_ = RecoveryState::running;
    consecutive_failures_ = 0;
}

void RecoveryController::record_failure(
    std::chrono::steady_clock::time_point now) noexcept {
    if (state_ == RecoveryState::stopped || state_ == RecoveryState::failed) {
        return;
    }
    state_ = RecoveryState::recovering;
    ++consecutive_failures_;
    if (consecutive_failures_ >= policy_.maximum_consecutive_failures) {
        state_ = RecoveryState::failed;
        return;
    }
    auto backoff = policy_.initial_backoff;
    for (std::uint32_t attempt = 1; attempt < consecutive_failures_ &&
                                    backoff < policy_.maximum_backoff;
         ++attempt) {
        backoff = std::min(backoff * 2, policy_.maximum_backoff);
    }
    next_attempt_ = now + backoff;
}

RecoveryState RecoveryController::state() const noexcept { return state_; }

std::uint32_t RecoveryController::consecutive_failures() const noexcept {
    return consecutive_failures_;
}

std::uint64_t RecoveryController::total_recoveries() const noexcept {
    return total_recoveries_;
}

std::chrono::steady_clock::time_point RecoveryController::next_attempt() const
    noexcept {
    return next_attempt_;
}

RecoveringVideoPipeline::RecoveringVideoPipeline() = default;
RecoveringVideoPipeline::~RecoveringVideoPipeline() { stop(); }

bool RecoveringVideoPipeline::start(
    RecoveringVideoPipelineConfig config,
    std::chrono::steady_clock::time_point now, std::string* error) {
    stop();
    if (config.frames_per_second < 5 || config.frames_per_second > 60 ||
        config.bitrate == 0 || !media_foundation_.ready()) {
        set_error(error, "invalid or unavailable video pipeline configuration");
        return false;
    }
    config_ = config;
    recovery_ = RecoveryController(config.recovery);
    QueryPerformanceFrequency(&qpc_frequency_);
    if (qpc_frequency_.QuadPart <= 0) {
        set_error(error, "performance counter frequency is unavailable");
        return false;
    }
    started_ = true;
    recovery_.start(now);
    return true;
}

PipelinePollResult RecoveringVideoPipeline::poll(
    std::uint32_t capture_timeout_ms,
    std::chrono::steady_clock::time_point now, PipelineFrame& frame,
    std::string* error) {
    frame = {};
    if (!started_) {
        return PipelinePollResult::stopped;
    }
    if (recovery_.state() == RecoveryState::failed) {
        return PipelinePollResult::fatal;
    }
    if (recovery_.state() == RecoveryState::recovering) {
        if (!recovery_.attempt_due(now)) {
            return PipelinePollResult::recovering;
        }
        if (!initialize_capture(error)) {
            recovery_.record_failure(now);
            return recovery_.state() == RecoveryState::failed
                ? PipelinePollResult::fatal
                : PipelinePollResult::recovering;
        }
        recovery_.record_success();
    }

    std::string capture_error;
    if (!duplicator_.acquire_next_frame(capture_timeout_ms, frame.capture,
                                        &capture_error)) {
        if (capture_error == "capture timeout") {
            return PipelinePollResult::capture_timeout;
        }
        if (error != nullptr) {
            *error = capture_error;
        }
        enter_recovery(now);
        return recovery_.state() == RecoveryState::failed
            ? PipelinePollResult::fatal
            : PipelinePollResult::recovering;
    }
    if (!initialize_frame_pipeline(duplicator_.texture(), error) ||
        !converter_.convert_bgra_to_nv12(duplicator_.texture(), error)) {
        duplicator_.release_frame();
        enter_recovery(now);
        return recovery_.state() == RecoveryState::failed
            ? PipelinePollResult::fatal
            : PipelinePollResult::recovering;
    }
    const long double timestamp =
        static_cast<long double>(frame.capture.present_time_qpc) *
        10'000'000.0L / static_cast<long double>(qpc_frequency_.QuadPart);
    frame.timestamp_100ns = timestamp >=
            static_cast<long double>(std::numeric_limits<std::int64_t>::max())
        ? std::numeric_limits<std::int64_t>::max()
        : static_cast<std::int64_t>(timestamp);
    const bool encoded = encoder_.encode_nv12_texture(
        converter_.nv12_texture(), frame.timestamp_100ns, frame.access_unit,
        error);
    duplicator_.release_frame();
    if (!encoded) {
        enter_recovery(now);
        return recovery_.state() == RecoveryState::failed
            ? PipelinePollResult::fatal
            : PipelinePollResult::recovering;
    }
    return PipelinePollResult::frame_processed;
}

bool RecoveringVideoPipeline::request_keyframe(std::string* error) {
    return recovery_.state() == RecoveryState::running &&
           encoder_.request_keyframe(error);
}

void RecoveringVideoPipeline::stop() noexcept {
    reset_components();
    recovery_.stop();
    started_ = false;
    config_ = {};
}

RecoveryState RecoveringVideoPipeline::state() const noexcept {
    return recovery_.state();
}

std::uint32_t RecoveringVideoPipeline::width() const noexcept { return width_; }
std::uint32_t RecoveringVideoPipeline::height() const noexcept { return height_; }

const RecoveryController& RecoveringVideoPipeline::recovery() const noexcept {
    return recovery_;
}

bool RecoveringVideoPipeline::initialize_capture(std::string* error) {
    reset_components();
    return duplicator_.initialize(config_.adapter_index, config_.output_index,
                                   error);
}

bool RecoveringVideoPipeline::initialize_frame_pipeline(
    ID3D11Texture2D* texture, std::string* error) {
    if (texture == nullptr) {
        set_error(error, "capture texture is unavailable");
        return false;
    }
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    if (description.Width == width_ && description.Height == height_ &&
        converter_.nv12_texture() != nullptr && encoder_.initialized()) {
        return true;
    }
    converter_.reset();
    encoder_.reset();
    width_ = description.Width;
    height_ = description.Height;
    if (!converter_.initialize(duplicator_.device(), duplicator_.context(),
                               width_, height_, error) ||
        !encoder_.initialize(duplicator_.device(), width_, height_,
                             config_.frames_per_second, config_.bitrate,
                             error)) {
        width_ = 0;
        height_ = 0;
        return false;
    }
    return true;
}

void RecoveringVideoPipeline::enter_recovery(
    std::chrono::steady_clock::time_point now) noexcept {
    reset_components();
    recovery_.record_failure(now);
}

void RecoveringVideoPipeline::reset_components() noexcept {
    encoder_.reset();
    converter_.reset();
    duplicator_.reset();
    width_ = 0;
    height_ = 0;
}

} // namespace nstu::video
