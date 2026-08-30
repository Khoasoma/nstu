#include "nstu/desktop_duplication.hpp"

#include <utility>

namespace nstu::video {
namespace {

void set_error(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
}

} // namespace

DesktopDuplicator::~DesktopDuplicator() {
    reset();
}

bool DesktopDuplicator::initialize(std::uint32_t adapter_index,
                                    std::uint32_t output_index,
                                    std::string* error) {
    reset();
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        set_error(error, "CreateDXGIFactory1 failed");
        return false;
    }
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(adapter_index, &adapter) == DXGI_ERROR_NOT_FOUND) {
        set_error(error, "DXGI adapter not found");
        return false;
    }

    constexpr UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT |
                           D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    D3D_FEATURE_LEVEL feature_level{};
    if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                 flags, nullptr, 0, D3D11_SDK_VERSION, &device_,
                                 &feature_level, &context_))) {
        set_error(error, "D3D11CreateDevice failed");
        return false;
    }
    if (feature_level < D3D_FEATURE_LEVEL_11_0) {
        set_error(error, "D3D feature level 11.0 is required");
        reset();
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    if (adapter->EnumOutputs(output_index, &output) == DXGI_ERROR_NOT_FOUND) {
        set_error(error, "DXGI output not found");
        reset();
        return false;
    }
    Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
    if (FAILED(output.As(&output1)) ||
        FAILED(output1->DuplicateOutput(device_.Get(), &duplication_))) {
        set_error(error, "Desktop Duplication is unavailable");
        reset();
        return false;
    }
    return true;
}

bool DesktopDuplicator::acquire_next_frame(std::uint32_t timeout_ms,
                                            CapturedFrameInfo& info,
                                            std::string* error) {
    if (!duplication_ || frame_acquired_) {
        set_error(error, "duplicator is not ready for AcquireNextFrame");
        return false;
    }
    DXGI_OUTDUPL_FRAME_INFO frame_info{};
    Microsoft::WRL::ComPtr<IDXGIResource> resource;
    const HRESULT result = duplication_->AcquireNextFrame(timeout_ms, &frame_info,
                                                           &resource);
    if (result == DXGI_ERROR_WAIT_TIMEOUT) {
        set_error(error, "capture timeout");
        return false;
    }
    if (result == DXGI_ERROR_ACCESS_LOST) {
        set_error(error, "desktop duplication access lost; reinitialize required");
        reset();
        return false;
    }
    if (FAILED(result) || FAILED(resource.As(&texture_))) {
        set_error(error, "AcquireNextFrame failed");
        return false;
    }
    frame_acquired_ = true;
    info.present_time_qpc =
        static_cast<std::uint64_t>(frame_info.LastPresentTime.QuadPart);
    info.accumulated_frames = frame_info.AccumulatedFrames;
    info.pointer_shape_changed = frame_info.PointerShapeBufferSize != 0;
    return true;
}

void DesktopDuplicator::release_frame() noexcept {
    texture_.Reset();
    if (duplication_ && frame_acquired_) {
        duplication_->ReleaseFrame();
    }
    frame_acquired_ = false;
}

void DesktopDuplicator::reset() noexcept {
    release_frame();
    duplication_.Reset();
    context_.Reset();
    device_.Reset();
}

ID3D11Device* DesktopDuplicator::device() const noexcept {
    return device_.Get();
}

ID3D11DeviceContext* DesktopDuplicator::context() const noexcept {
    return context_.Get();
}

ID3D11Texture2D* DesktopDuplicator::texture() const noexcept {
    return texture_.Get();
}

bool DesktopDuplicator::initialized() const noexcept {
    return duplication_ != nullptr;
}

} // namespace nstu::video
