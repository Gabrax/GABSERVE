#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "mf_h264.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
#include <wmcodecdsp.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>

namespace {

template <class T>
void release_com(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

uint8_t clamp_byte(int value) {
    return static_cast<uint8_t>(std::max(0, std::min(255, value)));
}

bool contains_h264_nal_type(const uint8_t* bytes, uint32_t size, uint8_t wanted_type) {
    for (uint32_t index = 0; index + 4 < size; ++index) {
        uint32_t nal_index = 0;
        if (bytes[index] == 0 && bytes[index + 1] == 0 && bytes[index + 2] == 1) {
            nal_index = index + 3;
        } else if (index + 5 < size && bytes[index] == 0 && bytes[index + 1] == 0 &&
                   bytes[index + 2] == 0 && bytes[index + 3] == 1) {
            nal_index = index + 4;
        }
        if (nal_index && (bytes[nal_index] & 0x1F) == wanted_type) return true;
    }
    return false;
}

void bgr24_to_nv12(const uint8_t* source, uint32_t source_stride,
                   uint32_t width, uint32_t height, std::vector<uint8_t>& nv12) {
    const size_t y_size = static_cast<size_t>(width) * height;
    nv12.resize(y_size + y_size / 2);
    uint8_t* y_plane = nv12.data();
    uint8_t* uv_plane = y_plane + y_size;

    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* row = source + static_cast<size_t>(y) * source_stride;
        uint8_t* target = y_plane + static_cast<size_t>(y) * width;
        for (uint32_t x = 0; x < width; ++x) {
            const int b = row[x * 3 + 0];
            const int g = row[x * 3 + 1];
            const int r = row[x * 3 + 2];
            target[x] = clamp_byte(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
        }
    }

    for (uint32_t y = 0; y < height; y += 2) {
        const uint8_t* row0 = source + static_cast<size_t>(y) * source_stride;
        const uint8_t* row1 = source + static_cast<size_t>(y + 1) * source_stride;
        uint8_t* target = uv_plane + static_cast<size_t>(y / 2) * width;
        for (uint32_t x = 0; x < width; x += 2) {
            int r = 0, g = 0, b = 0;
            for (uint32_t dy = 0; dy < 2; ++dy) {
                const uint8_t* row = dy == 0 ? row0 : row1;
                for (uint32_t dx = 0; dx < 2; ++dx) {
                    b += row[(x + dx) * 3 + 0];
                    g += row[(x + dx) * 3 + 1];
                    r += row[(x + dx) * 3 + 2];
                }
            }
            r /= 4; g /= 4; b /= 4;
            target[x + 0] = clamp_byte(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
            target[x + 1] = clamp_byte(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
        }
    }
}

void nv12_to_bgra(const uint8_t* source, uint32_t source_stride,
                  uint32_t storage_height, uint32_t width, uint32_t height,
                  std::vector<uint8_t>& bgra) {
    bgra.resize(static_cast<size_t>(width) * height * 4);
    const uint8_t* y_plane = source;
    const uint8_t* uv_plane = source + static_cast<size_t>(source_stride) * storage_height;
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* y_row = y_plane + static_cast<size_t>(y) * source_stride;
        const uint8_t* uv_row = uv_plane + static_cast<size_t>(y / 2) * source_stride;
        uint8_t* target = bgra.data() + static_cast<size_t>(y) * width * 4;
        for (uint32_t x = 0; x < width; ++x) {
            const int c = std::max(0, static_cast<int>(y_row[x]) - 16);
            const int d = static_cast<int>(uv_row[x & ~1u]) - 128;
            const int e = static_cast<int>(uv_row[(x & ~1u) + 1]) - 128;
            target[x * 4 + 0] = clamp_byte((298 * c + 516 * d + 128) >> 8);
            target[x * 4 + 1] = clamp_byte((298 * c - 100 * d - 208 * e + 128) >> 8);
            target[x * 4 + 2] = clamp_byte((298 * c + 409 * e + 128) >> 8);
            target[x * 4 + 3] = 0xFF;
        }
    }
}

HRESULT create_sample(const uint8_t* bytes, DWORD size, LONGLONG time,
                      LONGLONG duration, IMFSample** output) {
    *output = nullptr;
    IMFMediaBuffer* buffer = nullptr;
    IMFSample* sample = nullptr;
    HRESULT hr = MFCreateMemoryBuffer(size, &buffer);
    BYTE* target = nullptr;
    if (SUCCEEDED(hr)) hr = buffer->Lock(&target, nullptr, nullptr);
    if (SUCCEEDED(hr)) std::memcpy(target, bytes, size);
    if (target) buffer->Unlock();
    if (SUCCEEDED(hr)) hr = buffer->SetCurrentLength(size);
    if (SUCCEEDED(hr)) hr = MFCreateSample(&sample);
    if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer);
    if (SUCCEEDED(hr)) hr = sample->SetSampleTime(time);
    if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(duration);
    release_com(buffer);
    if (FAILED(hr)) release_com(sample);
    else *output = sample;
    return hr;
}

bool copy_sample_bytes(IMFSample* sample, std::vector<uint8_t>& output) {
    IMFMediaBuffer* buffer = nullptr;
    HRESULT hr = sample->ConvertToContiguousBuffer(&buffer);
    BYTE* bytes = nullptr;
    DWORD length = 0;
    if (SUCCEEDED(hr)) hr = buffer->Lock(&bytes, nullptr, &length);
    if (SUCCEEDED(hr) && length > 0) output.assign(bytes, bytes + length);
    if (bytes) buffer->Unlock();
    release_com(buffer);
    return SUCCEEDED(hr) && length > 0;
}

HRESULT activate_transform(REFGUID category, const MFT_REGISTER_TYPE_INFO& input_info,
                           const MFT_REGISTER_TYPE_INFO& output_info, bool hardware,
                           IMFTransform** transform, std::string& name) {
    *transform = nullptr;
    IMFActivate** activations = nullptr;
    UINT32 count = 0;
    const UINT32 flags = (hardware ? MFT_ENUM_FLAG_HARDWARE : MFT_ENUM_FLAG_SYNCMFT) |
                         MFT_ENUM_FLAG_SORTANDFILTER;
    HRESULT hr = MFTEnumEx(category, flags, &input_info, &output_info, &activations, &count);
    if (FAILED(hr) || count == 0) {
        if (activations) CoTaskMemFree(activations);
        return FAILED(hr) ? hr : MF_E_TOPO_CODEC_NOT_FOUND;
    }

    HRESULT activation_hr = MF_E_TOPO_CODEC_NOT_FOUND;
    for (UINT32 index = 0; index < count && !*transform; ++index) {
        IMFTransform* candidate = nullptr;
        activation_hr = activations[index]->ActivateObject(IID_PPV_ARGS(&candidate));
        if (FAILED(activation_hr)) continue;
        UINT32 async = FALSE;
        IMFAttributes* attributes = nullptr;
        if (SUCCEEDED(candidate->GetAttributes(&attributes))) {
            attributes->GetUINT32(MF_TRANSFORM_ASYNC, &async);
            release_com(attributes);
        }
        // The transport loop is deliberately low-latency and synchronous. Async
        // hardware MFTs are skipped until their event-driven queue is supported.
        if (async) {
            release_com(candidate);
            continue;
        }
        WCHAR* friendly_name = nullptr;
        UINT32 friendly_length = 0;
        if (SUCCEEDED(activations[index]->GetAllocatedString(
                MFT_FRIENDLY_NAME_Attribute, &friendly_name, &friendly_length))) {
            char utf8[256]{};
            WideCharToMultiByte(CP_UTF8, 0, friendly_name, -1, utf8,
                                static_cast<int>(sizeof(utf8)), nullptr, nullptr);
            name = utf8;
            CoTaskMemFree(friendly_name);
        }
        *transform = candidate;
        activation_hr = S_OK;
    }
    for (UINT32 index = 0; index < count; ++index) activations[index]->Release();
    CoTaskMemFree(activations);
    return *transform ? S_OK : activation_hr;
}

void set_codec_u32(ICodecAPI* codec, const GUID& key, ULONG value) {
    VARIANT setting;
    VariantInit(&setting);
    setting.vt = VT_UI4;
    setting.ulVal = value;
    codec->SetValue(&key, &setting);
    VariantClear(&setting);
}

void set_codec_bool(ICodecAPI* codec, const GUID& key, bool value) {
    VARIANT setting;
    VariantInit(&setting);
    setting.vt = VT_BOOL;
    setting.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    codec->SetValue(&key, &setting);
    VariantClear(&setting);
}

} // namespace

struct MfH264Encoder::Impl {
    IMFTransform* transform = nullptr;
    ICodecAPI* codec = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps = 0;
    LONGLONG frame_index = 0;
    LONGLONG frame_duration = 0;
    std::vector<uint8_t> nv12;
    std::vector<uint8_t> sequence_header;
    std::deque<std::vector<uint8_t>> pending;
    std::string backend;

