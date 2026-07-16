#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct H264EncoderConfig {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps = 0;
    uint32_t bitrate = 0;
    bool prefer_hardware = false;
};

class MfH264Encoder {
public:
    MfH264Encoder();
    ~MfH264Encoder();
    MfH264Encoder(const MfH264Encoder&) = delete;
    MfH264Encoder& operator=(const MfH264Encoder&) = delete;

    bool initialize(const H264EncoderConfig& config);
    bool encode_bgr24(const uint8_t* pixels, uint32_t stride,
                      std::vector<uint8_t>& encoded_frame);
    void request_keyframe();
    const std::string& backend_name() const;
    const std::vector<uint8_t>& sequence_header() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class MfH264Decoder {
public:
    MfH264Decoder();
    ~MfH264Decoder();
    MfH264Decoder(const MfH264Decoder&) = delete;
    MfH264Decoder& operator=(const MfH264Decoder&) = delete;

    bool initialize(uint32_t width, uint32_t height, uint32_t fps,
                    const uint8_t* sequence_header, uint32_t sequence_header_size);
    bool decode(const uint8_t* encoded_frame, uint32_t encoded_size,
                std::vector<uint8_t>& bgra_frame);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
