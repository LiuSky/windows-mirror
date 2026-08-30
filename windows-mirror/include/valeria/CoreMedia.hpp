#pragma once

#include "valeria/ByteIO.hpp"
#include "valeria/Clock.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace valeria {

namespace cm {
constexpr std::uint32_t kSampleBuffer = 0x73627566U;
constexpr std::uint32_t kOutputTimestamp = 0x6F707473U;
constexpr std::uint32_t kTimingArray = 0x73746961U;
constexpr std::uint32_t kSampleData = 0x73646174U;
constexpr std::uint32_t kAttachments = 0x73617474U;
constexpr std::uint32_t kAttachmentArray = 0x73617279U;
constexpr std::uint32_t kSampleSizes = 0x7373697AU;
constexpr std::uint32_t kSampleCount = 0x6E736D70U;
constexpr std::uint32_t kFormatDescription = 0x66647363U;
constexpr std::uint32_t kMediaType = 0x6D646961U;
constexpr std::uint32_t kVideoDimensions = 0x7664696DU;
constexpr std::uint32_t kCodec = 0x636F6463U;
constexpr std::uint32_t kExtensions = 0x6578746EU;
constexpr std::uint32_t kAudioStreamDescription = 0x61736264U;
constexpr std::uint32_t kVideo = 0x76696465U;
constexpr std::uint32_t kSound = 0x736F756EU;
constexpr std::uint32_t kAvc1 = 0x61766331U;
constexpr std::uint32_t kHvc1 = 0x68766331U;
constexpr std::uint32_t kHev1 = 0x68657631U;
constexpr std::uint32_t kLpcm = 0x6C70636DU;
constexpr std::uint32_t kDataValue = 0x64617476U;
} // namespace cm

enum class VideoCodec { Unknown, H264, HEVC };

struct SampleTimingInfo {
    CMTime duration;
    CMTime presentationTimestamp;
    CMTime decodeTimestamp;
};

struct VideoFormat {
    VideoCodec codec = VideoCodec::Unknown;
    std::uint32_t codecFourcc = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint8_t nalLengthSize = 4;
    Bytes decoderConfigurationRecord;
    std::vector<Bytes> parameterSets;
    Bytes annexBParameterSets;
};

struct AudioFormat {
    double sampleRate = 0.0;
    std::uint32_t formatId = 0;
    std::uint32_t formatFlags = 0;
    std::uint32_t bytesPerPacket = 0;
    std::uint32_t framesPerPacket = 0;
    std::uint32_t bytesPerFrame = 0;
    std::uint32_t channelsPerFrame = 0;
    std::uint32_t bitsPerChannel = 0;
    std::uint32_t reserved = 0;
};

struct FormatDescription {
    std::uint32_t mediaType = 0;
    std::optional<VideoFormat> video;
    std::optional<AudioFormat> audio;
};

struct SampleBuffer {
    std::uint32_t mediaType = 0;
    CMTime outputPresentationTimestamp;
    std::vector<SampleTimingInfo> timing;
    std::uint32_t sampleCount = 0;
    std::vector<std::uint32_t> sampleSizes;
    Bytes sampleData;
    std::optional<FormatDescription> format;
};

struct VideoSample {
    VideoCodec codec = VideoCodec::Unknown;
    CMTime presentationTimestamp;
    CMTime decodeTimestamp;
    CMTime duration;
    bool keyFrame = false;
    Bytes annexB;
};

struct AudioSample {
    CMTime presentationTimestamp;
    CMTime duration;
    Bytes pcm;
};

SampleBuffer parseSampleBuffer(const std::uint8_t* data, std::size_t size,
                               std::uint32_t expectedMediaType);
inline SampleBuffer parseSampleBuffer(const Bytes& data, std::uint32_t expectedMediaType) {
    return parseSampleBuffer(data.data(), data.size(), expectedMediaType);
}

VideoFormat parseAvcDecoderConfiguration(const std::uint8_t* data, std::size_t size,
                                         std::uint32_t width = 0,
                                         std::uint32_t height = 0);
VideoFormat parseHevcDecoderConfiguration(const std::uint8_t* data, std::size_t size,
                                          std::uint32_t width = 0,
                                          std::uint32_t height = 0,
                                          std::uint32_t codecFourcc = cm::kHvc1);

Bytes lengthPrefixedToAnnexB(const std::uint8_t* data, std::size_t size,
                             std::uint8_t nalLengthSize);
bool isKeyFrame(VideoCodec codec, const Bytes& annexB);
std::string codecName(VideoCodec codec);

} // namespace valeria
