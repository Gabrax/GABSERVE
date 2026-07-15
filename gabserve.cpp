#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmsystem.h>
#include <wincodec.h>
#include <objidl.h>
#include <propidl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr uint32_t kMagic = 0x53424147; // "GABS" in little-endian.
constexpr uint8_t kProtocolVersion = 1;
constexpr size_t kPayloadBytes = 1200;
constexpr uint32_t kMaxObjectBytes = 16 * 1024 * 1024;

enum class PacketKind : uint8_t {
    VideoJpeg = 1,
    AudioPcm = 2,
    AudioFormat = 3,
};

#pragma pack(push, 1)
struct PacketHeader {
    uint32_t magic;
    uint8_t version;
    uint8_t kind;
    uint16_t header_size;
    uint32_t object_id;
    uint32_t chunk_index;
    uint32_t chunk_count;
    uint32_t payload_size;
    uint32_t total_size;
    uint64_t timestamp_us;
};
#pragma pack(pop)

static_assert(sizeof(PacketHeader) == 36, "Unexpected protocol header size");

std::atomic_bool g_running{true};

template <class T>
void release_com(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

uint64_t now_us() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

const char* hr_text(HRESULT hr) {
    static thread_local char text[32];
    std::snprintf(text, sizeof(text), "0x%08lX", static_cast<unsigned long>(hr));
    return text;
}

class UdpSender {
public:
    ~UdpSender() {
        if (socket_ != INVALID_SOCKET) closesocket(socket_);
    }

    bool open(const char* host, uint16_t port) {
        socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == INVALID_SOCKET) return false;

        int buffer_size = 4 * 1024 * 1024;
        setsockopt(socket_, SOL_SOCKET, SO_SNDBUF,
                   reinterpret_cast<const char*>(&buffer_size), sizeof(buffer_size));

        std::memset(&target_, 0, sizeof(target_));
        target_.sin_family = AF_INET;
        target_.sin_port = htons(port);
        if (InetPtonA(AF_INET, host, &target_.sin_addr) != 1) {
            std::fprintf(stderr, "Invalid IPv4 address: %s\n", host);
            return false;
        }
        return true;
    }

    bool send_object(PacketKind kind, uint32_t object_id, const uint8_t* data,
                     uint32_t size, uint64_t timestamp) {
        if (size == 0 || size > kMaxObjectBytes) return false;
        const uint32_t chunks = static_cast<uint32_t>((size + kPayloadBytes - 1) / kPayloadBytes);
        std::vector<uint8_t> packet(sizeof(PacketHeader) + kPayloadBytes);

        for (uint32_t index = 0; index < chunks && g_running.load(); ++index) {
            const uint32_t offset = static_cast<uint32_t>(index * kPayloadBytes);
            const uint32_t payload = std::min<uint32_t>(
                static_cast<uint32_t>(kPayloadBytes), size - offset);
            PacketHeader header{};
            header.magic = kMagic;
            header.version = kProtocolVersion;
            header.kind = static_cast<uint8_t>(kind);
            header.header_size = sizeof(PacketHeader);
            header.object_id = object_id;
            header.chunk_index = index;
            header.chunk_count = chunks;
            header.payload_size = payload;
            header.total_size = size;
            header.timestamp_us = timestamp;
            std::memcpy(packet.data(), &header, sizeof(header));
            std::memcpy(packet.data() + sizeof(header), data + offset, payload);

            const int sent = sendto(socket_, reinterpret_cast<const char*>(packet.data()),
                                    static_cast<int>(sizeof(header) + payload), 0,
                                    reinterpret_cast<const sockaddr*>(&target_), sizeof(target_));
            if (sent == SOCKET_ERROR) return false;
        }
        return true;
    }

private:
    SOCKET socket_ = INVALID_SOCKET;
    sockaddr_in target_{};
};

class ScreenCapture {
public:
    ~ScreenCapture() {
        if (old_bitmap_) SelectObject(memory_dc_, old_bitmap_);
        if (bitmap_) DeleteObject(bitmap_);
        if (memory_dc_) DeleteDC(memory_dc_);
        if (screen_dc_) ReleaseDC(nullptr, screen_dc_);
    }

