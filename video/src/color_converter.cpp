#include "nstu/color_converter.hpp"

namespace nstu::video {
namespace {

void set_error(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
}

} // namespace

bool D3D11ColorConverter::initialize(ID3D11Device* device,
                                     ID3D11DeviceContext* context,
                                     std::uint32_t width,
                                     std::uint32_t height,
                                     std::string* error) {
    reset();
    if (device == nullptr || context == nullptr || width == 0 || height == 0) {
        set_error(error, "invalid color converter configuration");
        return false;
    }
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&video_device_))) ||
        FAILED(context->QueryInterface(IID_PPV_ARGS(&video_context_)))) {
        set_error(error, "D3D11 video interfaces are unavailable");
        return false;
    }
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputFrameRate = {60, 1};
    content.InputWidth = width;
    content.InputHeight = height;
    content.OutputFrameRate = {60, 1};
    content.OutputWidth = width;
    content.OutputHeight = height;
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    if (FAILED(video_device_->CreateVideoProcessorEnumerator(
            &content, &enumerator_)) ||
        FAILED(video_device_->CreateVideoProcessor(enumerator_.Get(), 0,
                                                   &processor_))) {
        set_error(error, "D3D11 video processor creation failed");
        reset();
        return false;
    }
    D3D11_TEXTURE2D_DESC texture{};
    texture.Width = width;
    texture.Height = height;
    texture.MipLevels = 1;
    texture.ArraySize = 1;
    texture.Format = DXGI_FORMAT_NV12;
    texture.SampleDesc.Count = 1;
    texture.Usage = D3D11_USAGE_DEFAULT;
    texture.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(device->CreateTexture2D(&texture, nullptr, &nv12_texture_))) {
        set_error(error, "NV12 GPU texture creation failed");
        reset();
        return false;
    }
    return true;
}

bool D3D11ColorConverter::convert_bgra_to_nv12(ID3D11Texture2D* source,
                                                std::string* error) {
    if (source == nullptr || !video_device_ || !video_context_ || !processor_ ||
        !nv12_texture_) {
        set_error(error, "color converter is not initialized");
        return false;
    }
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_description{};
    input_description.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    input_description.Texture2D.MipSlice = 0;
    input_description.Texture2D.ArraySlice = 0;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> input_view;
    if (FAILED(video_device_->CreateVideoProcessorInputView(
            source, enumerator_.Get(), &input_description, &input_view))) {
        set_error(error, "video processor input view creation failed");
        return false;
    }
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_description{};
    output_description.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    output_description.Texture2D.MipSlice = 0;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> output_view;
    if (FAILED(video_device_->CreateVideoProcessorOutputView(
            nv12_texture_.Get(), enumerator_.Get(), &output_description,
            &output_view))) {
        set_error(error, "video processor output view creation failed");
        return false;
    }
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = input_view.Get();
    if (FAILED(video_context_->VideoProcessorBlt(processor_.Get(),
                                                 output_view.Get(), 0, 1,
                                                 &stream))) {
        set_error(error, "GPU BGRA-to-NV12 conversion failed");
        return false;
    }
    return true;
}

ID3D11Texture2D* D3D11ColorConverter::nv12_texture() const noexcept {
    return nv12_texture_.Get();
}

void D3D11ColorConverter::reset() noexcept {
    nv12_texture_.Reset();
    processor_.Reset();
    enumerator_.Reset();
    video_context_.Reset();
    video_device_.Reset();
}

} // namespace nstu::video
