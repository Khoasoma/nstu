#pragma once

#include <string>
#include <vector>

namespace nstu::setup {
struct EncoderScan { std::wstring friendly_name; };
[[nodiscard]] std::vector<EncoderScan> scan_hardware_h264_encoders(
    std::string* error = nullptr);
} // namespace nstu::setup
