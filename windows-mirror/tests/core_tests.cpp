#include "valeria/BinaryDictionary.hpp"
#include "valeria/CoreMedia.hpp"
#include "valeria/PacketFramer.hpp"
#include "valeria/QuickTimeSession.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace valeria;

#ifndef VALERIA_TEST_FIXTURE_DIR
#error "VALERIA_TEST_FIXTURE_DIR must be defined by CMake"
#endif

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

Bytes section(std::uint32_t magic, Bytes payload) {
    Bytes result;
    appendLe32(result, static_cast<std::uint32_t>(payload.size() + 8));
    appendLe32(result, magic);
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
}

void append(Bytes& destination, const Bytes& source) {
    destination.insert(destination.end(), source.begin(), source.end());
}

Bytes loadFixture(const std::string& name) {
    const std::string path = std::string(VALERIA_TEST_FIXTURE_DIR) + "/" + name;
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open fixture " + path);
    }
    return Bytes(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

Bytes makeAvcc() {
    return {1, 100, 0, 51, 0xFF, 0xE1,
            0, 3, 0x67, 0x64, 0x33,
            1, 0, 2, 0x68, 0xEE};
}

Bytes makeHvcc() {
    Bytes result(23, 0);
    result[0] = 1;
    result[21] = 3;
    result[22] = 3;
    const auto array = [&](std::uint8_t type, Bytes nalu) {
        result.push_back(type);
        result.push_back(0);
        result.push_back(1);
        result.push_back(static_cast<std::uint8_t>(nalu.size() >> 8U));
        result.push_back(static_cast<std::uint8_t>(nalu.size()));
        append(result, nalu);
    };
    array(32, {0x40, 0x01});
    array(33, {0x42, 0x01});
    array(34, {0x44, 0x01});
    return result;
}

Bytes timeBytes(std::int64_t value, std::uint32_t scale = 60000) {
    Bytes result;
    appendLeI64(result, value);
    appendLe32(result, scale);
    appendLe32(result, 1);
    appendLeI64(result, 0);
    return result;
}

Bytes makeVideoSampleBuffer() {
    Bytes fdsc;
    Bytes media;
    appendLe32(media, cm::kVideo);
    append(fdsc, section(cm::kMediaType, media));
    Bytes dimensions;
    appendLe32(dimensions, 828);
    appendLe32(dimensions, 1792);
    append(fdsc, section(cm::kVideoDimensions, dimensions));
    Bytes codec;
    appendLe32(codec, cm::kAvc1);
    append(fdsc, section(cm::kCodec, codec));
    append(fdsc, section(cm::kExtensions, section(cm::kDataValue, makeAvcc())));

    Bytes body;
    append(body, section(cm::kOutputTimestamp, timeBytes(1000)));
    append(body, section(cm::kFormatDescription, fdsc));
    Bytes sample{0, 0, 0, 3, 0x65, 0x88, 0x84};
    append(body, section(cm::kSampleData, sample));
    Bytes count;
    appendLe32(count, 1);
    append(body, section(cm::kSampleCount, count));
    return section(cm::kSampleBuffer, body);
}

Bytes makeAudioSampleBuffer() {
    Bytes fdsc;
    Bytes media;
    appendLe32(media, cm::kSound);
    append(fdsc, section(cm::kMediaType, media));
    Bytes asbd;
    appendLeDouble(asbd, 48000.0);
    appendLe32(asbd, cm::kLpcm);
    appendLe32(asbd, 12);
    appendLe32(asbd, 4);
    appendLe32(asbd, 1);
    appendLe32(asbd, 4);
    appendLe32(asbd, 2);
    appendLe32(asbd, 16);
    appendLe32(asbd, 0);
    append(fdsc, section(cm::kAudioStreamDescription, asbd));

    Bytes body;
    append(body, section(cm::kOutputTimestamp, timeBytes(480, 48000)));
    append(body, section(cm::kFormatDescription, fdsc));
    append(body, section(cm::kSampleData, {1, 2, 3, 4, 5, 6, 7, 8}));
    return section(cm::kSampleBuffer, body);
}

Bytes protocolPacket(std::uint32_t topLevel, std::uint32_t subtype,
                     std::uint64_t clock, std::uint64_t correlation,
                     Bytes payload = {}) {
    Bytes packet;
    appendLe32(packet, static_cast<std::uint32_t>(28 + payload.size()));
    appendLe32(packet, topLevel);
    appendLe64(packet, clock);
    appendLe32(packet, subtype);
    appendLe64(packet, correlation);
    append(packet, payload);
    return packet;
}

Bytes asyncPacket(std::uint32_t subtype, Bytes payload = {}) {
    Bytes packet;
    appendLe32(packet, static_cast<std::uint32_t>(20 + payload.size()));
    appendLe32(packet, wire::kAsync);
    appendLe64(packet, 1);
    appendLe32(packet, subtype);
    append(packet, payload);
    return packet;
}

class FakeTransport final : public IUsbTransport {
public:
    void open(const std::string&) override { open_ = true; }
    void close() noexcept override { open_ = false; }
    bool isOpen() const noexcept override { return open_; }
    const UsbInterfaceInfo& interfaceInfo() const override { return info_; }
    std::size_t readSome(std::uint8_t*, std::size_t,
                         std::chrono::milliseconds) override { return 0; }
    void writeAll(const std::uint8_t* data, std::size_t size,
                  std::chrono::milliseconds) override {
        writes.emplace_back(data, data + size);
    }
    void controlOut(std::uint8_t, std::uint8_t, std::uint16_t, std::uint16_t,
                    const std::uint8_t*, std::uint16_t,
                    std::chrono::milliseconds) override {}
    std::size_t controlIn(std::uint8_t, std::uint8_t, std::uint16_t, std::uint16_t,
                          std::uint8_t*, std::uint16_t,
                          std::chrono::milliseconds) override { return 0; }

    bool open_ = true;
    UsbInterfaceInfo info_;
    std::vector<Bytes> writes;
};

void testFramer() {
    Bytes ping;
    appendLe32(ping, 16);
    appendLe32(ping, wire::kPing);
    appendLe64(ping, 0x0000000100000000ULL);
    Bytes stream = ping;
    append(stream, ping);

    PacketFramer framer;
    check(framer.push(stream.data(), 3).empty(), "framer emitted before length was complete");
    check(framer.push(stream.data() + 3, 10).empty(), "framer emitted a partial packet");
    const auto packets = framer.push(stream.data() + 13, stream.size() - 13);
    check(packets.size() == 2, "framer did not split coalesced packets");
    check(packets[0] == ping && packets[1] == ping, "framer changed packet bytes");
}

void testDictionaryKeySetAndOrder() {
    const Bytes encoded = serializeDictionary(makeVideoRequestDictionary(1920, 1200, true));
    const std::string text(encoded.begin(), encoded.end());
    const auto display = text.find("DisplaySize");
    const auto hevc = text.find("HEVCDecoderSupports444");
    const auto h264 = text.find("H264DecoderSupports444");
    const auto valeria = text.find("Valeria");
    check(display < hevc && hevc < h264 && h264 < valeria,
          "iOS 18 HPD1 key set/order changed");

    const Bytes forceAvc = serializeDictionary(makeVideoRequestDictionary(1920, 1200, false));
    const std::string forceText(forceAvc.begin(), forceAvc.end());
    check(forceText.find("HEVCDecoderSupports444") != std::string::npos,
          "force-H264 removed a key required by iOS 18");
}

void testCodecRecords() {
    const Bytes avcc = makeAvcc();
    const VideoFormat avc = parseAvcDecoderConfiguration(avcc.data(), avcc.size(), 828, 1792);
    check(avc.codec == VideoCodec::H264 && avc.parameterSets.size() == 2,
          "AVC configuration did not expose SPS/PPS");
    check(avc.nalLengthSize == 4 && avc.width == 828 && avc.height == 1792,
          "AVC format metadata was wrong");
    const Bytes h264 = lengthPrefixedToAnnexB(
        reinterpret_cast<const std::uint8_t*>("\0\0\0\2\x65\x01"), 6, 4);
    check(isKeyFrame(VideoCodec::H264, h264), "H.264 IDR was not recognized");
    check(isKeyFrame(VideoCodec::H264, {0, 0, 1, 0x65}),
          "minimal three-byte-start-code H.264 IDR was not recognized");

    const Bytes hvcc = makeHvcc();
    const VideoFormat hevc = parseHevcDecoderConfiguration(hvcc.data(), hvcc.size(), 828, 1792);
    check(hevc.codec == VideoCodec::HEVC && hevc.parameterSets.size() == 3,
          "HEVC configuration did not expose VPS/SPS/PPS");
    const Bytes hevcSample{0, 0, 0, 2, 0x26, 0x01}; // type 19 IRAP
    const Bytes h265 = lengthPrefixedToAnnexB(hevcSample.data(), hevcSample.size(), 4);
    check(isKeyFrame(VideoCodec::HEVC, h265), "HEVC IRAP was not recognized");

    Bytes missingPps = hvcc;
    // Keep only the VPS and SPS arrays.
    missingPps[22] = 2;
    missingPps.resize(missingPps.size() - 7);
    bool rejectedMissingPps = false;
    try {
        parseHevcDecoderConfiguration(missingPps.data(), missingPps.size());
    } catch (const ParseError&) {
        rejectedMissingPps = true;
    }
    check(rejectedMissingPps, "HEVC configuration without PPS was accepted");
}

void testSampleBuffers() {
    const SampleBuffer video = parseSampleBuffer(makeVideoSampleBuffer(), cm::kVideo);
    check(video.format && video.format->video, "video fdsc was not parsed");
    check(video.format->video->width == 828 && video.format->video->height == 1792,
          "video dimensions were wrong");
    check(video.sampleData.size() == 7, "video sdat was wrong");

    const SampleBuffer audio = parseSampleBuffer(makeAudioSampleBuffer(), cm::kSound);
    check(audio.format && audio.format->audio, "audio fdsc was not parsed");
    check(audio.format->audio->formatId == cm::kLpcm &&
              audio.format->audio->channelsPerFrame == 2,
          "LPCM ASBD was wrong");
    check(audio.sampleData.size() == 8, "audio sdat was wrong");
}

void testSessionHandshakeAndCallbacks() {
    FakeTransport transport;
    unsigned formats = 0;
    unsigned frames = 0;
    unsigned audioFormats = 0;
    unsigned audioSamples = 0;
    SessionCallbacks callbacks;
    callbacks.onVideoFormat = [&](const VideoFormat&) { ++formats; };
    callbacks.onVideoSample = [&](const VideoSample& sample) {
        check(sample.keyFrame, "session lost video key-frame metadata");
        ++frames;
    };
    callbacks.onAudioFormat = [&](const AudioFormat&) { ++audioFormats; };
    callbacks.onAudioSample = [&](const AudioSample& sample) {
        check(sample.pcm.size() == 8, "session changed PCM bytes");
        ++audioSamples;
    };
    QuickTimeSession session(transport, SessionOptions{}, callbacks);

    Bytes deviceClock;
    appendLe64(deviceClock, 0x12340000);
    session.handlePacket(protocolPacket(wire::kSync, wire::kCwpa, 1, 9, deviceClock));
    check(transport.writes.size() == 4, "CWPA handshake write count changed");
    check(readLe32(transport.writes[0].data() + 16) == wire::kHpd1 &&
              readLe32(transport.writes[1].data() + 16) == wire::kHpd1 &&
              readLe32(transport.writes[2].data() + 4) == wire::kReply &&
              readLe32(transport.writes[3].data() + 16) == wire::kHpa1,
          "CWPA handshake order changed");

    transport.writes.clear();
    session.handlePacket(protocolPacket(wire::kSync, wire::kCvrp, 1, 10, deviceClock));
    check(transport.writes.size() == 2 &&
              readLe32(transport.writes[0].data() + 16) == wire::kNeed,
          "CVRP did not prime NEED flow");

    transport.writes.clear();
    session.handlePacket(asyncPacket(wire::kFeed, makeVideoSampleBuffer()));
    check(formats == 1 && frames == 1, "FEED did not reach continuous video callbacks");
    check(transport.writes.size() == 1 &&
              readLe32(transport.writes[0].data() + 16) == wire::kNeed,
          "FEED did not request exactly one next frame");

    transport.writes.clear();
    bool malformedRaised = false;
    try {
        session.handlePacket(asyncPacket(wire::kFeed, section(0xDEADBEEFU, {})));
    } catch (const ParseError&) {
        malformedRaised = true;
    }
    check(malformedRaised, "malformed FEED did not report a parse error");
    check(transport.writes.size() == 1 &&
              readLe32(transport.writes[0].data() + 16) == wire::kNeed,
          "malformed FEED did not produce exactly one NEED");

    session.handlePacket(asyncPacket(wire::kEat, makeAudioSampleBuffer()));
    check(audioFormats == 1 && audioSamples == 1,
          "EAT did not reach LPCM callbacks");
}

void testGoldenProtocolFixtures() {
    const Bytes cwpa = loadFixture("cwpa-request1");
    const Bytes afmt = loadFixture("afmt-request");
    const Bytes expectedAfmtReply = loadFixture("afmt-reply");
    const Bytes cvrp = loadFixture("cvrp-request");
    const Bytes feed = loadFixture("asyn-feed");
    Bytes eatPayload = loadFixture("asyn-eat");
    Bytes eat;
    appendLe32(eat, static_cast<std::uint32_t>(eatPayload.size() + 4));
    append(eat, eatPayload);

    // Real packets exercise both fragmented and coalesced WinUSB reads.
    Bytes combined = cwpa;
    append(combined, afmt);
    append(combined, cvrp);
    PacketFramer framer;
    check(framer.push(combined.data(), 5).empty(),
          "golden framer emitted a partial CWPA packet");
    const auto goldenPackets = framer.push(combined.data() + 5, combined.size() - 5);
    check(goldenPackets.size() == 3 && goldenPackets[0] == cwpa &&
              goldenPackets[1] == afmt && goldenPackets[2] == cvrp,
          "golden protocol packets were not framed exactly");

    FakeTransport transport;
    VideoFormat seenVideo;
    AudioFormat seenAudio;
    std::size_t videoBytes = 0;
    std::size_t pcmBytes = 0;
    bool keyFrame = false;
    SessionCallbacks callbacks;
    callbacks.onVideoFormat = [&](const VideoFormat& format) { seenVideo = format; };
    callbacks.onVideoSample = [&](const VideoSample& sample) {
        videoBytes = sample.annexB.size();
        keyFrame = sample.keyFrame;
    };
    callbacks.onAudioFormat = [&](const AudioFormat& format) { seenAudio = format; };
    callbacks.onAudioSample = [&](const AudioSample& sample) { pcmBytes = sample.pcm.size(); };
    QuickTimeSession session(transport, SessionOptions{}, callbacks);

    session.handlePacket(cwpa);
    check(transport.writes.size() == 4, "golden CWPA handshake was not completed");

    transport.writes.clear();
    session.handlePacket(afmt);
    check(transport.writes.size() == 1 && transport.writes.front() == expectedAfmtReply,
          "AFMT reply differs from Daniel Paulus golden bytes");

    transport.writes.clear();
    session.handlePacket(cvrp);
    check(transport.writes.size() == 2 &&
              readLe32(transport.writes.front().data() + 16) == wire::kNeed,
          "golden CVRP did not establish NEED flow");

    transport.writes.clear();
    session.handlePacket(feed);
    check(seenVideo.codec == VideoCodec::H264 && seenVideo.width == 1126 &&
              seenVideo.height == 2436 && seenVideo.parameterSets.size() == 2,
          "golden AVC1 format was parsed incorrectly");
    check(videoBytes == 90750 && keyFrame,
          "golden 90,750-byte AVC access unit/IDR was parsed incorrectly");
    check(transport.writes.size() == 1 &&
              readLe32(transport.writes.front().data() + 16) == wire::kNeed,
          "golden FEED did not produce exactly one NEED");

    session.handlePacket(eat);
    check(seenAudio.formatId == cm::kLpcm && seenAudio.sampleRate == 48000.0 &&
              seenAudio.channelsPerFrame == 2 && seenAudio.bitsPerChannel == 16,
          "golden LPCM format was parsed incorrectly");
    check(pcmBytes == 4096, "golden EAT did not expose 4096 PCM bytes");
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"framer", testFramer},
        {"dictionary", testDictionaryKeySetAndOrder},
        {"codec records", testCodecRecords},
        {"sample buffers", testSampleBuffers},
        {"session", testSessionHandshakeAndCallbacks},
        {"golden protocol fixtures", testGoldenProtocolFixtures},
    };
    unsigned passed = 0;
    for (const auto& test : tests) {
        try {
            test.second();
            std::cout << "[PASS] " << test.first << '\n';
            ++passed;
        } catch (const std::exception& error) {
            std::cerr << "[FAIL] " << test.first << ": " << error.what() << '\n';
            return 1;
        }
    }
    std::cout << passed << " protocol/core tests passed\n";
    return 0;
}