    ~Impl() {
        if (transform) {
            transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        }
        release_com(codec);
        release_com(transform);
    }

    bool drain(std::vector<uint8_t>& encoded) {
        MFT_OUTPUT_STREAM_INFO info{};
        HRESULT hr = transform->GetOutputStreamInfo(0, &info);
        if (FAILED(hr)) return false;

        IMFSample* sample = nullptr;
        IMFMediaBuffer* buffer = nullptr;
        if (!(info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
            const DWORD capacity = std::max<DWORD>(info.cbSize, width * height * 2);
            hr = MFCreateMemoryBuffer(capacity, &buffer);
            if (SUCCEEDED(hr)) hr = MFCreateSample(&sample);
            if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer);
        }
        MFT_OUTPUT_DATA_BUFFER output{};
        output.dwStreamID = 0;
        output.pSample = sample;
        DWORD status = 0;
        if (SUCCEEDED(hr)) hr = transform->ProcessOutput(0, 1, &output, &status);
        if (SUCCEEDED(hr)) {
            IMFSample* actual_sample = output.pSample ? output.pSample : sample;
            if (actual_sample) copy_sample_bytes(actual_sample, encoded);
        }
        if (output.pEvents) output.pEvents->Release();
        if (output.pSample && output.pSample != sample) output.pSample->Release();
        release_com(sample);
        release_com(buffer);
        if (FAILED(hr) && hr != MF_E_TRANSFORM_NEED_MORE_INPUT) {
            static HRESULT last_error = S_OK;
            if (hr != last_error) {
                std::fprintf(stderr, "H.264 encoder ProcessOutput failed (0x%08lX).\n",
                             static_cast<unsigned long>(hr));
                last_error = hr;
            }
        }
        return SUCCEEDED(hr) && !encoded.empty();
    }