    bool initialize(int max_width) {
        source_width_ = GetSystemMetrics(SM_CXSCREEN);
        source_height_ = GetSystemMetrics(SM_CYSCREEN);
        width_ = std::min(source_width_, max_width);
        height_ = std::max(1, static_cast<int>(
            static_cast<int64_t>(source_height_) * width_ / source_width_));

        screen_dc_ = GetDC(nullptr);
        memory_dc_ = CreateCompatibleDC(screen_dc_);
        if (!screen_dc_ || !memory_dc_) return false;

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width_;
        info.bmiHeader.biHeight = -height_; // top-down
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 24;
        info.bmiHeader.biCompression = BI_RGB;
        bitmap_ = CreateDIBSection(memory_dc_, &info, DIB_RGB_COLORS,
                                   reinterpret_cast<void**>(&pixels_), nullptr, 0);
        if (!bitmap_ || !pixels_) return false;
        old_bitmap_ = SelectObject(memory_dc_, bitmap_);
        stride_ = ((width_ * 3 + 3) / 4) * 4;
        SetStretchBltMode(memory_dc_, HALFTONE);
        SetBrushOrgEx(memory_dc_, 0, 0, nullptr);
        return true;
    }

    bool capture() {
        if (width_ == source_width_ && height_ == source_height_) {
            return BitBlt(memory_dc_, 0, 0, width_, height_, screen_dc_, 0, 0,
                          SRCCOPY | CAPTUREBLT) != FALSE;
        }
        return StretchBlt(memory_dc_, 0, 0, width_, height_, screen_dc_, 0, 0,
                          source_width_, source_height_, SRCCOPY) != FALSE;
    }

    void generate_test_pattern(uint32_t frame) {
        for (int y = 0; y < height_; ++y) {
            uint8_t* row = pixels_ + static_cast<size_t>(y) * stride_;
            for (int x = 0; x < width_; ++x) {
                row[x * 3 + 0] = static_cast<uint8_t>((x + frame * 5) & 0xFF);
                row[x * 3 + 1] = static_cast<uint8_t>((y + frame * 3) & 0xFF);
                row[x * 3 + 2] = static_cast<uint8_t>((x + y + frame * 7) & 0xFF);
            }
        }
    }

    int width() const { return width_; }
    int height() const { return height_; }
    int stride() const { return stride_; }
    const uint8_t* pixels() const { return pixels_; }

private:
    HDC screen_dc_ = nullptr;
    HDC memory_dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ old_bitmap_ = nullptr;
    uint8_t* pixels_ = nullptr;
    int source_width_ = 0;
    int source_height_ = 0;
    int width_ = 0;
    int height_ = 0;
    int stride_ = 0;
};

class JpegCodec {
public:
    ~JpegCodec() { release_com(factory_); }

