#include "nstu/desktop_duplication.hpp"
#include "nstu/color_converter.hpp"
#include "nstu/hardware_encoder.hpp"

#include <windows.h>

#include <iostream>

int main() {
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize_com = SUCCEEDED(com_result);

    nstu::video::MediaFoundationRuntime media_foundation;
    std::string encoder_error;
    const auto encoders = media_foundation.ready()
                              ? nstu::video::enumerate_hardware_h264_encoders(
                                    &encoder_error)
                              : std::vector<nstu::video::HardwareEncoderInfo>{};
    std::cout << "hardware_h264_encoders=" << encoders.size() << '\n';

    nstu::video::DesktopDuplicator duplicator;
    std::string duplication_error;
    const bool duplication_ready = duplicator.initialize(0, 0, &duplication_error);
    std::cout << "desktop_duplication="
              << (duplication_ready ? "available" : "unavailable") << '\n';
    if (!duplication_ready) {
        std::cout << "desktop_duplication_reason=" << duplication_error << '\n';
    }
    nstu::video::HardwareH264Encoder encoder;
    std::string encoder_init_error;
    const bool encoder_ready = duplication_ready && media_foundation.ready() &&
        encoder.initialize(duplicator.device(), 1920, 1080, 60, 4'000'000,
                           &encoder_init_error);
    std::cout << "h264_encoder_config="
              << (encoder_ready ? "available" : "unavailable") << '\n';
    if (!encoder_ready && !encoder_init_error.empty()) {
        std::cout << "h264_encoder_config_reason=" << encoder_init_error << '\n';
    }
    nstu::video::D3D11ColorConverter converter;
    std::string converter_error;
    const bool converter_ready = duplication_ready &&
        converter.initialize(duplicator.device(), duplicator.context(), 1920,
                             1080, &converter_error);
    std::cout << "gpu_bgra_to_nv12="
              << (converter_ready ? "available" : "unavailable") << '\n';
    if (!converter_ready && !converter_error.empty()) {
        std::cout << "gpu_bgra_to_nv12_reason=" << converter_error << '\n';
    }
    nstu::video::CapturedFrameInfo frame_info;
    std::string frame_error;
    bool frame_pipeline_ready = false;
    if (duplication_ready && duplicator.acquire_next_frame(1000, frame_info,
                                                            &frame_error)) {
        D3D11_TEXTURE2D_DESC frame_description{};
        duplicator.texture()->GetDesc(&frame_description);
        nstu::video::D3D11ColorConverter frame_converter;
        nstu::video::HardwareH264Encoder frame_encoder;
        std::vector<std::byte> access_unit;
        frame_pipeline_ready = frame_converter.initialize(
            duplicator.device(), duplicator.context(), frame_description.Width,
            frame_description.Height, &frame_error) &&
            frame_converter.convert_bgra_to_nv12(duplicator.texture(),
                                                  &frame_error) &&
            frame_encoder.initialize(duplicator.device(), frame_description.Width,
                                     frame_description.Height, 60, 4'000'000,
                                     &frame_error) &&
            frame_encoder.encode_nv12_texture(
                frame_converter.nv12_texture(),
                static_cast<std::int64_t>(frame_info.present_time_qpc),
                access_unit, &frame_error);
        duplicator.release_frame();
    }
    std::cout << "single_frame_gpu_pipeline="
              << (frame_pipeline_ready ? "available" : "unavailable") << '\n';
    if (!frame_pipeline_ready && !frame_error.empty()) {
        std::cout << "single_frame_gpu_pipeline_reason=" << frame_error << '\n';
    }
    if (!encoder_error.empty()) {
        std::cout << "encoder_probe_reason=" << encoder_error << '\n';
    }

    if (uninitialize_com) {
        CoUninitialize();
    }
    return 0;
}
