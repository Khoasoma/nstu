#include "nstu/setup/driver_scan.hpp"

#include <mfapi.h>
#include <mfidl.h>

#include <utility>

namespace nstu::setup {
namespace { void set_error(std::string* error, const char* message) { if (error != nullptr) *error = message; } }

std::vector<EncoderScan> scan_hardware_h264_encoders(std::string* error) {
    MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO output{MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate** activations = nullptr;
    UINT32 count = 0;
    const HRESULT result = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
        &input, &output, &activations, &count);
    if (FAILED(result)) { set_error(error, "MFTEnumEx failed"); return {}; }
    std::vector<EncoderScan> encoders;
    encoders.reserve(count);
    for (UINT32 index = 0; index < count; ++index) {
        EncoderScan encoder;
        wchar_t* name = nullptr;
        UINT32 name_length = 0;
        if (SUCCEEDED(activations[index]->GetAllocatedString(
                MFT_FRIENDLY_NAME_Attribute, &name, &name_length)) &&
            name != nullptr) {
            encoder.friendly_name.assign(name, name_length);
            CoTaskMemFree(name);
        }
        encoders.push_back(std::move(encoder));
        activations[index]->Release();
    }
    CoTaskMemFree(activations);
    return encoders;
}
} // namespace nstu::setup
