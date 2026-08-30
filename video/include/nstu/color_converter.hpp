#pragma once

#include <cstdint>
#include <string>

#include <d3d11.h>
#include <wrl/client.h>

namespace nstu::video {

class D3D11ColorConverter {
public:
    [[nodiscard]] bool initialize(ID3D11Device* device,
                                  ID3D11DeviceContext* context,
                                  std::uint32_t width, std::uint32_t height,
                                  std::string* error = nullptr);
    [[nodiscard]] bool convert_bgra_to_nv12(ID3D11Texture2D* source,
                                            std::string* error = nullptr);
    [[nodiscard]] ID3D11Texture2D* nv12_texture() const noexcept;
    void reset() noexcept;

private:
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> video_device_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> video_context_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> processor_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> nv12_texture_;
};

} // namespace nstu::video