    void refresh_sequence_header() {
        IMFMediaType* type = nullptr;
        UINT32 size = 0;
        HRESULT hr = transform->GetOutputCurrentType(0, &type);
        if (SUCCEEDED(hr)) hr = type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size);
        if (SUCCEEDED(hr) && size > 0) {
            sequence_header.resize(size);
            hr = type->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER,
                               sequence_header.data(), size, &size);
            if (FAILED(hr)) sequence_header.clear();
        }
        release_com(type);
    }
};

MfH264Encoder::MfH264Encoder() : impl_(std::make_unique<Impl>()) {}
MfH264Encoder::~MfH264Encoder() = default;

bool MfH264Encoder::initialize(const H264EncoderConfig& config) {
    if (!impl_ || config.width == 0 || config.height == 0 || config.fps == 0 ||
        (config.width & 1) || (config.height & 1)) return false;
    impl_->width = config.width;
    impl_->height = config.height;
    impl_->fps = config.fps;
    impl_->frame_duration = 10000000LL / config.fps;

    MFT_REGISTER_TYPE_INFO input_info{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO output_info{MFMediaType_Video, MFVideoFormat_H264};
    bool hardware = false;
    HRESULT hr = MF_E_TOPO_CODEC_NOT_FOUND;
    if (config.prefer_hardware) {
        hr = activate_transform(MFT_CATEGORY_VIDEO_ENCODER, input_info, output_info,
                                true, &impl_->transform, impl_->backend);
        hardware = SUCCEEDED(hr);
    }
    if (!impl_->transform) {
        hr = activate_transform(MFT_CATEGORY_VIDEO_ENCODER, input_info, output_info,
                                false, &impl_->transform, impl_->backend);
        hardware = false;
    }
    if (FAILED(hr) || !impl_->transform) {
        std::fprintf(stderr, "No synchronous Media Foundation H.264 encoder is available.\n");
        return false;
    }

    if (SUCCEEDED(impl_->transform->QueryInterface(IID_PPV_ARGS(&impl_->codec)))) {
        set_codec_bool(impl_->codec, CODECAPI_AVLowLatencyMode, true);
        set_codec_u32(impl_->codec, CODECAPI_AVEncCommonAllowFrameDrops, 0);
        set_codec_u32(impl_->codec, CODECAPI_AVEncCommonRateControlMode,
                      eAVEncCommonRateControlMode_CBR);
        set_codec_u32(impl_->codec, CODECAPI_AVEncCommonMeanBitRate, config.bitrate);
        set_codec_u32(impl_->codec, CODECAPI_AVEncMPVGOPSize, std::max(1u, config.fps * 2));
        set_codec_u32(impl_->codec, CODECAPI_AVEncMPVDefaultBPictureCount, 0);
    }

    IMFMediaType* output_type = nullptr;
    IMFMediaType* input_type = nullptr;
    hr = MFCreateMediaType(&output_type);
    if (SUCCEEDED(hr)) hr = output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(output_type, MF_MT_FRAME_SIZE, config.width, config.height);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(output_type, MF_MT_FRAME_RATE, config.fps, 1);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(output_type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(hr)) hr = output_type->SetUINT32(MF_MT_AVG_BITRATE, config.bitrate);
    if (SUCCEEDED(hr)) hr = output_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = output_type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);
    if (SUCCEEDED(hr)) hr = impl_->transform->SetOutputType(0, output_type, 0);

    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&input_type);
    if (SUCCEEDED(hr)) hr = input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = input_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(input_type, MF_MT_FRAME_SIZE, config.width, config.height);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(input_type, MF_MT_FRAME_RATE, config.fps, 1);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(input_type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(hr)) hr = input_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = impl_->transform->SetInputType(0, input_type, 0);
    release_com(input_type);
    release_com(output_type);
    if (FAILED(hr)) {
        std::fprintf(stderr, "Could not configure the Media Foundation H.264 encoder (0x%08lX).\n",
                     static_cast<unsigned long>(hr));
        return false;
    }

    if (impl_->codec) {
        set_codec_bool(impl_->codec, CODECAPI_AVLowLatencyMode, true);
        set_codec_u32(impl_->codec, CODECAPI_AVEncCommonAllowFrameDrops, 0);
        set_codec_u32(impl_->codec, CODECAPI_AVEncCommonRateControlMode,
                      eAVEncCommonRateControlMode_CBR);
        set_codec_u32(impl_->codec, CODECAPI_AVEncCommonMeanBitRate, config.bitrate);
        set_codec_u32(impl_->codec, CODECAPI_AVEncMPVGOPSize, std::max(1u, config.fps * 2));
        set_codec_u32(impl_->codec, CODECAPI_AVEncMPVDefaultBPictureCount, 0);
    }
    impl_->transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    impl_->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    impl_->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    impl_->refresh_sequence_header();
    impl_->backend = std::string("mf-h264-") + (hardware ? "hardware: " : "software: ") +
                     (impl_->backend.empty() ? "unknown MFT" : impl_->backend);
    return true;
}

