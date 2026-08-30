#include "valeria/CoreMedia.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <sstream>

namespace valeria {
namespace {

struct ChunkView {
    std::uint32_t length = 0;
    std::uint32_t magic = 0;
    const std::uint8_t* payload = nullptr;
    std::size_t payloadSize = 0;
};

ChunkView chunkAt(const std::uint8_t* data, std::size_t size, std::size_t offset,
                  const char* context) {
    requireRange(size, offset, 8, context);
    const std::uint32_t length = readLe32(data + offset);
    if (length < 8) {
        throw ParseError(std::string(context) + " has a section shorter than 8 bytes");
    }
    requireRange(size, offset, length, context);
    return {length, readLe32(data + offset + 4), data + offset + 8, length - 8U};
}

CMTime parseTime(const std::uint8_t* data, std::size_t size) {
    requireRange(size, 0, 24, "CMTime");
    CMTime result;
    result.value = readLeI64(data);
    result.timescale = readLe32(data + 8);
    result.flags = readLe32(data + 12);
    result.epoch = readLeI64(data + 16);
    return result;
}

void appendStartCode(Bytes& output) {
    static constexpr std::array<std::uint8_t, 4> startCode{0, 0, 0, 1};
    output.insert(output.end(), startCode.begin(), startCode.end());
}

void appendParameterSet(VideoFormat& format, const std::uint8_t* data, std::size_t size) {
    format.parameterSets.emplace_back(data, data + size);
    appendStartCode(format.annexBParameterSets);
    format.annexBParameterSets.insert(format.annexBParameterSets.end(), data, data + size);
}

std::optional<VideoFormat> findDecoderConfiguration(const std::uint8_t* data,
                                                    std::size_t size,
                                                    std::uint32_t codec,
                                                    std::uint32_t width,
                                                    std::uint32_t height) {
    auto tryRecord = [&](const std::uint8_t* record, std::size_t recordSize)
        -> std::optional<VideoFormat> {
        try {
            if (codec == cm::kAvc1) {
                return parseAvcDecoderConfiguration(record, recordSize, width, height);
            }
            if (codec == cm::kHvc1 || codec == cm::kHev1) {
                return parseHevcDecoderConfiguration(record, recordSize, width, height, codec);
            }
        } catch (const ParseError&) {
        }
        return std::nullopt;
    };

    // The record is normally a nested datv value at extension key 49/105.
    // Scan section boundaries defensively instead of assuming those private keys.
    for (std::size_t offset = 0; offset + 8 <= size; ++offset) {
        const std::uint32_t length = readLe32(data + offset);
        if (readLe32(data + offset + 4) != cm::kDataValue || length < 9 ||
            length > size - offset) {
            continue;
        }
        if (auto parsed = tryRecord(data + offset + 8, length - 8U)) {
            return parsed;
        }
    }

    // Capture variants may wrap the record differently; structural validation
    // keeps this byte-wise fallback from accepting random extension data.
    for (std::size_t offset = 0; offset < size; ++offset) {
        if (data[offset] != 1) {
            continue;
        }
        if (auto parsed = tryRecord(data + offset, size - offset)) {
            return parsed;
        }
    }
    return std::nullopt;
}

FormatDescription parseFormatDescription(const std::uint8_t* data, std::size_t size) {
    const ChunkView outer = chunkAt(data, size, 0, "format description");
    if (outer.magic != cm::kFormatDescription) {
        throw ParseError("expected fdsc section");
    }

    std::size_t position = 8;
    const ChunkView media = chunkAt(data, outer.length, position, "fdsc media type");
    if (media.magic != cm::kMediaType || media.length != 12) {
        throw ParseError("invalid fdsc media type section");
    }

    FormatDescription result;
    result.mediaType = readLe32(media.payload);
    position += media.length;

    if (result.mediaType == cm::kSound) {
        const ChunkView asbd = chunkAt(data, outer.length, position, "audio ASBD");
        if (asbd.magic != cm::kAudioStreamDescription || asbd.payloadSize < 40) {
            throw ParseError("invalid audio stream basic description");
        }
        AudioFormat audio;
        audio.sampleRate = readLeDouble(asbd.payload);
        audio.formatId = readLe32(asbd.payload + 8);
        audio.formatFlags = readLe32(asbd.payload + 12);
        audio.bytesPerPacket = readLe32(asbd.payload + 16);
        audio.framesPerPacket = readLe32(asbd.payload + 20);
        audio.bytesPerFrame = readLe32(asbd.payload + 24);
        audio.channelsPerFrame = readLe32(asbd.payload + 28);
        audio.bitsPerChannel = readLe32(asbd.payload + 32);
        audio.reserved = readLe32(asbd.payload + 36);
        result.audio = audio;
        return result;
    }

    if (result.mediaType != cm::kVideo) {
        throw ParseError("unsupported CoreMedia media type " + fourcc(result.mediaType));
    }

    const ChunkView dimensions = chunkAt(data, outer.length, position, "video dimensions");
    if (dimensions.magic != cm::kVideoDimensions || dimensions.length != 16) {
        throw ParseError("invalid video dimensions section");
    }
    const std::uint32_t width = readLe32(dimensions.payload);
    const std::uint32_t height = readLe32(dimensions.payload + 4);
    position += dimensions.length;

    const ChunkView codecChunk = chunkAt(data, outer.length, position, "video codec");
    if (codecChunk.magic != cm::kCodec || codecChunk.length != 12) {
        throw ParseError("invalid video codec section");
    }
    const std::uint32_t codec = readLe32(codecChunk.payload);
    position += codecChunk.length;

    const ChunkView extensions = chunkAt(data, outer.length, position, "video extensions");
    if (extensions.magic != cm::kExtensions) {
        throw ParseError("missing video extension section");
    }

    auto parsed = findDecoderConfiguration(extensions.payload, extensions.payloadSize,
                                           codec, width, height);
    if (!parsed) {
        VideoFormat format;
        format.width = width;
        format.height = height;
        format.codecFourcc = codec;
        format.codec = codec == cm::kAvc1 ? VideoCodec::H264
                     : (codec == cm::kHvc1 || codec == cm::kHev1) ? VideoCodec::HEVC
                                                                  : VideoCodec::Unknown;
        result.video = std::move(format);
    } else {
        result.video = std::move(*parsed);
    }
    return result;
}

} // namespace

VideoFormat parseAvcDecoderConfiguration(const std::uint8_t* data, std::size_t size,
                                         std::uint32_t width, std::uint32_t height) {
    requireRange(size, 0, 7, "AVCDecoderConfigurationRecord");
    if (data[0] != 1) {
        throw ParseError("AVC decoder configuration version is not 1");
    }

    VideoFormat format;
    format.codec = VideoCodec::H264;
    format.codecFourcc = cm::kAvc1;
    format.width = width;
    format.height = height;
    format.nalLengthSize = static_cast<std::uint8_t>((data[4] & 0x03U) + 1U);

    std::size_t position = 6;
    const std::uint8_t spsCount = data[5] & 0x1FU;
    if (spsCount == 0) {
        throw ParseError("AVC configuration contains no SPS");
    }
    for (std::uint8_t i = 0; i < spsCount; ++i) {
        requireRange(size, position, 2, "AVC SPS length");
        const std::uint16_t length = readBe16(data + position);
        position += 2;
        requireRange(size, position, length, "AVC SPS");
        appendParameterSet(format, data + position, length);
        position += length;
    }

    requireRange(size, position, 1, "AVC PPS count");
    const std::uint8_t ppsCount = data[position++];
    if (ppsCount == 0) {
        throw ParseError("AVC configuration contains no PPS");
    }
    for (std::uint8_t i = 0; i < ppsCount; ++i) {
        requireRange(size, position, 2, "AVC PPS length");
        const std::uint16_t length = readBe16(data + position);
        position += 2;
        requireRange(size, position, length, "AVC PPS");
        appendParameterSet(format, data + position, length);
        position += length;
    }
    format.decoderConfigurationRecord.assign(data, data + position);
    return format;
}

VideoFormat parseHevcDecoderConfiguration(const std::uint8_t* data, std::size_t size,
                                          std::uint32_t width, std::uint32_t height,
                                          std::uint32_t codecFourcc) {
    requireRange(size, 0, 23, "HEVCDecoderConfigurationRecord");
    if (data[0] != 1) {
        throw ParseError("HEVC decoder configuration version is not 1");
    }

    VideoFormat format;
    format.codec = VideoCodec::HEVC;
    format.codecFourcc = codecFourcc;
    format.width = width;
    format.height = height;
    format.nalLengthSize = static_cast<std::uint8_t>((data[21] & 0x03U) + 1U);

    std::size_t position = 23;
    const std::uint8_t arrayCount = data[22];
    bool hasSps = false;
    bool hasPps = false;
    for (std::uint8_t array = 0; array < arrayCount; ++array) {
        requireRange(size, position, 3, "HEVC NAL array");
        const std::uint8_t nalType = data[position++] & 0x3FU;
        const std::uint16_t naluCount = readBe16(data + position);
        position += 2;
        for (std::uint16_t i = 0; i < naluCount; ++i) {
            requireRange(size, position, 2, "HEVC NAL length");
            const std::uint16_t length = readBe16(data + position);
            position += 2;
            requireRange(size, position, length, "HEVC parameter NAL");
            if (nalType == 32 || nalType == 33 || nalType == 34) {
                appendParameterSet(format, data + position, length);
                hasSps = hasSps || nalType == 33;
                hasPps = hasPps || nalType == 34;
            }
            position += length;
        }
    }
    if (!hasSps || !hasPps) {
        throw ParseError("HEVC configuration must contain SPS and PPS");
    }
    format.decoderConfigurationRecord.assign(data, data + position);
    return format;
}

SampleBuffer parseSampleBuffer(const std::uint8_t* data, std::size_t size,
                               std::uint32_t expectedMediaType) {
    const ChunkView outer = chunkAt(data, size, 0, "CMSampleBuffer");
    if (outer.magic != cm::kSampleBuffer) {
        throw ParseError("expected sbuf section");
    }

    SampleBuffer result;
    result.mediaType = expectedMediaType;
    std::size_t position = 8;
    while (position < outer.length) {
        const ChunkView chunk = chunkAt(data, outer.length, position, "CMSampleBuffer child");
        switch (chunk.magic) {
        case cm::kOutputTimestamp:
            result.outputPresentationTimestamp = parseTime(chunk.payload, chunk.payloadSize);
            break;
        case cm::kTimingArray:
            if (chunk.payloadSize % 72U != 0) {
                throw ParseError("stia length is not a multiple of CMSampleTimingInfo");
            }
            for (std::size_t offset = 0; offset < chunk.payloadSize; offset += 72) {
                result.timing.push_back({parseTime(chunk.payload + offset, 24),
                                         parseTime(chunk.payload + offset + 24, 24),
                                         parseTime(chunk.payload + offset + 48, 24)});
            }
            break;
        case cm::kSampleData:
            result.sampleData.assign(chunk.payload, chunk.payload + chunk.payloadSize);
            break;
        case cm::kSampleCount:
            if (chunk.payloadSize != 4) {
                throw ParseError("nsmp payload is not four bytes");
            }
            result.sampleCount = readLe32(chunk.payload);
            break;
        case cm::kSampleSizes:
            if (chunk.payloadSize % 4U != 0) {
                throw ParseError("ssiz payload is not a multiple of four");
            }
            for (std::size_t offset = 0; offset < chunk.payloadSize; offset += 4) {
                result.sampleSizes.push_back(readLe32(chunk.payload + offset));
            }
            break;
        case cm::kFormatDescription:
            result.format = parseFormatDescription(data + position, chunk.length);
            if (result.format->mediaType != expectedMediaType) {
                throw ParseError("fdsc media type does not match FEED/EAT packet");
            }
            break;
        case cm::kAttachments:
        case cm::kAttachmentArray:
            break;
        default:
            // Private CoreMedia adds sections across OS releases. Their declared
            // length is enough to preserve framing, so unknown metadata is skipped.
            break;
        }
        position += chunk.length;
    }
    return result;
}

Bytes lengthPrefixedToAnnexB(const std::uint8_t* data, std::size_t size,
                             std::uint8_t nalLengthSize) {
    if (nalLengthSize == 0 || nalLengthSize > 4) {
        throw ParseError("NAL length field must contain 1 to 4 bytes");
    }
    Bytes output;
    output.reserve(size + 16);
    std::size_t position = 0;
    while (position < size) {
        requireRange(size, position, nalLengthSize, "NAL length");
        const std::uint32_t length = readBeN(data + position, nalLengthSize);
        position += nalLengthSize;
        if (length == 0) {
            continue;
        }
        requireRange(size, position, length, "NAL payload");
        appendStartCode(output);
        output.insert(output.end(), data + position, data + position + length);
        position += length;
    }
    return output;
}

bool isKeyFrame(VideoCodec codec, const Bytes& annexB) {
    for (std::size_t i = 0; i < annexB.size(); ++i) {
        std::size_t header = 0;
        if (i + 4 < annexB.size() && annexB[i] == 0 && annexB[i + 1] == 0 &&
                   annexB[i + 2] == 0 && annexB[i + 3] == 1) {
            header = i + 4;
        } else if (i + 3 < annexB.size() && annexB[i] == 0 && annexB[i + 1] == 0 &&
                   annexB[i + 2] == 1) {
            header = i + 3;
        } else {
            continue;
        }
        if (header >= annexB.size()) {
            break;
        }
        if (codec == VideoCodec::H264 && (annexB[header] & 0x1FU) == 5U) {
            return true;
        }
        if (codec == VideoCodec::HEVC) {
            const std::uint8_t type = (annexB[header] >> 1U) & 0x3FU;
            if (type >= 16U && type <= 23U) {
                return true;
            }
        }
    }
    return false;
}

std::string codecName(VideoCodec codec) {
    switch (codec) {
    case VideoCodec::H264:
        return "H.264";
    case VideoCodec::HEVC:
        return "HEVC";
    default:
        return "unknown";
    }
}

} // namespace valeria
