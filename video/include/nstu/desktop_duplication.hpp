#pragma once

#include <cstdint>
#include <string>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

namespace nstu::video {

struct CapturedFrameInfo {
    std::uint64_t present_time_qpc = 0;
    std::uint32_t accumulated_frames = 0;
    bool pointer_shape_changed = false;
};

class DesktopDuplicator {
public:
    DesktopDuplicator() = default;
    ~DesktopDuplicator();
    DesktopDuplicator(const DesktopDuplicator&) = delete;
    DesktopDuplicator& operator=(const DesktopDuplicator&) = delete;

    [[nodiscard]] bool initialize(std::uint32_t adapter_index,
                                  std::uint32_t output_index,
                                  std::string* error = nullptr);
    [[nodiscard]] bool acquire_next_frame(std::uint32_t timeout_ms,
                                          CapturedFrameInfo& info,
                                          std::string* error = nullptr);
    void release_frame() noexcept;
    void reset() noexcept;

    [[nodiscard]] ID3D11Device* device() const noexcept;
    [[nodiscard]] ID3D11DeviceContext* context() const noexcept;
    [[nodiscard]] ID3D11Texture2D* texture() const noexcept;
    [[nodiscard]] bool initialized() const noexcept;

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
    bool frame_acquired_ = false;
};

} // namespace nstu::video
