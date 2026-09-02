#include "nstu/hardware_encoder.hpp"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <codecapi.h>
#include <icodecapi.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <utility>

namespace nstu::video {
namespace {

void set_error(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
}

} // namespace

struct HardwareH264Encoder::Impl {
    Microsoft::WRL::ComPtr<IMFTransform> transform;
    Microsoft::WRL::ComPtr<IMFMediaEventGenerator> event_generator;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> device_manager;
    DWORD input_stream_id = 0;
    DWORD output_stream_id = 0;
    std::uint32_t fps = 60;
    bool asynchronous = false;
};

MediaFoundationRuntime::MediaFoundationRuntime() noexcept {
    ready_ = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
}

MediaFoundationRuntime::~MediaFoundationRuntime() {
    if (ready_) {
        MFShutdown();
    }
}

bool MediaFoundationRuntime::ready() const noexcept {
    return ready_;
}

std::vector<HardwareEncoderInfo> enumerate_hardware_h264_encoders(
    std::string* error) {
    MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO output{MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate** activations = nullptr;
    UINT32 count = 0;
    const HRESULT result = MFTEnumEx(
        MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
        &input, &output, &activations, &count);
    if (FAILED(result)) {
        set_error(error, "MFTEnumEx failed");
        return {};
    }

    std::vector<HardwareEncoderInfo> encoders;
    encoders.reserve(count);
    for (UINT32 index = 0; index < count; ++index) {
        wchar_t* name = nullptr;
        UINT32 name_length = 0;
        HardwareEncoderInfo info;
        if (SUCCEEDED(activations[index]->GetAllocatedString(
                MFT_FRIENDLY_NAME_Attribute, &name, &name_length)) &&
            name != nullptr) {
            info.friendly_name.assign(name, name_length);
            CoTaskMemFree(name);
        }
        encoders.push_back(std::move(info));
        activations[index]->Release();
    }
    CoTaskMemFree(activations);
    return encoders;
}

HardwareH264Encoder::HardwareH264Encoder() : impl_(std::make_unique<Impl>()) {}

HardwareH264Encoder::~HardwareH264Encoder() = default;

HardwareH264Encoder::HardwareH264Encoder(HardwareH264Encoder&&) noexcept = default;

HardwareH264Encoder& HardwareH264Encoder::operator=(
    HardwareH264Encoder&&) noexcept = default;

bool HardwareH264Encoder::initialize(ID3D11Device* device, std::uint32_t width,
                                     std::uint32_t height, std::uint32_t fps,
                                     std::uint32_t bitrate,
                                     std::string* error) {
    reset();
    if (device == nullptr || width == 0 || height == 0 || fps == 0 ||
        bitrate == 0) {
        set_error(error, "invalid encoder configuration");
        return false;
    }
    MFT_REGISTER_TYPE_INFO input_info{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO output_info{MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate** activations = nullptr;
    UINT32 count = 0;
    if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                         MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                         &input_info, &output_info, &activations, &count)) ||
        count == 0) {
        set_error(error, "no hardware H.264 encoder MFT available");
        return false;
    }
    const HRESULT activate_result = activations[0]->ActivateObject(
        IID_PPV_ARGS(&impl_->transform));
    for (UINT32 index = 0; index < count; ++index) {
        activations[index]->Release();
    }
    CoTaskMemFree(activations);
    if (FAILED(activate_result)) {
        set_error(error, "hardware H.264 MFT activation failed");
        return false;
    }

    Microsoft::WRL::ComPtr<IMFAttributes> attributes;
    if (SUCCEEDED(impl_->transform->GetAttributes(&attributes))) {
        UINT32 asynchronous = FALSE;
        attributes->GetUINT32(MF_TRANSFORM_ASYNC, &asynchronous);
        impl_->asynchronous = asynchronous != FALSE;
        if (impl_->asynchronous) {
            attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            impl_->transform.As(&impl_->event_generator);
        }
        attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
    }
    UINT reset_token = 0;
    if (FAILED(MFCreateDXGIDeviceManager(&reset_token,
                                         &impl_->device_manager)) ||
        FAILED(impl_->device_manager->ResetDevice(device, reset_token)) ||
        FAILED(impl_->transform->ProcessMessage(
            MFT_MESSAGE_SET_D3D_MANAGER,
            reinterpret_cast<ULONG_PTR>(impl_->device_manager.Get())))) {
        set_error(error, "encoder D3D11 device manager setup failed");
        reset();
        return false;
    }

    DWORD input_count = 0;
    DWORD output_count = 0;
    if (FAILED(impl_->transform->GetStreamCount(&input_count, &output_count)) ||
        input_count == 0 || output_count == 0) {
        set_error(error, "encoder stream discovery failed");
        reset();
        return false;
    }
    DWORD input_id = 0;
    DWORD output_id = 0;
    if (FAILED(impl_->transform->GetStreamIDs(1, &input_id, 1, &output_id))) {
        input_id = 0;
        output_id = 0;
    }
    impl_->input_stream_id = input_id;
    impl_->output_stream_id = output_id;

    Microsoft::WRL::ComPtr<IMFMediaType> output_type;
    Microsoft::WRL::ComPtr<IMFMediaType> input_type;
    if (FAILED(MFCreateMediaType(&output_type)) ||
        FAILED(MFCreateMediaType(&input_type)) ||
        FAILED(output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
        FAILED(output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264)) ||
        FAILED(output_type->SetUINT32(MF_MT_AVG_BITRATE, bitrate)) ||
        FAILED(output_type->SetUINT32(MF_MT_INTERLACE_MODE,
                                      MFVideoInterlace_Progressive)) ||
        FAILED(MFSetAttributeSize(output_type.Get(), MF_MT_FRAME_SIZE, width,
                                  height)) ||
        FAILED(MFSetAttributeRatio(output_type.Get(), MF_MT_FRAME_RATE, fps,
                                   1)) ||
        FAILED(MFSetAttributeRatio(output_type.Get(),
                                   MF_MT_PIXEL_ASPECT_RATIO, 1, 1)) ||
        FAILED(input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
        FAILED(input_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12)) ||
        FAILED(input_type->SetUINT32(MF_MT_INTERLACE_MODE,
                                     MFVideoInterlace_Progressive)) ||
        FAILED(MFSetAttributeSize(input_type.Get(), MF_MT_FRAME_SIZE, width,
                                  height)) ||
        FAILED(MFSetAttributeRatio(input_type.Get(), MF_MT_FRAME_RATE, fps,
                                   1)) ||
        FAILED(MFSetAttributeRatio(input_type.Get(),
                                   MF_MT_PIXEL_ASPECT_RATIO, 1, 1))) {
        set_error(error, "encoder media type construction failed");
        reset();
        return false;
    }

    if (FAILED(impl_->transform->SetOutputType(output_id, output_type.Get(), 0)) ||
        FAILED(impl_->transform->SetInputType(input_id, input_type.Get(), 0))) {
        set_error(error, "encoder media type negotiation failed");
        reset();
        return false;
    }
    impl_->fps = fps;
    if (FAILED(impl_->transform->ProcessMessage(
            MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)) ||
        FAILED(impl_->transform->ProcessMessage(
            MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0))) {
        set_error(error, "encoder stream start failed");
        reset();
        return false;
    }
    return true;
}

bool HardwareH264Encoder::encode_nv12_texture(
    ID3D11Texture2D* texture, std::int64_t timestamp_100ns,
    std::vector<std::byte>& access_unit, std::string* error) {
    access_unit.clear();
    if (!initialized() || texture == nullptr) {
        set_error(error, "encoder is not initialized");
        return false;
    }
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    if (description.Format != DXGI_FORMAT_NV12) {
        set_error(error, "encoder input texture must use DXGI_FORMAT_NV12");
        return false;
    }
    Microsoft::WRL::ComPtr<IMFMediaBuffer> surface_buffer;
    Microsoft::WRL::ComPtr<IMFSample> input_sample;
    if (FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), texture, 0,
                                         FALSE, &surface_buffer)) ||
        FAILED(MFCreateSample(&input_sample)) ||
        FAILED(input_sample->AddBuffer(surface_buffer.Get()))) {
        set_error(error, "DXGI input sample creation failed");
        return false;
    }
    if (FAILED(input_sample->SetSampleTime(timestamp_100ns)) ||
        FAILED(input_sample->SetSampleDuration(10'000'000LL / impl_->fps))) {
        set_error(error, "encoder input timestamp setup failed");
        return false;
    }
    if (impl_->asynchronous && impl_->event_generator) {
        while (true) {
            Microsoft::WRL::ComPtr<IMFMediaEvent> event;
            if (FAILED(impl_->event_generator->GetEvent(0, &event))) {
                set_error(error, "encoder input event wait failed");
                return false;
            }
            MediaEventType event_type = MEUnknown;
            event->GetType(&event_type);
            if (event_type == METransformNeedInput) {
                break;
            }
        }
    }
    const HRESULT input_result = impl_->transform->ProcessInput(
        impl_->input_stream_id, input_sample.Get(), 0);
    if (FAILED(input_result)) {
        set_error(error, "encoder ProcessInput failed");
        return false;
    }

    MFT_OUTPUT_STREAM_INFO stream_info{};
    if (FAILED(impl_->transform->GetOutputStreamInfo(impl_->output_stream_id,
                                                     &stream_info))) {
        set_error(error, "encoder output stream info failed");
        return false;
    }
    Microsoft::WRL::ComPtr<IMFSample> output_sample;
    if ((stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0) {
        Microsoft::WRL::ComPtr<IMFMediaBuffer> output_buffer;
        if (FAILED(MFCreateSample(&output_sample))) {
            set_error(error, "encoder output sample creation failed");
            return false;
        }
        const DWORD buffer_size =
            std::max<DWORD>(stream_info.cbSize, 1024u * 1024u);
        if (FAILED(MFCreateMemoryBuffer(buffer_size, &output_buffer)) ||
            FAILED(output_sample->AddBuffer(output_buffer.Get()))) {
            set_error(error, "encoder output buffer creation failed");
            return false;
        }
    }
    if (impl_->asynchronous && impl_->event_generator) {
        while (true) {
            Microsoft::WRL::ComPtr<IMFMediaEvent> event;
            if (FAILED(impl_->event_generator->GetEvent(0, &event))) {
                set_error(error, "encoder output event wait failed");
                return false;
            }
            MediaEventType event_type = MEUnknown;
            event->GetType(&event_type);
            if (event_type == METransformHaveOutput) {
                break;
            }
        }
    }
    MFT_OUTPUT_DATA_BUFFER output{};
    output.dwStreamID = impl_->output_stream_id;
    output.pSample = output_sample.Get();
    DWORD status = 0;
    const HRESULT output_result = impl_->transform->ProcessOutput(
        0, 1, &output, &status);
    if (output.pEvents != nullptr) {
        output.pEvents->Release();
    }
    if (output_result == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        return true;
    }
    if (FAILED(output_result)) {
        set_error(error, "encoder ProcessOutput failed");
        return false;
    }
    if (output.pSample != nullptr && output.pSample != output_sample.Get()) {
        output_sample.Attach(output.pSample);
    }
    if (!output_sample) {
        set_error(error, "encoder returned no output sample");
        return false;
    }
    Microsoft::WRL::ComPtr<IMFMediaBuffer> output_buffer;
    if (FAILED(output_sample->ConvertToContiguousBuffer(&output_buffer))) {
        set_error(error, "encoded sample buffer conversion failed");
        return false;
    }
    BYTE* encoded = nullptr;
    DWORD max_length = 0;
    DWORD current_length = 0;
    if (FAILED(output_buffer->Lock(&encoded, &max_length, &current_length))) {
        set_error(error, "encoded buffer lock failed");
        return false;
    }
    access_unit.resize(current_length);
    std::copy_n(reinterpret_cast<const std::byte*>(encoded), current_length,
                access_unit.begin());
    output_buffer->Unlock();
    return true;
}