    bool initialize() {
        const HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory_));
        if (FAILED(hr)) std::fprintf(stderr, "WIC init: %s\n", hr_text(hr));
        return SUCCEEDED(hr);
    }

    bool encode(const ScreenCapture& capture, float quality, std::vector<uint8_t>& output) {
        IStream* stream = nullptr;
        IWICBitmapEncoder* encoder = nullptr;
        IWICBitmapFrameEncode* frame = nullptr;
        IPropertyBag2* properties = nullptr;
        HGLOBAL global = nullptr;
        bool ok = false;

        HRESULT hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
        if (SUCCEEDED(hr)) hr = factory_->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder);
        if (SUCCEEDED(hr)) hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
        if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, &properties);
        if (SUCCEEDED(hr) && properties) {
            PROPBAG2 option{};
            option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
            VARIANT value;
            VariantInit(&value);
            value.vt = VT_R4;
            value.fltVal = std::clamp(quality, 0.1f, 1.0f);
            properties->Write(1, &option, &value);
            VariantClear(&value);
        }
        if (SUCCEEDED(hr)) hr = frame->Initialize(properties);
        if (SUCCEEDED(hr)) hr = frame->SetSize(capture.width(), capture.height());
        WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
        if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
        if (SUCCEEDED(hr) && format != GUID_WICPixelFormat24bppBGR) hr = E_FAIL;
        if (SUCCEEDED(hr)) hr = frame->WritePixels(capture.height(), capture.stride(),
            capture.stride() * capture.height(), const_cast<BYTE*>(capture.pixels()));
        if (SUCCEEDED(hr)) hr = frame->Commit();
        if (SUCCEEDED(hr)) hr = encoder->Commit();
        if (SUCCEEDED(hr)) hr = GetHGlobalFromStream(stream, &global);
        if (SUCCEEDED(hr)) {
            STATSTG stats{};
            hr = stream->Stat(&stats, STATFLAG_NONAME);
            const SIZE_T size = SUCCEEDED(hr) ? static_cast<SIZE_T>(stats.cbSize.QuadPart) : 0;
            const void* bytes = GlobalLock(global);
            if (bytes && size > 0 && size <= kMaxObjectBytes) {
                output.assign(static_cast<const uint8_t*>(bytes),
                              static_cast<const uint8_t*>(bytes) + size);
                ok = true;
            }
            if (bytes) GlobalUnlock(global);
        }

        release_com(properties);
        release_com(frame);
        release_com(encoder);
        release_com(stream);
        return ok;
    }

    bool decode(const uint8_t* jpeg, size_t jpeg_size, std::vector<uint8_t>& pixels,
                UINT& width, UINT& height) {
        HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE, jpeg_size);
        if (!global) return false;
        void* target = GlobalLock(global);
        std::memcpy(target, jpeg, jpeg_size);
        GlobalUnlock(global);

        IStream* stream = nullptr;
        IWICBitmapDecoder* decoder = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICFormatConverter* converter = nullptr;
        bool ok = false;
        HRESULT hr = CreateStreamOnHGlobal(global, TRUE, &stream);
        if (FAILED(hr)) GlobalFree(global);
        if (SUCCEEDED(hr)) hr = factory_->CreateDecoderFromStream(
            stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
        if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
        if (SUCCEEDED(hr)) hr = frame->GetSize(&width, &height);
        if (SUCCEEDED(hr)) hr = factory_->CreateFormatConverter(&converter);
        if (SUCCEEDED(hr)) hr = converter->Initialize(frame, GUID_WICPixelFormat32bppBGR,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
        if (SUCCEEDED(hr) && width > 0 && height > 0 && width <= 8192 && height <= 8192) {
            const UINT stride = width * 4;
            pixels.resize(static_cast<size_t>(stride) * height);
            hr = converter->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()), pixels.data());
            ok = SUCCEEDED(hr);
        }
        release_com(converter);
        release_com(frame);
        release_com(decoder);
        release_com(stream);
        return ok;
    }

private:
    IWICImagingFactory* factory_ = nullptr;
};

void audio_capture_loop(UdpSender* sender) {
    const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioCaptureClient* capture = nullptr;
    WAVEFORMATEX* format = nullptr;
    uint32_t audio_id = 1;

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&enumerator));
    if (SUCCEEDED(hr)) hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (SUCCEEDED(hr)) hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                              reinterpret_cast<void**>(&client));
    if (SUCCEEDED(hr)) hr = client->GetMixFormat(&format);
    if (SUCCEEDED(hr)) hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK, 0, 0, format, nullptr);
    if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&capture));

    const uint32_t format_size = format
        ? std::min<uint32_t>(sizeof(WAVEFORMATEX) + format->cbSize, sizeof(WAVEFORMATEXTENSIBLE))
        : 0;
    if (SUCCEEDED(hr)) {
        sender->send_object(PacketKind::AudioFormat, 1,
            reinterpret_cast<const uint8_t*>(format), format_size, now_us());
        hr = client->Start();
    }
    if (FAILED(hr)) {
        std::fprintf(stderr, "Audio loopback is unavailable (%s); video will still be transmitted.\n",
                     hr_text(hr));
    } else {
        std::printf("Audio: %lu Hz, %u channel(s), %u-bit\n",
                    format->nSamplesPerSec, format->nChannels, format->wBitsPerSample);
    }

    std::vector<uint8_t> silence;
    auto last_format = std::chrono::steady_clock::now();
    while (SUCCEEDED(hr) && g_running.load()) {
        UINT32 packet_frames = 0;
        hr = capture->GetNextPacketSize(&packet_frames);
        while (SUCCEEDED(hr) && packet_frames > 0 && g_running.load()) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;
            const uint32_t bytes = frames * format->nBlockAlign;
            const uint8_t* send_data = data;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                silence.assign(bytes, 0);
                send_data = silence.data();
            }
            sender->send_object(PacketKind::AudioPcm, audio_id++, send_data, bytes, now_us());
            capture->ReleaseBuffer(frames);
            hr = capture->GetNextPacketSize(&packet_frames);
        }
        const auto current = std::chrono::steady_clock::now();
        if (current - last_format >= std::chrono::seconds(1)) {
            sender->send_object(PacketKind::AudioFormat, 1,
                reinterpret_cast<const uint8_t*>(format), format_size, now_us());
            last_format = current;
        }
        Sleep(3);
    }

    if (client) client->Stop();
    if (format) CoTaskMemFree(format);
    release_com(capture);
    release_com(client);
    release_com(device);
    release_com(enumerator);
    if (SUCCEEDED(com_hr)) CoUninitialize();
}