bool MfH264Encoder::encode_bgr24(const uint8_t* pixels, uint32_t stride,
                                 std::vector<uint8_t>& encoded_frame) {
    encoded_frame.clear();
    if (!impl_->transform || !pixels) return false;
    bgr24_to_nv12(pixels, stride, impl_->width, impl_->height, impl_->nv12);
    IMFSample* sample = nullptr;
    HRESULT hr = create_sample(impl_->nv12.data(), static_cast<DWORD>(impl_->nv12.size()),
                               impl_->frame_index * impl_->frame_duration,
                               impl_->frame_duration, &sample);
    if (SUCCEEDED(hr)) hr = impl_->transform->ProcessInput(0, sample, 0);
    for (int attempt = 0; hr == MF_E_NOTACCEPTING && attempt < 16; ++attempt) {
        std::vector<uint8_t> drained;
        if (!impl_->drain(drained)) break;
        impl_->pending.push_back(std::move(drained));
        hr = impl_->transform->ProcessInput(0, sample, 0);
    }
    release_com(sample);
    ++impl_->frame_index;
    if (FAILED(hr)) {
        static HRESULT last_error = S_OK;
        if (hr != last_error) {
            std::fprintf(stderr, "H.264 encoder ProcessInput failed (0x%08lX).\n",
                         static_cast<unsigned long>(hr));
            last_error = hr;
        }
    }
    if (SUCCEEDED(hr)) {
        for (int output_index = 0; output_index < 16; ++output_index) {
            std::vector<uint8_t> drained;
            if (!impl_->drain(drained)) break;
            impl_->pending.push_back(std::move(drained));
        }
    }
    if (!impl_->pending.empty()) {
        encoded_frame = std::move(impl_->pending.front());
        impl_->pending.pop_front();
    }
    if (!encoded_frame.empty() && impl_->sequence_header.empty())
        impl_->refresh_sequence_header();
    return !encoded_frame.empty();
}