bool HardwareH264Encoder::request_keyframe(std::string* error) {
    if (!initialized()) {
        set_error(error, "encoder is not initialized");
        return false;
    }
    Microsoft::WRL::ComPtr<ICodecAPI> codec_api;
    if (FAILED(impl_->transform.As(&codec_api))) {
        set_error(error, "encoder keyframe control is unavailable");
        return false;
    }
    VARIANT value;
    VariantInit(&value);
    value.vt = VT_UI4;
    value.ulVal = 1;
    const HRESULT result = codec_api->SetValue(
        &CODECAPI_AVEncVideoForceKeyFrame, &value);
    VariantClear(&value);
    if (FAILED(result)) {
        set_error(error, "encoder keyframe request failed");
        return false;
    }
    return true;
}

void HardwareH264Encoder::reset() noexcept {
    if (impl_ && impl_->transform) {
        impl_->transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        impl_->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }
    if (!impl_) {
        impl_ = std::make_unique<Impl>();
        return;
    }
    impl_->transform.Reset();
    impl_->event_generator.Reset();
    impl_->device_manager.Reset();
    impl_->asynchronous = false;
}

bool HardwareH264Encoder::initialized() const noexcept {
    return impl_ && impl_->transform != nullptr;
}

} // namespace nstu::video