struct Assembly {
    uint32_t object_id = 0;
    uint32_t chunk_count = 0;
    uint32_t total_size = 0;
    uint32_t received_count = 0;
    uint64_t timestamp_us = 0;
    std::vector<uint8_t> data;
    std::vector<uint8_t> received;

    void reset(const PacketHeader& header) {
        object_id = header.object_id;
        chunk_count = header.chunk_count;
        total_size = header.total_size;
        timestamp_us = header.timestamp_us;
        received_count = 0;
        data.assign(total_size, 0);
        received.assign(chunk_count, 0);
    }

    bool add(const PacketHeader& header, const uint8_t* payload) {
        if (header.object_id != object_id || header.chunk_count != chunk_count ||
            header.total_size != total_size) {
            reset(header);
        }
        if (header.chunk_index >= chunk_count || received[header.chunk_index]) return false;
        const size_t offset = static_cast<size_t>(header.chunk_index) * kPayloadBytes;
        if (offset + header.payload_size > data.size()) return false;
        std::memcpy(data.data() + offset, payload, header.payload_size);
        received[header.chunk_index] = 1;
        ++received_count;
        return received_count == chunk_count;
    }
};

class AudioPlayer {
    struct AudioBuffer {
        WAVEHDR header{};
        std::vector<uint8_t> data;
    };

public:
    ~AudioPlayer() { close(); }

    void set_format(const std::vector<uint8_t>& bytes) {
        if (bytes.size() < sizeof(WAVEFORMATEX)) return;
        if (bytes == format_) return;
        close();
        format_ = bytes;
        const MMRESULT result = waveOutOpen(&output_, WAVE_MAPPER,
            reinterpret_cast<WAVEFORMATEX*>(format_.data()), 0, 0, CALLBACK_NULL);
        if (result != MMSYSERR_NOERROR) {
            output_ = nullptr;
            std::fprintf(stderr, "Could not open the audio output (waveOut=%u).\n", result);
        } else {
            std::printf("Audio receiver started.\n");
        }
    }

