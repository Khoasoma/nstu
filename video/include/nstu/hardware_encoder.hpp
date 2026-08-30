#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct ID3D11Device;
struct ID3D11Texture2D;

namespace nstu::video {

struct HardwareEncoderInfo {
    std::wstring friendly_name;
};

class MediaFoundationRuntime {
public:
    MediaFoundationRuntime() noexcept;
    ~MediaFoundationRuntime();
    MediaFoundationRuntime(const MediaFoundationRuntime&) = delete;
    MediaFoundationRuntime& operator=(const MediaFoundationRuntime&) = delete;

    [[nodiscard]] bool ready() const noexcept;

private:
    bool ready_ = false;
};

[[nodiscard]] std::vector<HardwareEncoderInfo> enumerate_hardware_h264_encoders(
    std::string* error = nullptr);

class HardwareH264Encoder {
public:
    HardwareH264Encoder();
    ~HardwareH264Encoder();
    HardwareH264Encoder(HardwareH264Encoder&&) noexcept;
    HardwareH264Encoder& operator=(HardwareH264Encoder&&) noexcept;
    HardwareH264Encoder(const HardwareH264Encoder&) = delete;
    HardwareH264Encoder& operator=(const HardwareH264Encoder&) = delete;

    [[nodiscard]] bool initialize(ID3D11Device* device, std::uint32_t width,
                                  std::uint32_t height, std::uint32_t fps,
                                  std::uint32_t bitrate,
                                  std::string* error = nullptr);
    [[nodiscard]] bool encode_nv12_texture(
        ID3D11Texture2D* texture, std::int64_t timestamp_100ns,
        std::vector<std::byte>& access_unit, std::string* error = nullptr);
    [[nodiscard]] bool request_keyframe(std::string* error = nullptr);
    void reset() noexcept;
    [[nodiscard]] bool initialized() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nstu::video
