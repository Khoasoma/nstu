#pragma once

#include <d3d11.h>
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

namespace nstu::ui {

// A bundled presentation asset. Decode/upload happens during device setup;
// the renderer only borrows the texture and dimensions.
class BrandImage {
public:
    void reset() noexcept {
        view_.Reset();
        width_ = 0;
        height_ = 0;
    }

    [[nodiscard]] ID3D11ShaderResourceView* view() const noexcept { return view_.Get(); }
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }

    [[nodiscard]] HRESULT load(ID3D11Device* device) {
        reset();
        if (device == nullptr) return E_POINTER;
        struct ComScope {
            HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
        } com;
        if (FAILED(com.result) && com.result != RPC_E_CHANGED_MODE) return com.result;

        const auto module = GetModuleHandleW(nullptr);
        const auto resource = FindResourceW(module, L"NSTU_BRAND_LOGO", MAKEINTRESOURCEW(10));
        if (resource == nullptr) return E_FAIL;
        const auto size = SizeofResource(module, resource);
        const auto memory = LockResource(LoadResource(module, resource));
        if (size == 0 || memory == nullptr) return E_FAIL;

        Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
        HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        if (FAILED(result)) return result;
        Microsoft::WRL::ComPtr<IWICStream> stream;
        result = factory->CreateStream(&stream);
        if (FAILED(result)) return result;
        result = stream->InitializeFromMemory(static_cast<BYTE*>(memory), size);
        if (FAILED(result)) return result;
        Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
        result = factory->CreateDecoderFromStream(stream.Get(), nullptr,
            WICDecodeMetadataCacheOnLoad, &decoder);
        if (FAILED(result)) return result;
        Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
        result = decoder->GetFrame(0, &frame);
        if (FAILED(result)) return result;
        UINT width = 0;
        UINT height = 0;
        result = frame->GetSize(&width, &height);
        if (FAILED(result)) return result;
        constexpr UINT maximum_dimension = 4096;
        if (width == 0 || height == 0 || width > maximum_dimension ||
            height > maximum_dimension) return E_INVALIDARG;
        Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
        result = factory->CreateFormatConverter(&converter);
        if (FAILED(result)) return result;
        // ImGui's DX11 blend state expects straight alpha.
        result = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
        if (FAILED(result)) return result;
        const UINT stride = width * 4u;
        const UINT byte_count = stride * height;
        std::vector<BYTE> pixels(byte_count);
        result = converter->CopyPixels(nullptr, stride, byte_count, pixels.data());
        if (FAILED(result)) return result;
        D3D11_TEXTURE2D_DESC description{};
        description.Width = width;
        description.Height = height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{};
        data.pSysMem = pixels.data();
        data.SysMemPitch = stride;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        result = device->CreateTexture2D(&description, &data, &texture);
        if (FAILED(result)) return result;
        result = device->CreateShaderResourceView(texture.Get(), nullptr, &view_);
        if (FAILED(result)) return result;
        width_ = width;
        height_ = height;
        return S_OK;
    }

private:
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
};

} // namespace nstu::ui