    void enqueue(const std::vector<uint8_t>& bytes) {
        cleanup();
        if (!output_ || bytes.empty() || pending_.size() >= 64) return;
        auto buffer = std::make_unique<AudioBuffer>();
        buffer->data = bytes;
        buffer->header.lpData = reinterpret_cast<LPSTR>(buffer->data.data());
        buffer->header.dwBufferLength = static_cast<DWORD>(buffer->data.size());
        if (waveOutPrepareHeader(output_, &buffer->header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR)
            return;
        if (waveOutWrite(output_, &buffer->header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            waveOutUnprepareHeader(output_, &buffer->header, sizeof(WAVEHDR));
            return;
        }
        pending_.push_back(std::move(buffer));
    }

    void cleanup() {
        if (!output_) return;
        auto it = pending_.begin();
        while (it != pending_.end()) {
            if ((*it)->header.dwFlags & WHDR_DONE) {
                waveOutUnprepareHeader(output_, &(*it)->header, sizeof(WAVEHDR));
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
    }

private:
    void close() {
        if (!output_) return;
        waveOutReset(output_);
        for (auto& buffer : pending_)
            waveOutUnprepareHeader(output_, &buffer->header, sizeof(WAVEHDR));
        pending_.clear();
        waveOutClose(output_);
        output_ = nullptr;
    }

    HWAVEOUT output_ = nullptr;
    std::vector<uint8_t> format_;
    std::vector<std::unique_ptr<AudioBuffer>> pending_;
};

struct VideoFrame {
    std::vector<uint8_t> pixels;
    UINT width = 0;
    UINT height = 0;
};

VideoFrame g_video_frame;

LRESULT CALLBACK receiver_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        FillRect(dc, &client, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        if (!g_video_frame.pixels.empty()) {
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = static_cast<LONG>(g_video_frame.width);
            info.bmiHeader.biHeight = -static_cast<LONG>(g_video_frame.height);
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            StretchDIBits(dc, 0, 0, client.right, client.bottom,
                0, 0, g_video_frame.width, g_video_frame.height,
                g_video_frame.pixels.data(), &info, DIB_RGB_COLORS, SRCCOPY);
        }
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_DESTROY) {
        g_running.store(false);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

bool validate_packet(const uint8_t* packet, int packet_size, PacketHeader& header) {
    if (packet_size < static_cast<int>(sizeof(PacketHeader))) return false;
    std::memcpy(&header, packet, sizeof(header));
    if (header.magic != kMagic || header.version != kProtocolVersion ||
        header.header_size != sizeof(PacketHeader) || header.chunk_count == 0 ||
        header.total_size == 0 || header.total_size > kMaxObjectBytes ||
        header.payload_size > kPayloadBytes || header.chunk_index >= header.chunk_count ||
        sizeof(PacketHeader) + header.payload_size != static_cast<size_t>(packet_size)) return false;
    const uint32_t expected_chunks = static_cast<uint32_t>(
        (header.total_size + kPayloadBytes - 1) / kPayloadBytes);
    if (header.chunk_count != expected_chunks) return false;
    const size_t offset = static_cast<size_t>(header.chunk_index) * kPayloadBytes;
    return offset + header.payload_size <= header.total_size;
}

BOOL WINAPI console_handler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT) {
        g_running.store(false);
        return TRUE;
    }
    return FALSE;
}

struct Options {
    std::string mode;
    std::string address = "127.0.0.1";
    uint16_t port = 7777;
    int fps = 10;
    int max_width = 1280;
    float quality = 0.65f;
    bool audio = true;
    bool headless = false;
    bool test_pattern = false;
    int seconds = 0;
};

void print_usage() {
    std::printf(
        "GABSERVE - screen and audio over UDP\n\n"
        "  gabserve receive [--bind 127.0.0.1] [--port 7777] [--no-audio]\n"
        "  gabserve send    [--host 127.0.0.1] [--port 7777]\n"
        "                    [--fps 10] [--width 1280] [--quality 0.65] [--no-audio]\n"
        "  test: add --headless --seconds 5 to the receiver\n\n"
        "Start receive first, then send. Ctrl+C stops transmission.\n");
}

bool parse_options(int argc, char** argv, Options& options) {
    if (argc < 2) return false;
    options.mode = argv[1];
    for (int index = 2; index < argc; ++index) {
        const std::string argument = argv[index];
        auto next = [&]() -> const char* {
            return index + 1 < argc ? argv[++index] : nullptr;
        };
        const char* value = nullptr;
        if (argument == "--host" || argument == "--bind") {
            value = next(); if (!value) return false; options.address = value;
        } else if (argument == "--port") {
            value = next(); if (!value) return false; options.port = static_cast<uint16_t>(std::atoi(value));
        } else if (argument == "--fps") {
            value = next(); if (!value) return false; options.fps = std::atoi(value);
        } else if (argument == "--width") {
            value = next(); if (!value) return false; options.max_width = std::atoi(value);
        } else if (argument == "--quality") {
            value = next(); if (!value) return false; options.quality = std::strtof(value, nullptr);
        } else if (argument == "--no-audio") {
            options.audio = false;
        } else if (argument == "--headless") {
            options.headless = true;
        } else if (argument == "--test-pattern") {
            options.test_pattern = true;
        } else if (argument == "--seconds") {
            value = next(); if (!value) return false; options.seconds = std::atoi(value);
        } else {
            return false;
        }
    }
    return (options.mode == "send" || options.mode == "receive") && options.port > 0 &&
        options.fps >= 1 && options.fps <= 60 && options.max_width >= 160 &&
        options.max_width <= 7680 && options.quality >= 0.1f && options.quality <= 1.0f &&
        options.seconds >= 0 && options.seconds <= 86400;
}

int run_sender(const Options& options) {
    UdpSender sender;
    if (!sender.open(options.address.c_str(), options.port)) {
        std::fprintf(stderr, "Could not open the UDP socket (%d).\n", WSAGetLastError());
        return 1;
    }
    ScreenCapture screen;
    if (!screen.initialize(options.max_width)) {
        std::fprintf(stderr, "Could not initialize screen capture.\n");
        return 1;
    }
    JpegCodec codec;
    if (!codec.initialize()) return 1;

    std::thread audio_thread;
    if (options.audio) audio_thread = std::thread(audio_capture_loop, &sender);
    std::printf("Transmitting to %s:%u | %dx%d | %d FPS | JPEG %.0f%%\n",
        options.address.c_str(), options.port, screen.width(), screen.height(),
        options.fps, options.quality * 100.0f);

    uint32_t frame_id = 1;
    uint64_t captured_frames = 0;
    uint64_t encoded_frames = 0;
    uint64_t sent_frames = 0;
    std::vector<uint8_t> jpeg;
    const auto frame_duration = std::chrono::microseconds(1000000 / options.fps);
    auto deadline = std::chrono::steady_clock::now();
    const auto stop_time = options.seconds > 0
        ? std::chrono::steady_clock::now() + std::chrono::seconds(options.seconds)
        : std::chrono::steady_clock::time_point::max();
    while (g_running.load()) {
        if (std::chrono::steady_clock::now() >= stop_time) break;
        deadline += frame_duration;
        const bool captured = options.test_pattern
            ? (screen.generate_test_pattern(frame_id), true)
            : screen.capture();
        if (captured) {
            ++captured_frames;
            if (codec.encode(screen, options.quality, jpeg)) {
                ++encoded_frames;
                if (sender.send_object(PacketKind::VideoJpeg, frame_id++, jpeg.data(),
                                       static_cast<uint32_t>(jpeg.size()), now_us())) {
                    ++sent_frames;
                } else {
                std::fprintf(stderr, "sendto failed: %d\n", WSAGetLastError());
                }
            }
        }
        std::this_thread::sleep_until(deadline);
        if (std::chrono::steady_clock::now() - deadline > std::chrono::seconds(1))
            deadline = std::chrono::steady_clock::now();
    }
    g_running.store(false);
    if (audio_thread.joinable()) audio_thread.join();
    if (options.seconds > 0)
        std::printf("Statistics: captured=%llu, JPEG=%llu, transmitted=%llu.\n",
                    static_cast<unsigned long long>(captured_frames),
                    static_cast<unsigned long long>(encoded_frames),
                    static_cast<unsigned long long>(sent_frames));
    return 0;
}

int run_receiver(const Options& options, HINSTANCE instance) {
    SOCKET socket_handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_handle == INVALID_SOCKET) return 1;
    int receive_buffer = 8 * 1024 * 1024;
    setsockopt(socket_handle, SOL_SOCKET, SO_RCVBUF,
               reinterpret_cast<const char*>(&receive_buffer), sizeof(receive_buffer));
    u_long nonblocking = 1;
    ioctlsocket(socket_handle, FIONBIO, &nonblocking);

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(options.port);
    if (InetPtonA(AF_INET, options.address.c_str(), &local.sin_addr) != 1 ||
        bind(socket_handle, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == SOCKET_ERROR) {
        std::fprintf(stderr, "Could not listen on %s:%u (WSA=%d).\n",
                     options.address.c_str(), options.port, WSAGetLastError());
        closesocket(socket_handle);
        return 1;
    }

    HWND window = nullptr;
    if (!options.headless) {
        WNDCLASSW window_class{};
        window_class.lpfnWndProc = receiver_window_proc;
        window_class.hInstance = instance;
        window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
        window_class.lpszClassName = L"GABSERVE_RECEIVER";
        RegisterClassW(&window_class);
        window = CreateWindowExW(0, window_class.lpszClassName, L"GABSERVE - UDP receiver",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 760,
            nullptr, nullptr, instance, nullptr);
        if (!window) {
            closesocket(socket_handle);
            return 1;
        }
    }

    JpegCodec codec;
    if (!codec.initialize()) {
        if (window) DestroyWindow(window);
        closesocket(socket_handle);
        return 1;
    }
    AudioPlayer audio;
    Assembly video_assembly;
    Assembly audio_assembly;
    Assembly format_assembly;
    std::vector<uint8_t> packet(sizeof(PacketHeader) + kPayloadBytes);
    uint64_t video_frames = 0;
    uint64_t total_video_frames = 0;
    uint64_t datagrams_received = 0;
    uint64_t video_objects_completed = 0;
    uint64_t video_decode_failures = 0;
    auto stats_time = std::chrono::steady_clock::now();
    const auto stop_time = options.seconds > 0
        ? std::chrono::steady_clock::now() + std::chrono::seconds(options.seconds)
        : std::chrono::steady_clock::time_point::max();
    std::printf("Listening for UDP on %s:%u...\n", options.address.c_str(), options.port);

    MSG message{};
    while (g_running.load()) {
        if (std::chrono::steady_clock::now() >= stop_time) break;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        bool received_any = false;
        for (;;) {
            const int received = recvfrom(socket_handle, reinterpret_cast<char*>(packet.data()),
                                          static_cast<int>(packet.size()), 0, nullptr, nullptr);
            if (received == SOCKET_ERROR) {
                const int error = WSAGetLastError();
                if (error != WSAEWOULDBLOCK)
                    std::fprintf(stderr, "recvfrom: %d\n", error);
                break;
            }
            received_any = true;
            ++datagrams_received;
            PacketHeader header{};
            if (!validate_packet(packet.data(), received, header)) continue;
            const uint8_t* payload = packet.data() + sizeof(PacketHeader);
            Assembly* assembly = nullptr;
            const PacketKind kind = static_cast<PacketKind>(header.kind);
            if (kind == PacketKind::VideoJpeg) assembly = &video_assembly;
            else if (kind == PacketKind::AudioPcm) assembly = &audio_assembly;
            else if (kind == PacketKind::AudioFormat) assembly = &format_assembly;
            else continue;

            if (!assembly->add(header, payload)) continue;
            if (kind == PacketKind::VideoJpeg) {
                ++video_objects_completed;
                UINT width = 0, height = 0;
                if (codec.decode(assembly->data.data(), assembly->data.size(),
                                 g_video_frame.pixels, width, height)) {
                    g_video_frame.width = width;
                    g_video_frame.height = height;
                    ++video_frames;
                    ++total_video_frames;
                    if (window) InvalidateRect(window, nullptr, FALSE);
                } else {
                    ++video_decode_failures;
                }
            } else if (kind == PacketKind::AudioPcm) {
                if (options.audio) audio.enqueue(assembly->data);
            } else {
                if (options.audio) audio.set_format(assembly->data);
            }
        }
        audio.cleanup();
        const auto current = std::chrono::steady_clock::now();
        if (current - stats_time >= std::chrono::seconds(1)) {
            wchar_t title[160];
            swprintf_s(title, L"GABSERVE - UDP receiver | %llu FPS",
                       static_cast<unsigned long long>(video_frames));
            if (window) SetWindowTextW(window, title);
            video_frames = 0;
            stats_time = current;
        }
        if (!received_any) Sleep(1);
    }
    closesocket(socket_handle);
    if (IsWindow(window)) DestroyWindow(window);
    if (options.headless)
        std::printf("Headless test: datagrams=%llu, complete=%llu, decoded=%llu, errors=%llu.\n",
                    static_cast<unsigned long long>(datagrams_received),
                    static_cast<unsigned long long>(video_objects_completed),
                    static_cast<unsigned long long>(total_video_frames),
                    static_cast<unsigned long long>(video_decode_failures));
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        print_usage();
        return argc < 2 ? 0 : 2;
    }
    SetConsoleCtrlHandler(console_handler, TRUE);
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
        std::fprintf(stderr, "WSAStartup failed.\n");
        return 1;
    }
    const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int result = 1;
    if (SUCCEEDED(com_hr)) {
        result = options.mode == "send"
            ? run_sender(options)
            : run_receiver(options, GetModuleHandleW(nullptr));
        CoUninitialize();
    } else {
        std::fprintf(stderr, "COM init: %s\n", hr_text(com_hr));
    }
    WSACleanup();
    return result;
}