void MfH264Encoder::request_keyframe() {
    if (impl_->codec) set_codec_bool(impl_->codec, CODECAPI_AVEncVideoForceKeyFrame, true);
}

const std::string& MfH264Encoder::backend_name() const { return impl_->backend; }
const std::vector<uint8_t>& MfH264Encoder::sequence_header() const {
    return impl_->sequence_header;
}

struct MfH264Decoder::Impl {
    IMFTransform* transform = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps = 0;
    uint32_t stride = 0;
    LONGLONG frame_index = 0;
    LONGLONG frame_duration = 0;
    std::deque<std::vector<uint8_t>> pending;

    ~Impl() {
        if (transform) {
            transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        }
        release_com(transform);
    }

    HRESULT set_output_type() {
        IMFMediaType* output = nullptr;
        HRESULT hr = MFCreateMediaType(&output);
        if (SUCCEEDED(hr)) hr = output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (SUCCEEDED(hr)) hr = output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        if (SUCCEEDED(hr)) hr = MFSetAttributeSize(output, MF_MT_FRAME_SIZE, width, height);
        if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(output, MF_MT_FRAME_RATE, fps, 1);
        if (SUCCEEDED(hr)) hr = output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (SUCCEEDED(hr)) hr = output->SetUINT32(MF_MT_DEFAULT_STRIDE, width);
        if (SUCCEEDED(hr)) hr = transform->SetOutputType(0, output, 0);
        release_com(output);
        stride = width;
        return hr;
    }

    bool drain(std::vector<uint8_t>& bgra) {
        MFT_OUTPUT_STREAM_INFO info{};
        HRESULT hr = transform->GetOutputStreamInfo(0, &info);
        if (FAILED(hr)) return false;
        IMFSample* sample = nullptr;
        IMFMediaBuffer* buffer = nullptr;
        const DWORD expected = stride * height * 3 / 2;
        if (!(info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
            hr = MFCreateMemoryBuffer(std::max(info.cbSize, expected), &buffer);
            if (SUCCEEDED(hr)) hr = MFCreateSample(&sample);
            if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer);
        }
        MFT_OUTPUT_DATA_BUFFER output{};
        output.dwStreamID = 0;
        output.pSample = sample;
        DWORD status = 0;
        if (SUCCEEDED(hr)) hr = transform->ProcessOutput(0, 1, &output, &status);
        bool retry_after_stream_change = false;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            retry_after_stream_change = SUCCEEDED(set_output_type());
        } else if (SUCCEEDED(hr)) {
            IMFSample* actual_sample = output.pSample ? output.pSample : sample;
            IMFMediaBuffer* contiguous = nullptr;
            BYTE* bytes = nullptr;
            DWORD length = 0;
            if (actual_sample) hr = actual_sample->ConvertToContiguousBuffer(&contiguous);
            if (SUCCEEDED(hr)) hr = contiguous->Lock(&bytes, nullptr, &length);
            if (SUCCEEDED(hr) && length >= expected) {
                const uint32_t storage_height = std::max(
                    height, static_cast<uint32_t>((static_cast<uint64_t>(length) * 2) /
                                                  (static_cast<uint64_t>(stride) * 3)));
                nv12_to_bgra(bytes, stride, storage_height, width, height, bgra);
            }
            if (bytes) contiguous->Unlock();
            release_com(contiguous);
        }
        if (output.pEvents) output.pEvents->Release();
        if (output.pSample && output.pSample != sample) output.pSample->Release();
        release_com(sample);
        release_com(buffer);
        if (FAILED(hr) && hr != MF_E_TRANSFORM_NEED_MORE_INPUT &&
            hr != MF_E_TRANSFORM_STREAM_CHANGE) {
            static HRESULT last_error = S_OK;
            if (hr != last_error) {
                std::fprintf(stderr, "H.264 decoder ProcessOutput failed (0x%08lX).\n",
                             static_cast<unsigned long>(hr));
                last_error = hr;
            }
        }
        if (retry_after_stream_change) return drain(bgra);
        return SUCCEEDED(hr) && !bgra.empty();
    }
};

MfH264Decoder::MfH264Decoder() : impl_(std::make_unique<Impl>()) {}
MfH264Decoder::~MfH264Decoder() = default;

bool MfH264Decoder::initialize(uint32_t width, uint32_t height, uint32_t fps,
                               const uint8_t* sequence_header,
                               uint32_t sequence_header_size) {
    if (!impl_ || !width || !height || !fps) return false;
    impl_->width = width;
    impl_->height = height;
    impl_->fps = fps;
    impl_->frame_duration = 10000000LL / fps;

    MFT_REGISTER_TYPE_INFO input_info{MFMediaType_Video, MFVideoFormat_H264};
    MFT_REGISTER_TYPE_INFO output_info{MFMediaType_Video, MFVideoFormat_NV12};
    std::string name;
    HRESULT hr = activate_transform(MFT_CATEGORY_VIDEO_DECODER, input_info, output_info,
                                    false, &impl_->transform, name);
    if (FAILED(hr) || !impl_->transform) {
        std::fprintf(stderr, "No Media Foundation H.264 decoder is available.\n");
        return false;
    }

    IMFAttributes* decoder_attributes = nullptr;
    if (SUCCEEDED(impl_->transform->GetAttributes(&decoder_attributes))) {
        decoder_attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
        release_com(decoder_attributes);
    }
    ICodecAPI* decoder_codec = nullptr;
    if (SUCCEEDED(impl_->transform->QueryInterface(IID_PPV_ARGS(&decoder_codec)))) {
        set_codec_bool(decoder_codec, CODECAPI_AVLowLatencyMode, true);
        release_com(decoder_codec);
    }

    IMFMediaType* input = nullptr;
    hr = MFCreateMediaType(&input);
    if (SUCCEEDED(hr)) hr = input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(input, MF_MT_FRAME_SIZE, width, height);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(input, MF_MT_FRAME_RATE, fps, 1);
    if (SUCCEEDED(hr) && sequence_header && sequence_header_size > 0)
        hr = input->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER,
                            sequence_header, sequence_header_size);
    if (SUCCEEDED(hr)) hr = impl_->transform->SetInputType(0, input, 0);
    release_com(input);
    if (SUCCEEDED(hr)) hr = impl_->set_output_type();
    if (FAILED(hr)) {
        std::fprintf(stderr, "Could not configure the Media Foundation H.264 decoder (0x%08lX).\n",
                     static_cast<unsigned long>(hr));
        return false;
    }
    impl_->transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    impl_->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    impl_->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

bool MfH264Decoder::decode(const uint8_t* encoded_frame, uint32_t encoded_size,
                           std::vector<uint8_t>& bgra_frame) {
    bgra_frame.clear();
    if (!impl_->transform || !encoded_frame || !encoded_size) return false;
    IMFSample* sample = nullptr;
    HRESULT hr = create_sample(encoded_frame, encoded_size,
                               impl_->frame_index * impl_->frame_duration,
                               impl_->frame_duration, &sample);
    if (SUCCEEDED(hr)) {
        const bool keyframe = contains_h264_nal_type(encoded_frame, encoded_size, 5) ||
                              contains_h264_nal_type(encoded_frame, encoded_size, 7);
        sample->SetUINT32(MFSampleExtension_CleanPoint, keyframe ? TRUE : FALSE);
        if (impl_->frame_index == 0)
            sample->SetUINT32(MFSampleExtension_Discontinuity, TRUE);
        sample->SetUINT64(MFSampleExtension_DecodeTimestamp,
                          impl_->frame_index * impl_->frame_duration);
    }
    if (SUCCEEDED(hr)) hr = impl_->transform->ProcessInput(0, sample, 0);
    for (int attempt = 0; hr == MF_E_NOTACCEPTING && attempt < 16; ++attempt) {
        std::vector<uint8_t> drained;
        if (!impl_->drain(drained)) break;
        impl_->pending.push_back(std::move(drained));
        hr = impl_->transform->ProcessInput(0, sample, 0);
    }
    release_com(sample);
    ++impl_->frame_index;
    if (FAILED(hr)) {
        static HRESULT last_error = S_OK;
        if (hr != last_error) {
            std::fprintf(stderr, "H.264 decoder ProcessInput failed (0x%08lX).\n",
                         static_cast<unsigned long>(hr));
            last_error = hr;
        }
    }
    if (SUCCEEDED(hr)) {
        for (int output_index = 0; output_index < 16; ++output_index) {
            std::vector<uint8_t> drained;
            if (!impl_->drain(drained)) break;
            impl_->pending.push_back(std::move(drained));
        }
    }
    if (!impl_->pending.empty()) {
        bgra_frame = std::move(impl_->pending.front());
        impl_->pending.pop_front();
    }
    return !bgra_frame.empty();
}
